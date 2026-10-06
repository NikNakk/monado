// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS PS VR2 display target using IOSurface-backed Vulkan images and Metal presentation.
 * @ingroup comp_main
 */

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "main/comp_window.h"
#include "main/comp_macos_frontend.h"
#include "main/comp_macos_present_copy.h"
#include "main/comp_window_macos_trace_buffer.h"
#include "xrt/xrt_frame.h"
#include "util/u_debug.h"
#include "util/u_timing_trace.h"
#include "util/u_frame_share.h"
#include "util/u_handles.h"
#include "util/u_macos_hosted_client.h"
#include "main/comp_window_macos_hosted.h"
#include "util/u_misc.h"
#include "util/u_pacing.h"
#include "util/u_thread_priority.h"
#include "vk/vk_image_allocator.h"
#include "vk/vk_compositor_flags.h"

#include <dispatch/dispatch.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MACOS_TARGET_IMAGE_COUNT 3
#define MACOS_PASSTHROUGH_WIDTH 1024
#define MACOS_PASSTHROUGH_HEIGHT 1016
#define MACOS_PASSTHROUGH_MAP_SIZE 512

struct macos_present_job
{
	uint64_t frame_id;
	uint64_t enqueue_ns;
	uint64_t image_reuse_wait_ns;
	uint32_t image_index;
	uint64_t timeline_value;
	int64_t desired_present_time_ns;
	int64_t present_slop_ns;
	struct vk_bundle_queue *present_queue;
	bool passthrough_active;
	bool passthrough_has_application_layers;
	/*
	 * Set for present_external: a finished image, retained, presented instead
	 * of metal_images[image_index]. It needs no render-complete wait, and
	 * external_release runs once the presenter has finished with it.
	 */
	id<MTLTexture> external_texture;
	void (*external_release)(void *data);
	void *external_release_data;
};

/*
 * Presented callbacks are not guaranteed to arrive promptly during layer/display
 * teardown. Keep the callback-owned trace/feedback state independent of cwm so
 * destroy never has to wait forever merely to prevent a use-after-free.
 */
struct macos_presented_state
{
	atomic_uint_fast64_t ref_count;
	atomic_int_fast64_t latest_observed_present_offset_ns;
	atomic_uint_fast64_t present_offset_sample_serial;
	FILE *trace_presented;
};

static struct macos_presented_state *
macos_presented_state_create(FILE *trace_presented)
{
	struct macos_presented_state *state = calloc(1, sizeof(*state));
	if (state == NULL) {
		return NULL;
	}
	atomic_init(&state->ref_count, 1);
	atomic_init(&state->latest_observed_present_offset_ns, 0);
	atomic_init(&state->present_offset_sample_serial, 0);
	state->trace_presented = trace_presented;
	return state;
}

static void
macos_presented_state_retain(struct macos_presented_state *state)
{
	atomic_fetch_add_explicit(&state->ref_count, 1, memory_order_relaxed);
}

static void
macos_presented_state_release(struct macos_presented_state *state)
{
	if (state == NULL) {
		return;
	}
	if (atomic_fetch_sub_explicit(&state->ref_count, 1, memory_order_acq_rel) != 1) {
		return;
	}
	if (state->trace_presented != NULL) {
		macos_trace_buffered_fflush(state->trace_presented);
		fclose(state->trace_presented);
		state->trace_presented = NULL;
	}
	free(state);
}

DEBUG_GET_ONCE_BOOL_OPTION(macos_psvr2_timing_trace, "PSVR2_TIMING_TRACE", false)
DEBUG_GET_ONCE_NUM_OPTION(macos_present_min_lead_us, "XRT_MACOS_PRESENT_MIN_LEAD_US", 2000)
DEBUG_GET_ONCE_NUM_OPTION(macos_present_prelatch_us, "XRT_MACOS_PRESENT_PRELATCH_US", 2000)
DEBUG_GET_ONCE_BOOL_OPTION(macos_drawable_slot, "XRT_MACOS_DRAWABLE_SLOT", true)
DEBUG_GET_ONCE_NUM_OPTION(macos_refresh_rate_hz, "XRT_MACOS_REFRESH_RATE_HZ", 0)
// Vblank timing source: "ca" (default, macOS 14+) or "cv" (legacy fallback).
DEBUG_GET_ONCE_OPTION(macos_display_link, "XRT_MACOS_DISPLAY_LINK", "ca")
DEBUG_GET_ONCE_NUM_OPTION(macos_passthrough_fov_deg, "XRT_MACOS_PASSTHROUGH_FOV_DEG", 150)
DEBUG_GET_ONCE_NUM_OPTION(macos_passthrough_convergence_milli, "XRT_MACOS_PASSTHROUGH_CONVERGENCE_MILLI", 100)
DEBUG_GET_ONCE_NUM_OPTION(macos_passthrough_brightness_percent, "XRT_MACOS_PASSTHROUGH_BRIGHTNESS_PERCENT", 160)

/*
 * CVDisplayLink is deprecated from macOS 15 in favour of CADisplayLink
 * (-[NSScreen displayLinkWithTarget:selector:]). Pacing and present timing
 * were compared on the PS VR2 in native and heavy Unreal/Game Mode runs.
 * CADisplayLink is the default; XRT_MACOS_DISPLAY_LINK=cv retains the legacy
 * path, also used when CADisplayLink is unavailable.
 */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct comp_window_macos;

struct macos_passthrough_sink
{
	struct xrt_frame_sink base;
	struct comp_window_macos *cwm;
	uint32_t eye;
};

struct comp_window_macos
{
	struct comp_target_swapchain base;
	struct comp_macos_frontend *frontend;
	struct u_macos_hosted_client *hosted_client;
	//! Borrowed from the frontend.
	CAMetalLayer *metal_layer;
	CGDirectDisplayID display_id;
	char display_name[128];
	id<MTLCommandQueue> present_queue;
	struct comp_macos_present_copy *present_copy;
	id<MTLTexture> metal_images[MACOS_TARGET_IMAGE_COUNT];

	/* Camera-backed XR_FB_passthrough resources. */
	id<MTLTexture> passthrough_camera_textures[2];
	id<MTLTexture> passthrough_uv_maps[2];
	id<MTLRenderPipelineState> passthrough_pipeline;
	struct macos_passthrough_sink passthrough_sinks[2];
	struct xrt_frame *passthrough_frames[2];
	int64_t passthrough_uploaded_timestamp[2];
	pthread_mutex_t passthrough_mutex;
	atomic_bool passthrough_shutdown;
	bool passthrough_sinks_attached;
	id<MTLSharedEvent> render_complete_event;
	atomic_bool image_in_flight[MACOS_TARGET_IMAGE_COUNT];
	dispatch_group_t present_command_group;
	dispatch_queue_t present_worker_queue;
	dispatch_group_t present_worker_group;
	pthread_mutex_t present_worker_mutex;
	struct macos_present_job pending_present_job;
	bool pending_present_job_valid;
	bool present_worker_scheduled;
	bool drawable_slot_acquire_scheduled;
	bool present_worker_shutdown;
	bool present_worker_enabled;
	bool drawable_slot_enabled;
	id<CAMetalDrawable> prefetched_drawable;
	uint64_t prefetched_drawable_timeline_value;
	uint64_t prefetched_drawable_begin_ns;
	uint64_t prefetched_drawable_end_ns;
	struct macos_presented_state *presented_state;
	uint64_t last_image_acquire_wait_ns;
	xrt_graphics_buffer_handle_t io_surfaces[MACOS_TARGET_IMAGE_COUNT];
	struct vk_image_collection vkic;
	CVDisplayLinkRef display_link;
	//! MonadoCADisplayLinkSource, used instead of display_link with XRT_MACOS_DISPLAY_LINK=ca.
	void *ca_display_link;
	mach_timebase_info_data_t mach_timebase;
	atomic_uint_fast64_t latest_vblank_ns;
	atomic_uint_fast64_t latest_displaylink_now_host_ns;
	atomic_uint_fast64_t latest_displaylink_output_host_ns;
	atomic_uint_fast64_t latest_displaylink_now_ns;
	atomic_uint_fast64_t latest_displaylink_output_ns;
	atomic_uint_fast64_t latest_displaylink_callback_ns;
	//! CADisplayLink's timestamp-to-target interval, consumed on the compositor thread.
	atomic_int_fast64_t latest_displaylink_period_ns;
	atomic_int_fast64_t host_to_monotonic_offset_ns;
	atomic_int_fast64_t latest_observed_present_offset_ns;
	atomic_uint_fast64_t present_offset_sample_serial;
	uint64_t consumed_present_offset_sample_serial;
	int64_t calibrated_present_offset_ns;
	uint32_t present_offset_sample_count;
	uint64_t last_vblank_ns;
	uint64_t trace_frame_id;
	uint64_t cadence_sample_count;
	uint64_t cadence_total_ns;
	uint64_t cadence_min_ns;
	uint64_t cadence_max_ns;
	uint64_t last_present_ns;
	uint64_t present_sample_count;
	uint64_t present_total_ns;
	uint64_t present_min_ns;
	uint64_t present_max_ns;
	uint64_t present_missed_intervals;
	uint64_t present_vk_wait_total_ns;
	uint64_t present_drawable_wait_total_ns;
	atomic_int_fast64_t display_period_ns;
	uint32_t pixel_width;
	uint32_t pixel_height;
	uint32_t next_image;
	bool logged_layer_state;
	bool logged_external_present;
	FILE *trace_present;
	FILE *trace_presented;
	FILE *trace_present_complete;
	FILE *trace_present_scheduled;
	FILE *trace_present_worker;
	FILE *trace_drawable_prefetch;
	FILE *trace_vblank;
	dispatch_group_t trace_present_group;
	uint64_t trace_present_rows;
	uint64_t trace_present_worker_rows;
	uint64_t trace_drawable_prefetch_rows;
	uint64_t trace_vblank_rows;
	uint64_t worker_jobs_enqueued;
	uint64_t worker_jobs_submitted;
	uint64_t worker_jobs_superseded;
	uint64_t worker_drawable_half_refresh_stalls;
	uint64_t worker_drawable_stalls;
	uint64_t worker_queue_delay_total_ns;
	uint64_t worker_queue_delay_max_ns;
};

static FILE *
macos_timing_trace_open_file(const char *suffix, const char *header)
{
	FILE *file = u_timing_trace_open(suffix, 64 * 1024);
	if (file == NULL) {
		return NULL;
	}
	fputs(header, file);
	fputc('\n', file);
	macos_trace_buffered_fflush(file);
	return file;
}

static void
macos_timing_trace_open(struct comp_window_macos *cwm)
{
	if (!debug_get_bool_option_macos_psvr2_timing_trace()) {
		return;
	}
	cwm->trace_present = macos_timing_trace_open_file(
	    "present",
	    "frame_id,host_call_ns,desired_present_ns,desired_minus_call_ns,target_output_ns,target_minus_desired_ns,"
	    "metal_request_ns,metal_request_minus_target_ns,metal_request_minus_call_ns,scheduled_present_host_s,"
	    "present_slop_ns,image_index,"
	    "timeline_value,wait_mode,after_vk_wait_ns,"
	    "after_drawable_ns,before_present_call_ns,after_present_call_ns,after_commit_ns,after_metal_wait_ns,"
	    "latest_displaylink_output_ns,gpu_start_time_s,gpu_end_time_s,"
	    "async_present,shared_event_wait,image_reuse_wait_ns");
	cwm->trace_presented = macos_timing_trace_open_file(
	    "presented",
	    "frame_id,presented_handler_ns,desired_present_ns,target_output_ns,presented_time_host_s,"
	    "presented_monotonic_ns,presented_minus_desired_ns,presented_minus_target_ns,observed_present_offset_ns,"
	    "present_queue_depth");
	if (cwm->trace_presented != NULL) {
		cwm->presented_state = macos_presented_state_create(cwm->trace_presented);
		if (cwm->presented_state == NULL) {
			fclose(cwm->trace_presented);
			cwm->trace_presented = NULL;
		}
	}
	cwm->trace_present_complete = macos_timing_trace_open_file(
	    "present_complete",
	    "frame_id,completion_handler_ns,image_index,timeline_value,status,commit_to_completion_ns,"
	    "gpu_start_time_s,gpu_end_time_s,shared_event_wait");
	cwm->trace_present_scheduled = macos_timing_trace_open_file(
	    "present_scheduled",
	    "frame_id,scheduled_callback_ns,observer_registered_ns,image_index,timeline_value,status,"
	    "minimum_duration_us,presents_with_transaction");
	cwm->trace_present_worker = NULL;
	if (cwm->present_worker_enabled) {
		cwm->trace_present_worker = macos_timing_trace_open_file(
		    "present_worker",
		    "event,frame_id,event_ns,enqueue_ns,handoff_return_ns,worker_start_ns,"
		    "next_drawable_begin_ns,next_drawable_end_ns,metal_commit_ns,worker_delay_ns,"
		    "drawable_wait_ns,image_index,timeline_value,queue_depth,shared_event_wait,"
		    "newer_pending,pending_frame_id,pending_timeline_value");
	}
	cwm->trace_drawable_prefetch = macos_timing_trace_open_file(
	    "drawable_prefetch",
	    "event,timeline_value,event_ns,next_drawable_begin_ns,next_drawable_end_ns,drawable_wait_ns");
	cwm->trace_vblank = macos_timing_trace_open_file(
	    "vblank",
	    "host_consumed_ns,displaylink_callback_ns,displaylink_now_host_ns,displaylink_output_host_ns,"
	    "host_to_monotonic_offset_ns,displaylink_now_ns,displaylink_output_ns,derived_last_vblank_ns,"
	    "output_minus_now_ns,callback_minus_output_ns,callback_minus_vblank_ns,interval_from_previous_vblank_ns,"
	    "display_period_ns");
}

static void
macos_timing_trace_close(struct comp_window_macos *cwm)
{
	if (cwm->presented_state != NULL) {
		/* Ownership of trace_presented belongs to the ref-counted callback state. */
		cwm->trace_presented = NULL;
		macos_presented_state_release(cwm->presented_state);
		cwm->presented_state = NULL;
	}
	if (cwm->trace_present_group != NULL) {
		dispatch_group_wait(cwm->trace_present_group, DISPATCH_TIME_FOREVER);
		cwm->trace_present_group = NULL;
	}
	if (cwm->trace_present != NULL) {
		macos_trace_buffered_fflush(cwm->trace_present);
		fclose(cwm->trace_present);
		cwm->trace_present = NULL;
	}
	if (cwm->trace_presented != NULL) {
		macos_trace_buffered_fflush(cwm->trace_presented);
		fclose(cwm->trace_presented);
		cwm->trace_presented = NULL;
	}
	if (cwm->trace_present_complete != NULL) {
		macos_trace_buffered_fflush(cwm->trace_present_complete);
		fclose(cwm->trace_present_complete);
		cwm->trace_present_complete = NULL;
	}
	if (cwm->trace_present_scheduled != NULL) {
		macos_trace_buffered_fflush(cwm->trace_present_scheduled);
		fclose(cwm->trace_present_scheduled);
		cwm->trace_present_scheduled = NULL;
	}
	if (cwm->trace_present_worker != NULL) {
		macos_trace_buffered_fflush(cwm->trace_present_worker);
		fclose(cwm->trace_present_worker);
		cwm->trace_present_worker = NULL;
	}
	if (cwm->trace_drawable_prefetch != NULL) {
		macos_trace_buffered_fflush(cwm->trace_drawable_prefetch);
		fclose(cwm->trace_drawable_prefetch);
		cwm->trace_drawable_prefetch = NULL;
	}
	if (cwm->trace_vblank != NULL) {
		macos_trace_buffered_fflush(cwm->trace_vblank);
		fclose(cwm->trace_vblank);
		cwm->trace_vblank = NULL;
	}
}

static uint64_t
host_time_to_ns(struct comp_window_macos *cwm, uint64_t host_time)
{
	return (uint64_t)(((__uint128_t)host_time * cwm->mach_timebase.numer) / cwm->mach_timebase.denom);
}

static int64_t
refresh_host_to_monotonic_offset_ns(struct comp_window_macos *cwm)
{
	uint64_t host_ns = host_time_to_ns(cwm, CVGetCurrentHostTime());
	int64_t monotonic_ns = os_monotonic_get_ns();
	int64_t offset_ns = monotonic_ns - (int64_t)host_ns;
	atomic_store_explicit(&cwm->host_to_monotonic_offset_ns, offset_ns, memory_order_release);
	return offset_ns;
}

static uint64_t
host_ns_to_monotonic_ns(uint64_t host_ns, int64_t offset_ns)
{
	int64_t monotonic_ns = (int64_t)host_ns + offset_ns;
	return monotonic_ns > 0 ? (uint64_t)monotonic_ns : 0;
}

static double
monotonic_ns_to_host_seconds(struct comp_window_macos *cwm, int64_t monotonic_ns)
{
	int64_t offset_ns = atomic_load_explicit(&cwm->host_to_monotonic_offset_ns, memory_order_acquire);
	int64_t host_ns = monotonic_ns - offset_ns;
	return host_ns > 0 ? (double)host_ns / (double)U_TIME_1S_IN_NS : 0.0;
}

static uint64_t
derive_last_vblank_ns(struct comp_window_macos *cwm, uint64_t output_ns, uint64_t now_ns)
{
	if (output_ns == 0 || now_ns == 0 || cwm->display_period_ns <= 0) {
		return now_ns;
	}

	uint64_t period_ns = (uint64_t)cwm->display_period_ns;
	if (output_ns > now_ns) {
		uint64_t delta_ns = output_ns - now_ns;
		uint64_t periods = (delta_ns + period_ns - 1) / period_ns;
		uint64_t adjustment = periods * period_ns;
		return output_ns > adjustment ? output_ns - adjustment : 0;
	}

	uint64_t periods = (now_ns - output_ns) / period_ns;
	return output_ns + periods * period_ns;
}

/*!
 * One vblank from whichever display link is in use. Host times are in
 * nanoseconds of mach_absolute_time, zero if unknown. For CA, @p now_host_ns
 * identifies the previous refresh (@p now_is_vblank); CV supplies its current
 * host time instead. @p output_host_ns identifies the upcoming output.
 */
static void
macos_display_link_tick(struct comp_window_macos *cwm,
                        uint64_t now_host_ns,
                        uint64_t output_host_ns,
                        bool now_is_vblank)
{
	int64_t offset_ns = refresh_host_to_monotonic_offset_ns(cwm);
	uint64_t callback_ns = (uint64_t)os_monotonic_get_ns();
	uint64_t now_ns = callback_ns;
	uint64_t output_ns = 0;

	if (now_host_ns != 0) {
		now_ns = host_ns_to_monotonic_ns(now_host_ns, offset_ns);
		atomic_store_explicit(&cwm->latest_displaylink_now_host_ns, now_host_ns, memory_order_release);
		atomic_store_explicit(&cwm->latest_displaylink_now_ns, now_ns, memory_order_release);
	}
	if (output_host_ns != 0) {
		output_ns = host_ns_to_monotonic_ns(output_host_ns, offset_ns);
		atomic_store_explicit(&cwm->latest_displaylink_output_host_ns, output_host_ns, memory_order_release);
		atomic_store_explicit(&cwm->latest_displaylink_output_ns, output_ns, memory_order_release);
	}

	if (output_ns != 0) {
		// CA's timestamp already identifies the previous refresh. Projecting its target
		// with a rounded mode period (120 Hz vs actual 119.88 Hz) subtracts two periods.
		uint64_t last_vblank_ns =
		    now_is_vblank && now_host_ns != 0 ? now_ns : derive_last_vblank_ns(cwm, output_ns, now_ns);
		atomic_store_explicit(&cwm->latest_vblank_ns, last_vblank_ns, memory_order_release);
	}
	atomic_store_explicit(&cwm->latest_displaylink_callback_ns, callback_ns, memory_order_release);
}

static CVReturn
display_link_callback(CVDisplayLinkRef display_link,
                      const CVTimeStamp *in_now,
                      const CVTimeStamp *in_output_time,
                      CVOptionFlags flags_in,
                      CVOptionFlags *flags_out,
                      void *context)
{
	(void)display_link;
	(void)flags_in;
	(void)flags_out;
	struct comp_window_macos *cwm = context;

	uint64_t now_host_ns = 0;
	uint64_t output_host_ns = 0;
	if ((in_now->flags & kCVTimeStampHostTimeValid) != 0) {
		now_host_ns = host_time_to_ns(cwm, in_now->hostTime);
	}
	if ((in_output_time->flags & kCVTimeStampHostTimeValid) != 0) {
		output_host_ns = host_time_to_ns(cwm, in_output_time->hostTime);
	}
	macos_display_link_tick(cwm, now_host_ns, output_host_ns, false);
	return kCVReturnSuccess;
}



/*
 * CADisplayLink vblank source, selected with XRT_MACOS_DISPLAY_LINK=ca. It is
 * Apple's replacement for the deprecated CVDisplayLink. It only supplies
 * timestamps: drawables and presentation are untouched. CADisplayLink fires on
 * a run loop, so it gets a thread of its own, away from the main thread's
 * AppKit work.
 */
API_AVAILABLE(macos(14.0))
@interface MonadoCADisplayLinkSource : NSObject {
	struct comp_window_macos *_cwm;
	CGDirectDisplayID _displayID;
	NSThread *_thread;
	CADisplayLink *_link;
	dispatch_semaphore_t _ready;
	dispatch_semaphore_t _finished;
	BOOL _created;
	BOOL _stop;
	uint64_t _tickCount;
	FILE *_callbackTrace;
	uint64_t _traceRows;
	uint64_t _previousCallbackNS;
	uint64_t _previousTimestampNS;
}
- (instancetype)initWithWindow:(struct comp_window_macos *)cwm displayID:(CGDirectDisplayID)displayID;
- (BOOL)isRunning;
- (void)setRunning:(BOOL)running;
- (void)shutdown;
@end

@implementation MonadoCADisplayLinkSource

- (instancetype)initWithWindow:(struct comp_window_macos *)cwm displayID:(CGDirectDisplayID)displayID
{
	self = [super init];
	if (self == nil) {
		return nil;
	}

	_cwm = cwm;
	_displayID = displayID;
	_ready = dispatch_semaphore_create(0);
	_finished = dispatch_semaphore_create(0);
	_thread = [[NSThread alloc] initWithTarget:self selector:@selector(threadMain) object:nil];
	_thread.name = @"Monado CADisplayLink";
	_thread.qualityOfService = NSQualityOfServiceUserInteractive;
	[_thread start];
	dispatch_semaphore_wait(_ready, DISPATCH_TIME_FOREVER);

	if (!_created) {
		dispatch_semaphore_wait(_finished, DISPATCH_TIME_FOREVER);
		[self release];
		return nil;
	}
	return self;
}

- (void)dealloc
{
	[_thread release];
	[_link release];
	dispatch_release(_ready);
	dispatch_release(_finished);
	[super dealloc];
}

- (void)threadMain
{
	@autoreleasepool {
		for (NSScreen *screen in [NSScreen screens]) {
			if (comp_macos_frontend_display_id(screen) == _displayID) {
				_link = [[screen displayLinkWithTarget:self selector:@selector(tick:)] retain];
				break;
			}
		}
		if (_link != nil) {
			// Paused until the compositor starts it, like a stopped CVDisplayLink.
			_link.paused = YES;
			[_link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
			_created = YES;
		}
		if (_created && u_timing_trace_enabled()) {
			_callbackTrace = u_timing_trace_open("ca_callback", 128 * 1024);
			if (_callbackTrace != NULL) {
				fputs(
				    "sample,callback_entry_ns,callback_exit_ns,timestamp_ns,target_timestamp_ns,"
				    "callback_interval_ns,timestamp_interval_ns,frame_executed,frame_begin_ns,frame_"
				    "end_ns,"
				    "callback_minus_timestamp_ns,remaining_to_target_ns\n",
				    _callbackTrace);
			}
		}
		dispatch_semaphore_signal(_ready);

		while (_created && !_stop) {
			@autoreleasepool {
				[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
				                         beforeDate:[NSDate distantFuture]];
			}
		}

		[_link invalidate];
		if (_callbackTrace != NULL) {
			fflush(_callbackTrace);
			fclose(_callbackTrace);
			_callbackTrace = NULL;
		}
	}
	dispatch_semaphore_signal(_finished);
}

- (void)tick:(CADisplayLink *)link
{
	uint64_t entry_ns = os_monotonic_get_ns();
	_tickCount++;

	// Both are in seconds of mach_absolute_time, like CACurrentMediaTime().
	const CFTimeInterval now_s = link.timestamp;
	const CFTimeInterval output_s = link.targetTimestamp;
	const uint64_t now_ns = now_s > 0.0 ? (uint64_t)llround(now_s * (double)U_TIME_1S_IN_NS) : 0;
	const uint64_t output_ns = output_s > 0.0 ? (uint64_t)llround(output_s * (double)U_TIME_1S_IN_NS) : 0;
	const int64_t nominal_period_ns = _cwm->display_period_ns;
	if (now_ns != 0 && output_ns > now_ns && nominal_period_ns > 0) {
		const uint64_t period_ns = output_ns - now_ns;
		// A callback rate divisor or a discontinuity must not change the physical
		// refresh period. Accept only small corrections to the selected display mode.
		if (period_ns > (uint64_t)nominal_period_ns * 9 / 10 &&
		    period_ns < (uint64_t)nominal_period_ns * 11 / 10) {
			atomic_store_explicit(&_cwm->latest_displaylink_period_ns, (int64_t)period_ns,
			                      memory_order_release);
		}
	}
	macos_display_link_tick(_cwm, now_ns, output_ns, true);

	uint64_t exit_ns = os_monotonic_get_ns();
	if (_callbackTrace != NULL) {
		// Host timestamps and callback times use different clock domains: map explicitly.
		int64_t offset_ns = atomic_load_explicit(&_cwm->host_to_monotonic_offset_ns, memory_order_acquire);
		uint64_t timestamp_ns = host_ns_to_monotonic_ns(now_ns, offset_ns);
		uint64_t target_ns = host_ns_to_monotonic_ns(output_ns, offset_ns);
		fprintf(_callbackTrace, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu,%lld,%lld\n",
		        (unsigned long long)++_traceRows, (unsigned long long)entry_ns, (unsigned long long)exit_ns,
		        (unsigned long long)timestamp_ns, (unsigned long long)target_ns,
		        (unsigned long long)(_previousCallbackNS != 0 ? entry_ns - _previousCallbackNS : 0),
		        (unsigned long long)(_previousTimestampNS != 0 ? timestamp_ns - _previousTimestampNS : 0), 0u,
		        0ull, 0ull, (long long)((int64_t)entry_ns - (int64_t)timestamp_ns),
		        (long long)((int64_t)target_ns - (int64_t)exit_ns));
		if (!u_timing_trace_fully_buffered() && (_traceRows % 256) == 0) {
			fflush(_callbackTrace);
		}
	}
	_previousCallbackNS = entry_ns;
	_previousTimestampNS = host_ns_to_monotonic_ns(
	    now_ns, atomic_load_explicit(&_cwm->host_to_monotonic_offset_ns, memory_order_acquire));
}

- (void)applyPaused:(NSNumber *)paused
{
	_link.paused = paused.boolValue;
	if (!paused.boolValue) {
		// Unlike CVDisplayLink this follows the real display, so say so if nothing arrives.
		_tickCount = 0;
		[self performSelector:@selector(checkTicking) withObject:nil afterDelay:1.0];
	}
}

- (void)checkTicking
{
	if (_tickCount == 0 && !_link.paused && !_stop) {
		U_LOG_W(
		    "CADisplayLink has not fired in its first second (is the display asleep?); pacing is "
		    "running on estimates. Set XRT_MACOS_DISPLAY_LINK=cv to use CVDisplayLink.");
	} else if (!_stop) {
		U_LOG_I("CADisplayLink fired %llu times in its first second", (unsigned long long)_tickCount);
	}
}

- (void)applyStop
{
	_stop = YES;
}

- (BOOL)isRunning
{
	return _link != nil && !_link.paused;
}

- (void)setRunning:(BOOL)running
{
	// The link belongs to its thread's run loop, so change it there.
	[self performSelector:@selector(applyPaused:)
	             onThread:_thread
	           withObject:[NSNumber numberWithBool:!running]
	        waitUntilDone:YES];
}

- (void)shutdown
{
	// Performing the selector also wakes the run loop so that it sees _stop.
	[self performSelector:@selector(applyStop) onThread:_thread withObject:nil waitUntilDone:NO];
	dispatch_semaphore_wait(_finished, DISPATCH_TIME_FOREVER);
}

@end

static bool
macos_display_link_wants_ca(void)
{
	const char *value = debug_get_option_macos_display_link();
	return value != NULL && strcasecmp(value, "ca") == 0;
}

//! Nominal period of the display's current mode, zero if unknown.
static int64_t
macos_display_mode_period_ns(CGDirectDisplayID display_id)
{
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display_id);
	if (mode == NULL) {
		return 0;
	}
	const double refresh_hz = CGDisplayModeGetRefreshRate(mode);
	CGDisplayModeRelease(mode);
	return refresh_hz > 1.0 ? (int64_t)((double)U_TIME_1S_IN_NS / refresh_hz) : 0;
}

static bool
macos_display_link_exists(struct comp_window_macos *cwm)
{
	return cwm->display_link != NULL || cwm->ca_display_link != NULL;
}

static void
macos_display_link_stop(struct comp_window_macos *cwm)
{
	if (cwm->display_link != NULL) {
		CVDisplayLinkStop(cwm->display_link);
	}
	if (cwm->ca_display_link != NULL) {
		if (@available(macOS 14.0, *)) {
			[(MonadoCADisplayLinkSource *)cwm->ca_display_link setRunning:NO];
		}
	}
}

static void
macos_display_link_destroy(struct comp_window_macos *cwm)
{
	if (cwm->display_link != NULL) {
		CVDisplayLinkStop(cwm->display_link);
		CVDisplayLinkRelease(cwm->display_link);
		cwm->display_link = NULL;
	}
	if (cwm->ca_display_link != NULL) {
		if (@available(macOS 14.0, *)) {
			MonadoCADisplayLinkSource *source = (MonadoCADisplayLinkSource *)cwm->ca_display_link;
			[source shutdown];
			[source release];
		}
		cwm->ca_display_link = NULL;
	}
}

/*!
 * Create the vblank source for @p display_id, stopped, replacing any existing
 * one. Sets display_period_ns when the source knows the period.
 */
static bool
macos_display_link_create(struct comp_window_macos *cwm, CGDirectDisplayID display_id)
{
	macos_display_link_destroy(cwm);

	if (macos_display_link_wants_ca()) {
		if (@available(macOS 14.0, *)) {
			MonadoCADisplayLinkSource *source =
			    [[MonadoCADisplayLinkSource alloc] initWithWindow:cwm displayID:display_id];
			if (source != nil) {
				cwm->ca_display_link = source;
				const int64_t period_ns = macos_display_mode_period_ns(display_id);
				if (period_ns > 0) {
					cwm->display_period_ns = period_ns;
				}
				U_LOG_I("macOS vblank source: CADisplayLink (XRT_MACOS_DISPLAY_LINK=ca)");
				return true;
			}
		}
		U_LOG_W(
		    "XRT_MACOS_DISPLAY_LINK=ca: CADisplayLink is unavailable for this display; using CVDisplayLink");
	}

	CVReturn cvret = CVDisplayLinkCreateWithCGDisplay(display_id, &cwm->display_link);
	if (cvret == kCVReturnSuccess) {
		cvret = CVDisplayLinkSetOutputCallback(cwm->display_link, display_link_callback, cwm);
	}
	if (cvret != kCVReturnSuccess) {
		if (cwm->display_link != NULL) {
			CVDisplayLinkRelease(cwm->display_link);
			cwm->display_link = NULL;
		}
		U_LOG_W("Could not create a CVDisplayLink for the display (%d)", cvret);
		return false;
	}

	CVTime period = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(cwm->display_link);
	if ((period.flags & kCVTimeIsIndefinite) == 0 && period.timeValue > 0 && period.timeScale > 0) {
		cwm->display_period_ns = (int64_t)(((__int128)period.timeValue * U_TIME_1S_IN_NS) / period.timeScale);
	}
	return true;
}

static bool
macos_display_link_is_running(struct comp_window_macos *cwm)
{
	if (cwm->display_link != NULL) {
		return CVDisplayLinkIsRunning(cwm->display_link);
	}
	if (cwm->ca_display_link != NULL) {
		if (@available(macOS 14.0, *)) {
			return [(MonadoCADisplayLinkSource *)cwm->ca_display_link isRunning];
		}
	}
	return false;
}

static bool
macos_display_link_start(struct comp_window_macos *cwm)
{
	if (cwm->display_link != NULL) {
		return CVDisplayLinkStart(cwm->display_link) == kCVReturnSuccess;
	}
	if (cwm->ca_display_link != NULL) {
		if (@available(macOS 14.0, *)) {
			[(MonadoCADisplayLinkSource *)cwm->ca_display_link setRunning:YES];
			return true;
		}
	}
	return false;
}

static inline struct vk_bundle *
get_vk(struct comp_window_macos *cwm)
{
	return &cwm->base.base.c->base.vk;
}


static void
macos_passthrough_sink_push_frame(struct xrt_frame_sink *sink, struct xrt_frame *frame)
{
	struct macos_passthrough_sink *pts = container_of(sink, struct macos_passthrough_sink, base);
	struct comp_window_macos *cwm = pts->cwm;

	if (atomic_load_explicit(&cwm->passthrough_shutdown, memory_order_acquire)) {
		return;
	}
	if (frame == NULL || frame->format != XRT_FORMAT_BC4 || frame->width != MACOS_PASSTHROUGH_WIDTH ||
	    frame->height != MACOS_PASSTHROUGH_HEIGHT) {
		return;
	}

	pthread_mutex_lock(&cwm->passthrough_mutex);
	if (!atomic_load_explicit(&cwm->passthrough_shutdown, memory_order_acquire)) {
		xrt_frame_reference(&cwm->passthrough_frames[pts->eye], frame);
	}
	pthread_mutex_unlock(&cwm->passthrough_mutex);

	// In the service: also pass the frame to clients compositing in-process.
	u_passthrough_share_push(pts->eye, frame);
}

static bool
macos_passthrough_create_uv_maps(struct comp_window_macos *cwm)
{
	struct xrt_device *xdev = cwm->base.base.c->xdev;
	if (xdev == NULL || xdev->compute_distortion == NULL) {
		return false;
	}

	const float fx = 0.3585564f;
	const float fy = 0.3762281f;
	const float camera_width_ratio = 1016.0f / 1024.0f;
	float fov_deg = (float)debug_get_num_option_macos_passthrough_fov_deg();
	float convergence = (float)debug_get_num_option_macos_passthrough_convergence_milli() / 1000.0f;
	if (fov_deg < 90.0f)
		fov_deg = 90.0f;
	if (fov_deg > 190.0f)
		fov_deg = 190.0f;
	const float fov_rad = fov_deg * 3.14159265358979323846f / 180.0f;

	id<MTLDevice> device = [cwm->metal_layer device];
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG32Float
	                                                                                width:MACOS_PASSTHROUGH_MAP_SIZE
	                                                                               height:MACOS_PASSTHROUGH_MAP_SIZE
	                                                                            mipmapped:NO];
	[desc setUsage:MTLTextureUsageShaderRead];
	[desc setStorageMode:MTLStorageModeManaged];

	size_t count = (size_t)MACOS_PASSTHROUGH_MAP_SIZE * MACOS_PASSTHROUGH_MAP_SIZE;
	float *map = malloc(count * 2 * sizeof(float));
	if (map == NULL) {
		return false;
	}

	for (uint32_t eye = 0; eye < 2; eye++) {
		const float cx = eye == 0 ? 0.6603788f : 0.3396213f;
		const float shift = eye == 0 ? convergence : -convergence;

		for (uint32_t y = 0; y < MACOS_PASSTHROUGH_MAP_SIZE; y++) {
			float v = ((float)y + 0.5f) / (float)MACOS_PASSTHROUGH_MAP_SIZE;
			for (uint32_t x = 0; x < MACOS_PASSTHROUGH_MAP_SIZE; x++) {
				float u = ((float)x + 0.5f) / (float)MACOS_PASSTHROUGH_MAP_SIZE;
				struct xrt_uv_triplet distortion = {0};
				size_t index = ((size_t)y * MACOS_PASSTHROUGH_MAP_SIZE + x) * 2;

				if (xrt_device_compute_distortion(xdev, eye, u, v, &distortion) != XRT_SUCCESS) {
					map[index + 0] = -1.0f;
					map[index + 1] = -1.0f;
					continue;
				}

				/* Recover the green-channel tangent ray from the existing
				 * PS VR2 optical distortion mapping, then apply GAV's proven
				 * equidistant camera model. */
				float tan_x = (distortion.g.x - cx) / fx;
				float tan_y_down = (distortion.g.y - 0.5f) / fy;
				float len = sqrtf(tan_x * tan_x + tan_y_down * tan_y_down + 1.0f);
				float dir_x = tan_x / len;
				float dir_y = -tan_y_down / len;
				float neg_dir_z = 1.0f / len;
				float theta = acosf(fmaxf(-1.0f, fminf(1.0f, neg_dir_z)));
				float radius = theta / fov_rad;
				float xy_len = sqrtf(dir_x * dir_x + dir_y * dir_y);

				if (radius > 0.5f || xy_len < 1e-7f) {
					if (radius <= 0.5f && xy_len < 1e-7f) {
						map[index + 0] = (0.5f + shift) * camera_width_ratio;
						map[index + 1] = 0.5f;
					} else {
						map[index + 0] = -1.0f;
						map[index + 1] = -1.0f;
					}
					continue;
				}

				float cam_u = (0.5f + radius * (dir_x / xy_len) + shift) * camera_width_ratio;
				float cam_v = 0.5f - radius * (dir_y / xy_len);
				if (cam_u < 0.0f || cam_u > 1.0f || cam_v < 0.0f || cam_v > 1.0f) {
					map[index + 0] = -1.0f;
					map[index + 1] = -1.0f;
				} else {
					map[index + 0] = cam_u;
					map[index + 1] = cam_v;
				}
			}
		}

		cwm->passthrough_uv_maps[eye] = [device newTextureWithDescriptor:desc];
		if (cwm->passthrough_uv_maps[eye] == nil) {
			free(map);
			return false;
		}
		MTLRegion region = MTLRegionMake2D(0, 0, MACOS_PASSTHROUGH_MAP_SIZE, MACOS_PASSTHROUGH_MAP_SIZE);
		[cwm->passthrough_uv_maps[eye] replaceRegion:region
		                                 mipmapLevel:0
		                                   withBytes:map
		                                 bytesPerRow:MACOS_PASSTHROUGH_MAP_SIZE * 2 * sizeof(float)];
	}

	free(map);
	return true;
}

static bool
macos_passthrough_init(struct comp_window_macos *cwm)
{
	/*
	 * Check for a camera source first: the UV maps below sample the device's
	 * distortion for every texel, which for an IPC device (a client
	 * compositing in-process) is half a million round trips to the service.
	 * IPC devices have no passthrough sinks, as the cameras are the service's.
	 */
	struct xrt_device *xdev = cwm->base.base.c->xdev;
	if (xdev == NULL || xdev->set_passthrough_sinks == NULL) {
		COMP_INFO(cwm->base.base.c,
		          "PS VR2 passthrough unavailable: the head device has no camera source here");
		return false;
	}

	id<MTLDevice> device = [cwm->metal_layer device];

	MTLTextureDescriptor *cam_desc =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBC4_RUnorm
	                                                       width:MACOS_PASSTHROUGH_WIDTH
	                                                      height:MACOS_PASSTHROUGH_HEIGHT
	                                                   mipmapped:NO];
	[cam_desc setUsage:MTLTextureUsageShaderRead];
	[cam_desc setStorageMode:MTLStorageModeManaged];

	for (uint32_t eye = 0; eye < 2; eye++) {
		cwm->passthrough_camera_textures[eye] = [device newTextureWithDescriptor:cam_desc];
		if (cwm->passthrough_camera_textures[eye] == nil) {
			COMP_WARN(cwm->base.base.c, "Could not create PS VR2 BC4 passthrough texture %u", eye);
			return false;
		}
	}

	static const char *shader_source =
	    "#include <metal_stdlib>\n"
	    "using namespace metal;\n"
	    "struct VSOut { float4 pos [[position]]; float2 uv; };\n"
	    "vertex VSOut psvr2_pt_vs(uint vid [[vertex_id]]) {\n"
	    "  float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};\n"
	    "  VSOut o; o.pos=float4(p[vid],0,1);"
	    "  o.uv=float2(p[vid].x*0.5+0.5, 1.0-(p[vid].y*0.5+0.5)); return o;\n"
	    "}\n"
	    "struct Params { float brightness; uint has_app; uint map_size; uint pad; };\n"
	    "fragment float4 psvr2_pt_fs(VSOut in [[stage_in]],"
	    " texture2d<float> app [[texture(0)]], texture2d<float> cam_l [[texture(1)]],"
	    " texture2d<float> cam_r [[texture(2)]], texture2d<float, access::read> map_l [[texture(3)]],"
	    " texture2d<float, access::read> map_r [[texture(4)]], constant Params &params [[buffer(0)]]) {\n"
	    "  constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);\n"
	    "  bool right = in.uv.x >= 0.5; float2 local_uv=float2(right ? (in.uv.x-0.5)*2.0 : in.uv.x*2.0, in.uv.y);\n"
	    "  uint2 mi=uint2(min(uint(local_uv.x*float(params.map_size)), params.map_size-1),"
	    "                 min(uint(local_uv.y*float(params.map_size)), params.map_size-1));\n"
	    "  float2 cuv = right ? map_r.read(mi).rg : map_l.read(mi).rg;\n"
	    "  float3 camera=float3(0.0);"
	    "  if (cuv.x >= 0.0 && cuv.y >= 0.0) { float g=(right ? cam_r.sample(s,cuv).r : cam_l.sample(s,cuv).r);"
	    "    camera=float3(saturate(g*params.brightness)); }\n"
	    "  if (params.has_app != 0) { float4 a=app.sample(s,in.uv); return float4(a.rgb + camera*(1.0-a.a), 1.0); "
	    "}\n"
	    "  return float4(camera,1.0);\n"
	    "}\n";

	NSError *error = nil;
	NSString *source = [NSString stringWithUTF8String:shader_source];
	id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
	if (library == nil) {
		COMP_WARN(cwm->base.base.c, "Could not compile PS VR2 passthrough Metal shader: %s",
		          error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
		return false;
	}
	id<MTLFunction> vs = [library newFunctionWithName:@"psvr2_pt_vs"];
	id<MTLFunction> fs = [library newFunctionWithName:@"psvr2_pt_fs"];
	MTLRenderPipelineDescriptor *pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
	[pipeline_desc setVertexFunction:vs];
	[pipeline_desc setFragmentFunction:fs];
	MTLRenderPipelineColorAttachmentDescriptor *pipeline_color =
	    [[pipeline_desc colorAttachments] objectAtIndexedSubscript:0];
	[pipeline_color setPixelFormat:MTLPixelFormatBGRA8Unorm];
	cwm->passthrough_pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
	[pipeline_desc release];
	[vs release];
	[fs release];
	[library release];

	if (cwm->passthrough_pipeline == nil) {
		COMP_WARN(cwm->base.base.c, "Could not create PS VR2 passthrough Metal pipeline: %s",
		          error != nil ? [[error localizedDescription] UTF8String] : "unknown error");
		return false;
	}
	if (!macos_passthrough_create_uv_maps(cwm)) {
		COMP_WARN(cwm->base.base.c, "Could not create PS VR2 passthrough UV maps");
		return false;
	}

	for (uint32_t eye = 0; eye < 2; eye++) {
		cwm->passthrough_sinks[eye].base.push_frame = macos_passthrough_sink_push_frame;
		cwm->passthrough_sinks[eye].cwm = cwm;
		cwm->passthrough_sinks[eye].eye = eye;
	}

	xrt_result_t xret =
	    xdev->set_passthrough_sinks(xdev, &cwm->passthrough_sinks[0].base, &cwm->passthrough_sinks[1].base);
	if (xret == XRT_SUCCESS) {
		cwm->passthrough_sinks_attached = true;
		// Clients hosted by this service can have the frames too.
		if (cwm->hosted_client == NULL) {
			u_passthrough_share_set_source_available(true);
		}
		COMP_INFO(cwm->base.base.c, "PS VR2 BC4 passthrough attached (FOV %d deg, convergence %.3f)",
		          (int)debug_get_num_option_macos_passthrough_fov_deg(),
		          (double)debug_get_num_option_macos_passthrough_convergence_milli() / 1000.0);
		return true;
	}

	COMP_WARN(cwm->base.base.c,
	          "PS VR2 passthrough camera source unavailable (set PSVR2_CAMERA_STREAMS=1 before starting Monado)");
	return false;
}

static bool
macos_passthrough_upload(struct comp_window_macos *cwm)
{
	struct xrt_frame *frames[2] = {NULL, NULL};

	pthread_mutex_lock(&cwm->passthrough_mutex);
	for (uint32_t eye = 0; eye < 2; eye++) {
		xrt_frame_reference(&frames[eye], cwm->passthrough_frames[eye]);
	}
	pthread_mutex_unlock(&cwm->passthrough_mutex);

	bool ready = true;
	for (uint32_t eye = 0; eye < 2; eye++) {
		struct xrt_frame *frame = frames[eye];
		if (frame == NULL) {
			ready = false;
			continue;
		}
		if (frame->timestamp != cwm->passthrough_uploaded_timestamp[eye]) {
			MTLRegion region = MTLRegionMake2D(0, 0, MACOS_PASSTHROUGH_WIDTH, MACOS_PASSTHROUGH_HEIGHT);
			[cwm->passthrough_camera_textures[eye] replaceRegion:region
			                                         mipmapLevel:0
			                                           withBytes:frame->data
			                                         bytesPerRow:frame->stride];
			cwm->passthrough_uploaded_timestamp[eye] = frame->timestamp;
		}
	}

	for (uint32_t eye = 0; eye < 2; eye++) {
		xrt_frame_reference(&frames[eye], NULL);
	}
	return ready;
}

static bool
macos_passthrough_encode(struct comp_window_macos *cwm,
                         id<MTLCommandBuffer> command_buffer,
                         id<CAMetalDrawable> drawable,
                         id<MTLTexture> app_texture,
                         bool has_application_layers)
{
	if (cwm->passthrough_pipeline == nil || !cwm->passthrough_sinks_attached || !macos_passthrough_upload(cwm)) {
		return false;
	}

	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	MTLRenderPassColorAttachmentDescriptor *color = [[rp colorAttachments] objectAtIndexedSubscript:0];
	[color setTexture:[drawable texture]];
	[color setLoadAction:MTLLoadActionDontCare];
	[color setStoreAction:MTLStoreActionStore];

	id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:rp];
	if (encoder == nil) {
		return false;
	}

	struct
	{
		float brightness;
		uint32_t has_app;
		uint32_t map_size;
		uint32_t pad;
	} params = {
	    .brightness = (float)debug_get_num_option_macos_passthrough_brightness_percent() / 100.0f,
	    .has_app = has_application_layers ? 1u : 0u,
	    .map_size = MACOS_PASSTHROUGH_MAP_SIZE,
	    .pad = 0,
	};

	[encoder setRenderPipelineState:cwm->passthrough_pipeline];
	[encoder setFragmentTexture:app_texture atIndex:0];
	[encoder setFragmentTexture:cwm->passthrough_camera_textures[0] atIndex:1];
	[encoder setFragmentTexture:cwm->passthrough_camera_textures[1] atIndex:2];
	[encoder setFragmentTexture:cwm->passthrough_uv_maps[0] atIndex:3];
	[encoder setFragmentTexture:cwm->passthrough_uv_maps[1] atIndex:4];
	[encoder setFragmentBytes:&params length:sizeof(params) atIndex:0];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[encoder endEncoding];
	return true;
}

static void
macos_release_source_image(struct comp_window_macos *cwm, uint32_t index)
{
	if (index < MACOS_TARGET_IMAGE_COUNT) {
		atomic_store_explicit(&cwm->image_in_flight[index], false, memory_order_release);
	}
}

//! Ends the presenter's use of a job's source image, whichever kind it is.
static void
macos_release_job_source(struct comp_window_macos *cwm, const struct macos_present_job *job)
{
	if (job->external_texture != nil) {
		[job->external_texture release];
		job->external_release(job->external_release_data);
		return;
	}
	macos_release_source_image(cwm, job->image_index);
}

static void
macos_trace_drawable_prefetch(struct comp_window_macos *cwm,
                              const char *event,
                              uint64_t timeline_value,
                              uint64_t event_ns,
                              uint64_t begin_ns,
                              uint64_t end_ns)
{
	if (cwm->trace_drawable_prefetch == NULL) {
		return;
	}
	uint64_t wait_ns = end_ns > begin_ns ? end_ns - begin_ns : 0;
	flockfile(cwm->trace_drawable_prefetch);
	fprintf(cwm->trace_drawable_prefetch, "%s,%llu,%llu,%llu,%llu,%llu\n", event,
	        (unsigned long long)timeline_value, (unsigned long long)event_ns, (unsigned long long)begin_ns,
	        (unsigned long long)end_ns, (unsigned long long)wait_ns);
	cwm->trace_drawable_prefetch_rows++;
	if (cwm->trace_drawable_prefetch_rows % 256 == 0) {
		macos_trace_buffered_fflush(cwm->trace_drawable_prefetch);
	}
	funlockfile(cwm->trace_drawable_prefetch);
}

static void
macos_release_prefetched_drawable(struct comp_window_macos *cwm, const char *trace_event)
{
	if (cwm->prefetched_drawable == nil) {
		return;
	}
	if (trace_event != NULL) {
		macos_trace_drawable_prefetch(cwm, trace_event, cwm->prefetched_drawable_timeline_value,
		                              os_monotonic_get_ns(), cwm->prefetched_drawable_begin_ns,
		                              cwm->prefetched_drawable_end_ns);
	}
	[cwm->prefetched_drawable release];
	cwm->prefetched_drawable = nil;
	cwm->prefetched_drawable_timeline_value = 0;
	cwm->prefetched_drawable_begin_ns = 0;
	cwm->prefetched_drawable_end_ns = 0;
}

static void
macos_present_worker_run_one(struct comp_window_macos *cwm);

static void
macos_schedule_drawable_slot(struct comp_window_macos *cwm)
{
	if (!cwm->drawable_slot_enabled || cwm->present_worker_queue == NULL || cwm->present_worker_group == NULL ||
	    cwm->render_complete_event == nil || cwm->metal_layer == nil) {
		return;
	}

	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (cwm->present_worker_shutdown || cwm->drawable_slot_acquire_scheduled || cwm->prefetched_drawable != nil) {
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		return;
	}
	cwm->drawable_slot_acquire_scheduled = true;
	dispatch_group_enter(cwm->present_worker_group);
	pthread_mutex_unlock(&cwm->present_worker_mutex);

	dispatch_async(cwm->present_worker_queue, ^{
	  @autoreleasepool {
		  uint64_t begin_ns = os_monotonic_get_ns();
		  macos_trace_drawable_prefetch(cwm, "slot_acquire_begin", 0, begin_ns, begin_ns, 0);
		  id<CAMetalDrawable> drawable = [cwm->metal_layer nextDrawable];
		  uint64_t end_ns = os_monotonic_get_ns();
		  id<CAMetalDrawable> retained_drawable = drawable != nil ? [drawable retain] : nil;
		  bool stored = false;
		  bool schedule_present = false;
		  bool retry_acquire = false;

		  pthread_mutex_lock(&cwm->present_worker_mutex);
		  cwm->drawable_slot_acquire_scheduled = false;
		  if (!cwm->present_worker_shutdown && retained_drawable != nil && cwm->prefetched_drawable == nil) {
			  cwm->prefetched_drawable = retained_drawable;
			  cwm->prefetched_drawable_timeline_value = 0;
			  cwm->prefetched_drawable_begin_ns = begin_ns;
			  cwm->prefetched_drawable_end_ns = end_ns;
			  retained_drawable = nil;
			  stored = true;
			  if (cwm->pending_present_job_valid && !cwm->present_worker_scheduled) {
				  cwm->present_worker_scheduled = true;
				  dispatch_group_enter(cwm->present_worker_group);
				  schedule_present = true;
			  }
		  } else if (!cwm->present_worker_shutdown && drawable == nil && cwm->prefetched_drawable == nil) {
			  retry_acquire = true;
		  }
		  pthread_mutex_unlock(&cwm->present_worker_mutex);

		  if (retained_drawable != nil) {
			  [retained_drawable release];
		  }
		  macos_trace_drawable_prefetch(
		      cwm, stored ? "slot_ready" : (drawable == nil ? "slot_acquire_nil" : "slot_discard"), 0, end_ns,
		      begin_ns, end_ns);
		  dispatch_group_leave(cwm->present_worker_group);

		  if (schedule_present) {
			  dispatch_async(cwm->present_worker_queue, ^{
			    macos_present_worker_run_one(cwm);
			  });
		  }
		  if (retry_acquire) {
			  macos_schedule_drawable_slot(cwm);
		  }
	  }
	});
}

static void
macos_trace_present_worker(struct comp_window_macos *cwm,
                           const char *event,
                           const struct macos_present_job *job,
                           uint64_t event_ns,
                           uint64_t handoff_return_ns,
                           uint64_t worker_start_ns,
                           uint64_t next_drawable_begin_ns,
                           uint64_t next_drawable_end_ns,
                           uint64_t metal_commit_ns,
                           uint32_t queue_depth,
                           bool shared_event_wait)
{
	if (cwm->trace_present_worker == NULL) {
		return;
	}
	uint64_t worker_delay_ns = worker_start_ns > job->enqueue_ns ? worker_start_ns - job->enqueue_ns : 0;
	uint64_t drawable_wait_ns =
	    next_drawable_end_ns > next_drawable_begin_ns ? next_drawable_end_ns - next_drawable_begin_ns : 0;
	bool newer_pending = false;
	uint64_t pending_frame_id = 0;
	uint64_t pending_timeline_value = 0;
	if (next_drawable_end_ns != 0 && cwm->present_worker_enabled) {
		pthread_mutex_lock(&cwm->present_worker_mutex);
		if (cwm->pending_present_job_valid && cwm->pending_present_job.frame_id > job->frame_id) {
			newer_pending = true;
			pending_frame_id = cwm->pending_present_job.frame_id;
			pending_timeline_value = cwm->pending_present_job.timeline_value;
			if (queue_depth == 0) {
				queue_depth = 1;
			}
		}
		pthread_mutex_unlock(&cwm->present_worker_mutex);
	}
	flockfile(cwm->trace_present_worker);
	fprintf(cwm->trace_present_worker,
	        "%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu,%u,%u,%u,%llu,%llu\n", event,
	        (unsigned long long)job->frame_id, (unsigned long long)event_ns, (unsigned long long)job->enqueue_ns,
	        (unsigned long long)handoff_return_ns, (unsigned long long)worker_start_ns,
	        (unsigned long long)next_drawable_begin_ns, (unsigned long long)next_drawable_end_ns,
	        (unsigned long long)metal_commit_ns, (unsigned long long)worker_delay_ns,
	        (unsigned long long)drawable_wait_ns, job->image_index, (unsigned long long)job->timeline_value,
	        queue_depth, shared_event_wait ? 1u : 0u, newer_pending ? 1u : 0u, (unsigned long long)pending_frame_id,
	        (unsigned long long)pending_timeline_value);
	cwm->trace_present_worker_rows++;
	if (cwm->trace_present_worker_rows % 256 == 0) {
		macos_trace_buffered_fflush(cwm->trace_present_worker);
	}
	funlockfile(cwm->trace_present_worker);
}

static void
macos_drain_present_worker(struct comp_window_macos *cwm)
{
	if (cwm->present_worker_group != NULL) {
		dispatch_group_wait(cwm->present_worker_group, DISPATCH_TIME_FOREVER);
	}
	if (cwm->present_command_group != NULL) {
		dispatch_group_wait(cwm->present_command_group, DISPATCH_TIME_FOREVER);
	}
}


static bool
comp_window_macos_init(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	@autoreleasepool {
		struct comp_macos_frontend_info info = {0};
		cwm->frontend = comp_macos_frontend_create(ct->c, cwm->hosted_client, &info);
		if (cwm->frontend == NULL) {
			return false;
		}

		cwm->metal_layer = info.metal_layer;
		cwm->display_id = info.display_id;
		cwm->pixel_width = info.pixel_width;
		cwm->pixel_height = info.pixel_height;
		snprintf(cwm->display_name, sizeof(cwm->display_name), "%s", info.display_name);
		CAMetalLayer *metal_layer = cwm->metal_layer;
		[metal_layer setDrawableSize:CGSizeMake(cwm->pixel_width, cwm->pixel_height)];
		[metal_layer setOpaque:YES];
		[metal_layer setDisplaySyncEnabled:YES];
		[metal_layer setAllowsNextDrawableTimeout:YES];
		/* Two drawables starved nextDrawable and collapsed to ~60 Hz on hardware. */
		[metal_layer setMaximumDrawableCount:3];
		COMP_INFO(ct->c, "macOS CAMetalLayer maximumDrawableCount=%lu",
		          (unsigned long)[metal_layer maximumDrawableCount]);
		id<MTLCommandQueue> present_queue = [[metal_layer device] newCommandQueue];
		if (present_queue == nil) {
			comp_macos_frontend_destroy(&cwm->frontend);
			cwm->metal_layer = nil;
			COMP_ERROR(ct->c, "Failed to create the macOS Metal presentation queue");
			return false;
		}
		cwm->present_queue = present_queue;
		cwm->present_copy = comp_macos_present_copy_create([metal_layer device]);
		if (cwm->present_copy == NULL) {
			comp_macos_frontend_destroy(&cwm->frontend);
			cwm->metal_layer = nil;
			COMP_ERROR(ct->c, "Failed to create the macOS presentation copy");
			return false;
		}

		comp_macos_frontend_show(cwm->frontend);

		/* Best effort: ordinary presentation continues if camera passthrough
		 * is unavailable or PSVR2_CAMERA_STREAMS was not enabled. */
		(void)macos_passthrough_init(cwm);

		mach_timebase_info(&cwm->mach_timebase);
		refresh_host_to_monotonic_offset_ns(cwm);
		const int64_t estimated_period_ns = cwm->display_period_ns;
		cwm->display_period_ns = 0;
		if (!macos_display_link_create(cwm, cwm->display_id)) {
			COMP_WARN(ct->c, "Could not create the PS VR2 display link; using estimated pacing");
		}
		if (cwm->display_period_ns > 0) {
			ct->c->frame_interval_ns = cwm->display_period_ns;
			COMP_INFO(ct->c, "PS VR2 display period %.3fms (%.2f Hz)",
			          (double)cwm->display_period_ns / 1000000.0,
			          (double)U_TIME_1S_IN_NS / (double)ct->c->frame_interval_ns);
		} else {
			cwm->display_period_ns = estimated_period_ns;
		}

		VkExtent2D extent = {.width = cwm->pixel_width, .height = cwm->pixel_height};
		comp_target_swapchain_override_extents(&cwm->base, extent);
		COMP_INFO(ct->c, "Selected macOS display '%s' at %ux%u (%s front-end)", cwm->display_name, extent.width,
		          extent.height, comp_macos_frontend_name(cwm->frontend));
	}
	return true;
}

/*
 * Drawable-slot mode only prefetches drawables once the render-complete timeline
 * is exported as an MTLSharedEvent, and only schedules the present worker once a
 * drawable is prefetched. Without that handoff it would never present, so fall
 * back to the worker acquiring its own drawable behind a CPU Vulkan wait.
 */
static void
macos_disable_drawable_slot_without_shared_event(struct comp_window_macos *cwm)
{
	if (!cwm->drawable_slot_enabled) {
		return;
	}
	COMP_WARN(cwm->base.base.c,
	          "XRT_MACOS_DRAWABLE_SLOT requires the MTLSharedEvent render-complete handoff, which is unavailable; "
	          "drawable slot is disabled");
	cwm->drawable_slot_enabled = false;
}

static bool
comp_window_macos_init_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	(void)preferred_width;
	(void)preferred_height;
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	struct vk_bundle *vk = get_vk(cwm);

	if (!vk->features.timeline_semaphore || vk->vkWaitSemaphores == NULL) {
		COMP_WARN(ct->c,
		          "Timeline semaphores unavailable; macOS presentation will fall back to queue-idle waits");
		macos_disable_drawable_slot_without_shared_event(cwm);
		return true;
	}

	bool want_shared_event = vk->has_EXT_metal_objects && vk->vkExportMetalObjectsEXT != NULL;

	VkExportMetalObjectCreateInfoEXT metal_export_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
	    .pNext = NULL,
	    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT,
	};
	VkSemaphoreTypeCreateInfo type_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
	    .pNext = want_shared_event ? &metal_export_info : NULL,
	    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	    .initialValue = 0,
	};
	VkSemaphoreCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	    .pNext = &type_info,
	};
	VkResult ret = vk->vkCreateSemaphore(vk->device, &create_info, NULL, &ct->semaphores.render_complete);
	if (ret != VK_SUCCESS) {
		COMP_WARN(ct->c,
		          "Could not create macOS render-complete timeline semaphore: %s; using queue-idle fallback",
		          vk_result_string(ret));
		ct->semaphores.render_complete = VK_NULL_HANDLE;
		ct->semaphores.render_complete_is_timeline = false;
		macos_disable_drawable_slot_without_shared_event(cwm);
		return true;
	}
	ct->semaphores.render_complete_is_timeline = true;

	if (want_shared_event) {
		VkExportMetalSharedEventInfoEXT shared_event_info = {
		    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT,
		    .pNext = NULL,
		    .semaphore = ct->semaphores.render_complete,
		    .event = VK_NULL_HANDLE,
		    .mtlSharedEvent = nil,
		};
		VkExportMetalObjectsInfoEXT export_info = {
		    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
		    .pNext = &shared_event_info,
		};
		vk->vkExportMetalObjectsEXT(vk->device, &export_info);
		if (shared_event_info.mtlSharedEvent != nil) {
			cwm->render_complete_event = [shared_event_info.mtlSharedEvent retain];
			COMP_INFO(ct->c, "macOS target exported Vulkan render-complete timeline as MTLSharedEvent");
		} else {
			COMP_WARN(ct->c,
			          "VK_EXT_metal_objects did not export an MTLSharedEvent; retaining CPU Vulkan wait "
			          "fallback");
		}
	} else {
		COMP_WARN(ct->c,
		          "VK_EXT_metal_objects unavailable; asynchronous present will retain the CPU Vulkan wait");
	}

	COMP_INFO(ct->c, "macOS target using render-complete timeline semaphore%s",
	          cwm->render_complete_event != nil ? " with Metal shared-event handoff" : "");
	if (cwm->render_complete_event == nil) {
		macos_disable_drawable_slot_without_shared_event(cwm);
	}
	return true;
}

static bool
comp_window_macos_check_ready(struct comp_target *ct)
{
	(void)ct;
	return true;
}

static void
comp_window_macos_free_images(struct comp_window_macos *cwm)
{
	macos_drain_present_worker(cwm);
	macos_release_prefetched_drawable(cwm, "free_images_release");
	struct comp_target *ct = &cwm->base.base;
	struct vk_bundle *vk = get_vk(cwm);
	for (uint32_t i = 0; i < MACOS_TARGET_IMAGE_COUNT; i++) {
		[cwm->metal_images[i] release];
		cwm->metal_images[i] = nil;
		u_graphics_buffer_unref(&cwm->io_surfaces[i]);
	}
	if (ct->images != NULL) {
		for (uint32_t i = 0; i < ct->image_count; i++) {
			if (ct->images[i].storage_view != VK_NULL_HANDLE &&
			    ct->images[i].storage_view != ct->images[i].view) {
				vk->vkDestroyImageView(vk->device, ct->images[i].storage_view, NULL);
			}
			if (ct->images[i].view != VK_NULL_HANDLE) {
				vk->vkDestroyImageView(vk->device, ct->images[i].view, NULL);
			}
		}
		free(ct->images);
	}
	vk_ic_destroy(vk, &cwm->vkic);
	ct->images = NULL;
	ct->image_count = 0;
	ct->width = 0;
	ct->height = 0;
	ct->format = VK_FORMAT_UNDEFINED;
	ct->final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	for (uint32_t i = 0; i < MACOS_TARGET_IMAGE_COUNT; i++) {
		atomic_store_explicit(&cwm->image_in_flight[i], false, memory_order_release);
	}
}

static void
comp_window_macos_create_images(struct comp_target *ct,
                                const struct comp_target_create_images_info *create_info,
                                struct vk_bundle_queue *present_queue)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	struct vk_bundle *vk = get_vk(cwm);
	bool supports_bgra8 = false;
	assert(present_queue != NULL);
	for (uint32_t i = 0; i < create_info->format_count; i++) {
		if (create_info->formats[i] == VK_FORMAT_B8G8R8A8_UNORM) {
			supports_bgra8 = true;
			break;
		}
	}
	if (!supports_bgra8) {
		COMP_ERROR(ct->c, "The macOS Metal target requires VK_FORMAT_B8G8R8A8_UNORM");
		return;
	}

	comp_window_macos_free_images(cwm);
	struct xrt_swapchain_create_info info = {
	    .create = 0,
	    .bits = XRT_SWAPCHAIN_USAGE_COLOR | XRT_SWAPCHAIN_USAGE_SAMPLED | XRT_SWAPCHAIN_USAGE_TRANSFER_SRC |
	            XRT_SWAPCHAIN_USAGE_TRANSFER_DST | XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .sample_count = 1,
	    .width = cwm->pixel_width,
	    .height = cwm->pixel_height,
	    .face_count = 1,
	    .array_size = 1,
	    .mip_count = 1,
	};
	VkResult ret = vk_ic_allocate(vk, &info, MACOS_TARGET_IMAGE_COUNT, &cwm->vkic);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "Could not allocate macOS compositor images: %s", vk_result_string(ret));
		return;
	}
	ret = vk_ic_get_handles(vk, &cwm->vkic, MACOS_TARGET_IMAGE_COUNT, cwm->io_surfaces);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "Could not export macOS compositor IOSurfaces: %s", vk_result_string(ret));
		comp_window_macos_free_images(cwm);
		return;
	}

	ct->images = U_TYPED_ARRAY_CALLOC(struct comp_target_image, MACOS_TARGET_IMAGE_COUNT);
	if (ct->images == NULL) {
		COMP_ERROR(ct->c, "Could not allocate macOS compositor image metadata");
		comp_window_macos_free_images(cwm);
		return;
	}
	ct->image_count = MACOS_TARGET_IMAGE_COUNT;
	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};
	MTLTextureDescriptor *descriptor =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                       width:cwm->pixel_width
	                                                      height:cwm->pixel_height
	                                                   mipmapped:NO];
	[descriptor setUsage:MTLTextureUsageShaderRead];
	for (uint32_t i = 0; i < MACOS_TARGET_IMAGE_COUNT; i++) {
		ct->images[i].handle = cwm->vkic.images[i].handle;
		ret = vk_create_view(vk, ct->images[i].handle, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, range,
		                     &ct->images[i].view);
		if (ret != VK_SUCCESS) {
			COMP_ERROR(ct->c, "Could not create macOS compositor image view: %s", vk_result_string(ret));
			comp_window_macos_free_images(cwm);
			return;
		}
		ct->images[i].storage_view = ct->images[i].view;
		cwm->metal_images[i] = [[cwm->metal_layer device] newTextureWithDescriptor:descriptor
		                                                                 iosurface:cwm->io_surfaces[i]
		                                                                     plane:0];
		if (cwm->metal_images[i] == nil) {
			COMP_ERROR(ct->c, "Could not create a Metal texture for macOS compositor image %u", i);
			comp_window_macos_free_images(cwm);
			return;
		}
	}

	ct->width = cwm->pixel_width;
	ct->height = cwm->pixel_height;
	ct->format = VK_FORMAT_B8G8R8A8_UNORM;
	ct->image_usage = vk_csci_get_image_usage_flags(&ct->c->base.vk, (VkFormat)info.format, info.bits);
	ct->final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	// Same as comp_target_swapchain: the graphics path clears the target.
	ct->present_load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;
	ct->surface_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	cwm->next_image = 0;
	if (cwm->base.upc == NULL) {
		u_pc_fake_create(ct->c->frame_interval_ns, os_monotonic_get_ns(), &cwm->base.upc);
	}
	if (macos_display_link_exists(cwm) && !macos_display_link_is_running(cwm)) {
		if (!macos_display_link_start(cwm)) {
			COMP_WARN(ct->c, "Could not start the PS VR2 display link; using estimated pacing");
		}
	}
	macos_schedule_drawable_slot(cwm);
	COMP_INFO(ct->c, "Created %u IOSurface-backed macOS compositor images at %ux%u", ct->image_count, ct->width,
	          ct->height);
}

static bool
comp_window_macos_has_images(struct comp_target *ct)
{
	return ct->images != NULL && ct->image_count != 0;
}

static VkResult
comp_window_macos_acquire(struct comp_target *ct, uint32_t *out_index)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	if (!comp_window_macos_has_images(ct)) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	uint64_t wait_begin_ns = os_monotonic_get_ns();
	for (;;) {
		for (uint32_t n = 0; n < ct->image_count; n++) {
			uint32_t index = (cwm->next_image + n) % ct->image_count;
			bool expected = false;
			if (atomic_compare_exchange_strong_explicit(&cwm->image_in_flight[index], &expected, true,
			                                            memory_order_acq_rel, memory_order_acquire)) {
				*out_index = index;
				cwm->next_image = (index + 1) % ct->image_count;
				cwm->last_image_acquire_wait_ns = os_monotonic_get_ns() - wait_begin_ns;
				return VK_SUCCESS;
			}
		}

		/* Three source images should normally hide this; yield briefly only under back-pressure. */
		struct timespec ts = {.tv_sec = 0, .tv_nsec = 50000};
		(void)nanosleep(&ts, NULL);
	}
}

static void
macos_retire_unpresented_job(struct comp_window_macos *cwm,
                             const struct macos_present_job *job,
                             const char *trace_event,
                             uint32_t queue_depth);

static VkResult
macos_execute_present_job(struct comp_window_macos *cwm, const struct macos_present_job *job)
{
	struct comp_target *ct = &cwm->base.base;
	struct vk_bundle *vk = get_vk(cwm);
	struct vk_bundle_queue *present_queue = job->present_queue;
	uint64_t frame_id = job->frame_id;
	uint32_t index = job->image_index;
	uint64_t timeline_semaphore_value = job->timeline_value;
	int64_t desired_present_time_ns = job->desired_present_time_ns;
	int64_t present_slop_ns = job->present_slop_ns;
	u_timing_trace_poll_flush_request();
	uint64_t worker_start_ns = os_monotonic_get_ns();
	uint64_t next_drawable_begin_ns = 0;
	uint64_t after_drawable_ns = 0;
	uint64_t before_present_call_ns = 0;
	uint64_t after_present_call_ns = 0;
	uint64_t after_commit_ns = 0;
	uint64_t after_metal_wait_ns = 0;
	uint64_t target_output_ns = 0;
	uint64_t metal_request_ns = 0;
	const char *wait_mode = "queue_idle";
	bool external = job->external_texture != nil;
	bool shared_event_wait = !external && cwm->render_complete_event != nil;
	uint64_t image_reuse_wait_ns = job->image_reuse_wait_ns;
	double scheduled_present_host_s = 0.0;
	double gpu_start_time_s = 0.0;
	double gpu_end_time_s = 0.0;
	macos_trace_present_worker(cwm, "worker_start", job, worker_start_ns, 0, worker_start_ns, 0, 0, 0, 0,
	                           shared_event_wait);
	assert(external || present_queue != NULL);
	if (!external && (index >= ct->image_count || cwm->metal_images[index] == nil)) {
		macos_retire_unpresented_job(cwm, job, "invalid", 0);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	id<MTLTexture> source_texture = external ? job->external_texture : cwm->metal_images[index];

	uint64_t host_call_ns = job->enqueue_ns;
	uint64_t before_vk_wait_ns = os_monotonic_get_ns();
	VkResult ret = VK_SUCCESS;
	if (external) {
		wait_mode = "external";
	} else if (shared_event_wait) {
		wait_mode = "metal_shared_event";
	} else if (ct->semaphores.render_complete != VK_NULL_HANDLE && ct->semaphores.render_complete_is_timeline &&
	           vk->vkWaitSemaphores != NULL) {
		VkSemaphoreWaitInfo wait_info = {
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		    .semaphoreCount = 1,
		    .pSemaphores = &ct->semaphores.render_complete,
		    .pValues = &timeline_semaphore_value,
		};
		wait_mode = "timeline";
		ret = vk->vkWaitSemaphores(vk->device, &wait_info, UINT64_MAX);
	} else {
		vk_queue_lock(present_queue);
		ret = vk->vkQueueWaitIdle(present_queue->queue);
		vk_queue_unlock(present_queue);
	}
	uint64_t after_vk_wait_ns = os_monotonic_get_ns();
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "Vulkan render-complete wait before Metal presentation: %s", vk_result_string(ret));
		macos_release_job_source(cwm, job);
		return ret;
	}

	@autoreleasepool {
		id<CAMetalDrawable> drawable = nil;
		if (cwm->drawable_slot_enabled) {
			id<CAMetalDrawable> retained_drawable = nil;
			pthread_mutex_lock(&cwm->present_worker_mutex);
			if (cwm->prefetched_drawable != nil) {
				retained_drawable = cwm->prefetched_drawable;
				next_drawable_begin_ns = cwm->prefetched_drawable_begin_ns;
				after_drawable_ns = cwm->prefetched_drawable_end_ns;
				cwm->prefetched_drawable = nil;
				cwm->prefetched_drawable_timeline_value = 0;
				cwm->prefetched_drawable_begin_ns = 0;
				cwm->prefetched_drawable_end_ns = 0;
			}
			pthread_mutex_unlock(&cwm->present_worker_mutex);

			if (retained_drawable == nil) {
				/*
				 * This should only be a race/failure fallback: slot mode schedules the
				 * present worker only after a prefetched drawable is ready. If the
				 * slot disappears, block this background worker rather than dropping
				 * a rendered frame. The producer remains free to replace the one
				 * pending job with a newer frame while nextDrawable blocks.
				 */
				next_drawable_begin_ns = os_monotonic_get_ns();
				macos_trace_drawable_prefetch(cwm, "slot_fallback_begin", timeline_semaphore_value,
				                              next_drawable_begin_ns, next_drawable_begin_ns, 0);
				drawable = [cwm->metal_layer nextDrawable];
				after_drawable_ns = os_monotonic_get_ns();
				macos_trace_drawable_prefetch(cwm, "slot_fallback_end", timeline_semaphore_value,
				                              after_drawable_ns, next_drawable_begin_ns,
				                              after_drawable_ns);
			} else {
				drawable = [retained_drawable autorelease];
				macos_trace_drawable_prefetch(cwm, "slot_consumed", timeline_semaphore_value,
				                              os_monotonic_get_ns(), next_drawable_begin_ns,
				                              after_drawable_ns);
				macos_schedule_drawable_slot(cwm);
			}
		} else {
			if (cwm->prefetched_drawable != nil) {
				macos_release_prefetched_drawable(cwm, "present_mismatch_release");
			}
			uint64_t drawable_trace_begin_ns = os_monotonic_get_ns();
			/*
			 * Do not include trace-writing latency in the nextDrawable measurement.
			 * The actual call start is captured only after this row has been written;
			 * drawable_end records both the real call begin and end timestamps.
			 */
			macos_trace_present_worker(cwm, "drawable_trace_begin", job, drawable_trace_begin_ns, 0,
			                           worker_start_ns, 0, 0, 0, 0, shared_event_wait);
			next_drawable_begin_ns = os_monotonic_get_ns();
			drawable = [cwm->metal_layer nextDrawable];
			after_drawable_ns = os_monotonic_get_ns();
			macos_trace_present_worker(cwm, "drawable_end", job, after_drawable_ns, 0, worker_start_ns,
			                           next_drawable_begin_ns, after_drawable_ns, 0, 0, shared_event_wait);
		}
		if (drawable == nil) {
			COMP_ERROR(ct->c, "Could not acquire a CAMetalDrawable");
			macos_retire_unpresented_job(cwm, job, "drawable_error", 0);
			return VK_ERROR_OUT_OF_DATE_KHR;
		}
		id<MTLCommandBuffer> command_buffer = [cwm->present_queue commandBuffer];
		if (command_buffer == nil) {
			COMP_ERROR(ct->c, "Could not create a Metal presentation command buffer");
			macos_retire_unpresented_job(cwm, job, "command_buffer_error", 0);
			return VK_ERROR_DEVICE_LOST;
		}
		if (shared_event_wait) {
			[command_buffer encodeWaitForEvent:cwm->render_complete_event value:timeline_semaphore_value];
		}
		bool encoded_passthrough = false;
		if (job->passthrough_active) {
			encoded_passthrough = macos_passthrough_encode(cwm, command_buffer, drawable, source_texture,
			                                               job->passthrough_has_application_layers);
		}
		if (!encoded_passthrough &&
		    !comp_macos_present_copy_encode(cwm->present_copy, command_buffer, source_texture,
		                                    [drawable texture])) {
			COMP_ERROR(ct->c, "Could not encode the copy into the drawable");
			macos_retire_unpresented_job(cwm, job, "blit_error", 0);
			return VK_ERROR_DEVICE_LOST;
		}

		before_present_call_ns = os_monotonic_get_ns();
		uint64_t latest_output_ns =
		    atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);
		int64_t min_lead_us = debug_get_num_option_macos_present_min_lead_us();
		if (min_lead_us < 0) {
			min_lead_us = 0;
		}
		uint64_t earliest_output_ns = before_present_call_ns + (uint64_t)min_lead_us * 1000ULL;
		if (desired_present_time_ns > 0 && (uint64_t)desired_present_time_ns > earliest_output_ns) {
			earliest_output_ns = (uint64_t)desired_present_time_ns;
		}
		target_output_ns = latest_output_ns;
		if (target_output_ns == 0) {
			target_output_ns = earliest_output_ns;
		} else if (cwm->display_period_ns > 0) {
			uint64_t period_ns = (uint64_t)cwm->display_period_ns;
			while (target_output_ns < earliest_output_ns) {
				target_output_ns += period_ns;
			}
		}

		if (cwm->presented_state != NULL) {
			struct macos_presented_state *presented_state = cwm->presented_state;
			macos_presented_state_retain(presented_state);
			FILE *trace_file = presented_state->trace_presented;
			uint64_t traced_frame_id = frame_id;
			int64_t traced_desired_present_ns = desired_present_time_ns;
			uint64_t traced_target_output_ns = target_output_ns;
			int64_t traced_display_period_ns = cwm->display_period_ns;
			[drawable addPresentedHandler:^(id<MTLDrawable> presented_drawable) {
			  double presented_time_s = [presented_drawable presentedTime];
			  int64_t handler_ns = os_monotonic_get_ns();
			  double host_frequency = CVGetHostClockFrequency();
			  uint64_t current_host_ticks = CVGetCurrentHostTime();
			  int64_t current_host_ns =
			      host_frequency > 0.0 ? (int64_t)llround((double)current_host_ticks * 1e9 / host_frequency)
				                   : 0;
			  int64_t handler_offset_ns = handler_ns - current_host_ns;
			  int64_t presented_host_ns =
			      presented_time_s > 0.0 ? (int64_t)llround(presented_time_s * (double)U_TIME_1S_IN_NS) : 0;
			  int64_t presented_monotonic_ns =
			      presented_host_ns != 0 ? presented_host_ns + handler_offset_ns : 0;
			  int64_t presented_minus_desired_ns =
			      presented_monotonic_ns != 0 ? presented_monotonic_ns - traced_desired_present_ns : 0;
			  int64_t presented_minus_target_ns =
			      presented_monotonic_ns != 0 ? presented_monotonic_ns - (int64_t)traced_target_output_ns
				                          : 0;
			  int64_t observed_present_offset_ns =
			      presented_monotonic_ns != 0 ? presented_monotonic_ns - traced_desired_present_ns : 0;
			  /*
			   * Diagnostic equivalent of GAV's present-queue measurement:
			   * how many refresh periods after the compositor's chosen
			   * output slot did Core Animation actually present this
			   * drawable? 0 is on the selected slot, +1 is one refresh
			   * later, +2 identifies the persistent deep-queue condition
			   * seen by GAV. No active queue draining is performed here.
			   */
			  int64_t present_queue_depth = -1;
			  if (presented_monotonic_ns != 0 && traced_target_output_ns != 0 &&
			      traced_display_period_ns > 0) {
				  present_queue_depth = (int64_t)llround(
				      (double)(presented_monotonic_ns - (int64_t)traced_target_output_ns) /
				      (double)traced_display_period_ns);
			  }
			  if (observed_present_offset_ns > 0) {
				  atomic_store_explicit(&presented_state->latest_observed_present_offset_ns,
					                observed_present_offset_ns, memory_order_release);
				  atomic_fetch_add_explicit(&presented_state->present_offset_sample_serial, 1,
					                    memory_order_release);
			  }
			  flockfile(trace_file);
			  fprintf(trace_file,
				  "%llu,%" PRIi64 ",%" PRIi64 ",%llu,%.17g,%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64
				  ",%" PRIi64 "\n",
				  (unsigned long long)traced_frame_id, handler_ns, traced_desired_present_ns,
				  (unsigned long long)traced_target_output_ns, presented_time_s, presented_monotonic_ns,
				  presented_minus_desired_ns, presented_minus_target_ns, observed_present_offset_ns,
				  present_queue_depth);
			  macos_trace_buffered_fflush(trace_file);
			  funlockfile(trace_file);
			  macos_presented_state_release(presented_state);
			}];
		}

		int64_t prelatch_us = debug_get_num_option_macos_present_prelatch_us();
		if (prelatch_us < 0) {
			prelatch_us = 0;
		}
		uint64_t prelatch_ns = (uint64_t)prelatch_us * 1000ULL;
		metal_request_ns = target_output_ns > prelatch_ns ? target_output_ns - prelatch_ns : target_output_ns;
		scheduled_present_host_s = monotonic_ns_to_host_seconds(cwm, (int64_t)metal_request_ns);
		comp_macos_frontend_note_present(cwm->frontend, drawable);
		if (scheduled_present_host_s > 0.0) {
			macos_present_drawable_at_time(command_buffer, drawable, scheduled_present_host_s);
		} else {
			[command_buffer presentDrawable:drawable];
		}
		after_present_call_ns = os_monotonic_get_ns();
		uint64_t traced_frame_id = frame_id;
		uint32_t traced_index = index;
		struct macos_present_job source_job = *job;
		uint64_t traced_timeline_value = timeline_semaphore_value;
		uint64_t commit_begin_ns = os_monotonic_get_ns();
		bool traced_shared_event_wait = shared_event_wait;
		dispatch_group_t command_group = cwm->present_command_group;
		FILE *complete_trace = cwm->trace_present_complete;
		dispatch_group_enter(command_group);
		[command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed_buffer) {
		  uint64_t completion_ns = os_monotonic_get_ns();
		  MTLCommandBufferStatus status = [completed_buffer status];
		  double completed_gpu_start_s = [completed_buffer GPUStartTime];
		  double completed_gpu_end_s = [completed_buffer GPUEndTime];
		  if (status == MTLCommandBufferStatusError) {
			  COMP_ERROR(cwm->base.base.c, "Asynchronous Metal presentation failed: %s",
				     [[[completed_buffer error] localizedDescription] UTF8String]);
		  }
		  if (complete_trace != NULL) {
			  flockfile(complete_trace);
			  fprintf(complete_trace, "%llu,%llu,%u,%llu,%lu,%llu,%.17g,%.17g,%u\n",
				  (unsigned long long)traced_frame_id, (unsigned long long)completion_ns, traced_index,
				  (unsigned long long)traced_timeline_value, (unsigned long)status,
				  (unsigned long long)(completion_ns - commit_begin_ns), completed_gpu_start_s,
				  completed_gpu_end_s, traced_shared_event_wait ? 1u : 0u);
			  funlockfile(complete_trace);
		  }
		  macos_release_job_source(cwm, &source_job);
		  dispatch_group_leave(command_group);
		}];
		/* Passive observer of Metal scheduling. The timed-present convenience
		 * method has its own scheduled handler; this is not a timestamp of
		 * that private handler or of WindowServer accepting the drawable. */
		FILE *scheduled_trace = cwm->trace_present_scheduled;
		if (scheduled_trace != NULL) {
			uint64_t registered_ns = os_monotonic_get_ns();
			uint64_t minimum_duration_us = macos_present_min_duration_us();
			bool presents_with_transaction = [cwm->metal_layer presentsWithTransaction];
			dispatch_group_enter(command_group);
			[command_buffer addScheduledHandler:^(id<MTLCommandBuffer> scheduled_buffer) {
			  uint64_t scheduled_ns = os_monotonic_get_ns();
			  flockfile(scheduled_trace);
			  fprintf(scheduled_trace, "%llu,%llu,%llu,%u,%llu,%lu,%llu,%u\n",
				  (unsigned long long)traced_frame_id, (unsigned long long)scheduled_ns,
				  (unsigned long long)registered_ns, traced_index,
				  (unsigned long long)traced_timeline_value, (unsigned long)[scheduled_buffer status],
				  (unsigned long long)minimum_duration_us, presents_with_transaction ? 1u : 0u);
			  macos_trace_buffered_fflush(scheduled_trace);
			  funlockfile(scheduled_trace);
			  dispatch_group_leave(command_group);
			}];
		}
		[command_buffer commit];
		after_commit_ns = os_monotonic_get_ns();
		after_metal_wait_ns = after_commit_ns;
		macos_trace_present_worker(cwm, "submitted", job, after_commit_ns, 0, worker_start_ns,
		                           next_drawable_begin_ns, after_drawable_ns, after_commit_ns, 0,
		                           shared_event_wait);
		pthread_mutex_lock(&cwm->present_worker_mutex);
		cwm->worker_jobs_submitted++;
		uint64_t worker_delay_ns = worker_start_ns > job->enqueue_ns ? worker_start_ns - job->enqueue_ns : 0;
		uint64_t drawable_wait_ns =
		    after_drawable_ns > next_drawable_begin_ns ? after_drawable_ns - next_drawable_begin_ns : 0;
		cwm->worker_queue_delay_total_ns += worker_delay_ns;
		if (worker_delay_ns > cwm->worker_queue_delay_max_ns) {
			cwm->worker_queue_delay_max_ns = worker_delay_ns;
		}
		if (drawable_wait_ns > 5000000ULL) {
			cwm->worker_drawable_stalls++;
		}
		if (cwm->display_period_ns > 0 && drawable_wait_ns > (uint64_t)cwm->display_period_ns / 2) {
			cwm->worker_drawable_half_refresh_stalls++;
		}
		uint64_t submitted = cwm->worker_jobs_submitted;
		uint64_t superseded = cwm->worker_jobs_superseded;
		uint64_t half_refresh_stalls = cwm->worker_drawable_half_refresh_stalls;
		uint64_t stalls = cwm->worker_drawable_stalls;
		uint64_t average_delay_ns = submitted != 0 ? cwm->worker_queue_delay_total_ns / submitted : 0;
		uint64_t max_delay_ns = cwm->worker_queue_delay_max_ns;
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		if (submitted % 240 == 0) {
			COMP_INFO(
			    ct->c,
			    "macOS present worker: submitted %llu, superseded %llu, drawable >half-refresh %llu, >5ms "
			    "%llu, queue delay avg %.3fms max %.3fms",
			    (unsigned long long)submitted, (unsigned long long)superseded,
			    (unsigned long long)half_refresh_stalls, (unsigned long long)stalls,
			    (double)average_delay_ns / 1000000.0, (double)max_delay_ns / 1000000.0);
		}
		cwm->present_vk_wait_total_ns += after_vk_wait_ns - before_vk_wait_ns;
		cwm->present_drawable_wait_total_ns +=
		    after_drawable_ns > next_drawable_begin_ns ? after_drawable_ns - next_drawable_begin_ns : 0;
	}

	if (cwm->trace_present != NULL) {
		uint64_t latest_output_ns =
		    atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);
		fprintf(cwm->trace_present,
		        "%llu,%llu,%" PRIi64 ",%" PRIi64 ",%llu,%" PRIi64 ",%llu,%" PRIi64 ",%" PRIi64 ",%.17g,%" PRIi64
		        ",%u,%llu,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.17g,%.17g,%u,%u,%llu\n",
		        (unsigned long long)frame_id, (unsigned long long)host_call_ns, desired_present_time_ns,
		        desired_present_time_ns - (int64_t)host_call_ns, (unsigned long long)target_output_ns,
		        (int64_t)target_output_ns - desired_present_time_ns, (unsigned long long)metal_request_ns,
		        (int64_t)metal_request_ns - (int64_t)target_output_ns,
		        (int64_t)metal_request_ns - (int64_t)before_present_call_ns, scheduled_present_host_s,
		        present_slop_ns, index, (unsigned long long)timeline_semaphore_value, wait_mode,
		        (unsigned long long)after_vk_wait_ns, (unsigned long long)after_drawable_ns,
		        (unsigned long long)before_present_call_ns, (unsigned long long)after_present_call_ns,
		        (unsigned long long)after_commit_ns, (unsigned long long)after_metal_wait_ns,
		        (unsigned long long)latest_output_ns, gpu_start_time_s, gpu_end_time_s, 1u /* async_present */,
		        shared_event_wait ? 1u : 0u, (unsigned long long)image_reuse_wait_ns);
		cwm->trace_present_rows++;
		if (cwm->trace_present_rows % 256 == 0) {
			macos_trace_buffered_fflush(cwm->trace_present);
		}
	}

	uint64_t now_ns = os_monotonic_get_ns();
	if (cwm->last_present_ns != 0 && now_ns > cwm->last_present_ns) {
		uint64_t interval_ns = now_ns - cwm->last_present_ns;
		if (cwm->display_period_ns <= 0 || interval_ns <= (uint64_t)cwm->display_period_ns * 4) {
			cwm->present_total_ns += interval_ns;
			cwm->present_sample_count++;
			if (cwm->present_min_ns == 0 || interval_ns < cwm->present_min_ns) {
				cwm->present_min_ns = interval_ns;
			}
			if (interval_ns > cwm->present_max_ns) {
				cwm->present_max_ns = interval_ns;
			}
			if (cwm->display_period_ns > 0 && interval_ns > (uint64_t)cwm->display_period_ns * 3 / 2) {
				cwm->present_missed_intervals++;
			}
		}
	}
	cwm->last_present_ns = now_ns;
	if (cwm->present_sample_count == 240) {
		double average_ms = (double)cwm->present_total_ns / (double)cwm->present_sample_count / 1000000.0;
		const char *cadence_label = cwm->present_worker_enabled ? "macOS present-worker completion cadence"
		                                                        : "macOS async present completion cadence";
		COMP_INFO(ct->c, "%s: average %.3fms, min %.3fms, max %.3fms, late %llu/240", cadence_label, average_ms,
		          (double)cwm->present_min_ns / 1000000.0, (double)cwm->present_max_ns / 1000000.0,
		          (unsigned long long)cwm->present_missed_intervals);
		COMP_INFO(ct->c, "macOS presentation CPU waits: Vulkan %.3fms, drawable %.3fms",
		          (double)cwm->present_vk_wait_total_ns / 240.0 / 1000000.0,
		          (double)cwm->present_drawable_wait_total_ns / 240.0 / 1000000.0);
		cwm->present_sample_count = 0;
		cwm->present_total_ns = 0;
		cwm->present_min_ns = 0;
		cwm->present_max_ns = 0;
		cwm->present_missed_intervals = 0;
		cwm->present_vk_wait_total_ns = 0;
		cwm->present_drawable_wait_total_ns = 0;
	}
	return VK_SUCCESS;
}

static void
macos_retire_unpresented_job(struct comp_window_macos *cwm,
                             const struct macos_present_job *job,
                             const char *trace_event,
                             uint32_t queue_depth)
{
	struct macos_present_job retired_job = *job;
	bool shared_event_wait = cwm->render_complete_event != nil;
	uint64_t event_ns = os_monotonic_get_ns();
	macos_trace_present_worker(cwm, trace_event, &retired_job, event_ns, 0, 0, 0, 0, 0, queue_depth,
	                           shared_event_wait);
	if (retired_job.external_texture != nil) {
		// Complete before it was handed over, and never encoded here.
		macos_release_job_source(cwm, &retired_job);
		return;
	}

	/*
	 * Dropping presentation does not mean Vulkan has stopped writing the source.
	 * Retire it behind the captured render-complete value before making it
	 * acquirable again. In drawable-slot mode use a global queue: the serial
	 * slot/present queue may itself be blocked in nextDrawable, and retirement
	 * must not wait behind that acquisition before returning source images.
	 */
	dispatch_group_enter(cwm->present_command_group);
	dispatch_queue_t retirement_queue =
	    cwm->present_worker_enabled && !cwm->drawable_slot_enabled ? cwm->present_worker_queue : NULL;
	if (retirement_queue == NULL) {
		retirement_queue = dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0);
	}
	dispatch_async(retirement_queue, ^{
	  @autoreleasepool {
		  bool released_by_metal = false;
		  if (shared_event_wait) {
			  id<MTLCommandBuffer> command_buffer = [cwm->present_queue commandBuffer];
			  if (command_buffer != nil) {
				  [command_buffer encodeWaitForEvent:cwm->render_complete_event
					                       value:retired_job.timeline_value];
				  [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed_buffer) {
				    if ([completed_buffer status] == MTLCommandBufferStatusError) {
					    COMP_ERROR(cwm->base.base.c, "Metal retirement wait failed: %s",
						       [[[completed_buffer error] localizedDescription] UTF8String]);
				    }
				    macos_release_source_image(cwm, retired_job.image_index);
				    dispatch_group_leave(cwm->present_command_group);
				  }];
				  [command_buffer commit];
				  released_by_metal = true;
			  }
		  }

		  if (!released_by_metal) {
			  struct comp_target *ct = &cwm->base.base;
			  struct vk_bundle *vk = get_vk(cwm);
			  VkResult ret = VK_SUCCESS;
			  if (ct->semaphores.render_complete != VK_NULL_HANDLE &&
			      ct->semaphores.render_complete_is_timeline && vk->vkWaitSemaphores != NULL) {
				  VkSemaphoreWaitInfo wait_info = {
				      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
				      .semaphoreCount = 1,
				      .pSemaphores = &ct->semaphores.render_complete,
				      .pValues = &retired_job.timeline_value,
				  };
				  ret = vk->vkWaitSemaphores(vk->device, &wait_info, UINT64_MAX);
			  } else {
				  vk_queue_lock(retired_job.present_queue);
				  ret = vk->vkQueueWaitIdle(retired_job.present_queue->queue);
				  vk_queue_unlock(retired_job.present_queue);
			  }
			  if (ret != VK_SUCCESS) {
				  COMP_ERROR(ct->c, "Vulkan wait while retiring a dropped macOS present job: %s",
					     vk_result_string(ret));
			  }
			  macos_release_source_image(cwm, retired_job.image_index);
			  dispatch_group_leave(cwm->present_command_group);
		  }
	  }
	});
}

static void
macos_present_worker_run_one(struct comp_window_macos *cwm)
{
	struct macos_present_job job;
	bool need_slot = false;
	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (cwm->present_worker_shutdown || !cwm->pending_present_job_valid) {
		cwm->present_worker_scheduled = false;
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		dispatch_group_leave(cwm->present_worker_group);
		return;
	}
	if (cwm->drawable_slot_enabled && cwm->prefetched_drawable == nil) {
		/*
		 * Leave the newest job pending until the asynchronous nextDrawable
		 * finishes. New frames may continue replacing it in the meantime.
		 */
		cwm->present_worker_scheduled = false;
		need_slot = true;
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		dispatch_group_leave(cwm->present_worker_group);
		if (need_slot) {
			macos_schedule_drawable_slot(cwm);
		}
		return;
	}
	job = cwm->pending_present_job;
	cwm->pending_present_job_valid = false;
	pthread_mutex_unlock(&cwm->present_worker_mutex);

	(void)macos_execute_present_job(cwm, &job);

	/* Schedule one job at a time so already-queued dropped-image retirement work cannot starve. */
	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (!cwm->present_worker_shutdown && cwm->pending_present_job_valid) {
		dispatch_async(cwm->present_worker_queue, ^{
		  macos_present_worker_run_one(cwm);
		});
	} else {
		cwm->present_worker_scheduled = false;
		dispatch_group_leave(cwm->present_worker_group);
	}
	pthread_mutex_unlock(&cwm->present_worker_mutex);
}

static VkResult
macos_enqueue_present_job(struct comp_window_macos *cwm, const struct macos_present_job *in_job);

static VkResult
comp_window_macos_present(struct comp_target *ct,
                          struct vk_bundle_queue *present_queue,
                          uint32_t index,
                          uint64_t timeline_semaphore_value,
                          int64_t desired_present_time_ns,
                          int64_t present_slop_ns)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	struct macos_present_job job = {
	    .frame_id = ++cwm->trace_frame_id,
	    .enqueue_ns = os_monotonic_get_ns(),
	    .image_reuse_wait_ns = cwm->last_image_acquire_wait_ns,
	    .image_index = index,
	    .timeline_value = timeline_semaphore_value,
	    .desired_present_time_ns = desired_present_time_ns,
	    .present_slop_ns = present_slop_ns,
	    .present_queue = present_queue,
	    .passthrough_active = ct->c->passthrough_active,
	    .passthrough_has_application_layers = ct->c->passthrough_has_application_layers,
	};
	if (!cwm->present_worker_enabled) {
		/* Submit on the compositor thread; async GPU completion does not need a worker. */
		return macos_execute_present_job(cwm, &job);
	}
	if (index >= ct->image_count || cwm->metal_images[index] == nil || present_queue == NULL) {
		macos_retire_unpresented_job(cwm, &job, "invalid", 0);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	return macos_enqueue_present_job(cwm, &job);
}

static VkResult
comp_window_macos_present_external(struct comp_target *ct,
                                   const struct comp_target_external_image *image,
                                   int64_t desired_present_time_ns,
                                   int64_t present_slop_ns)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	id<MTLTexture> texture = (id<MTLTexture>)image->native_texture;
	if (texture == nil || cwm->present_copy == NULL) {
		image->release(image->release_data);
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	struct macos_present_job job = {
	    .frame_id = ++cwm->trace_frame_id,
	    .enqueue_ns = os_monotonic_get_ns(),
	    .image_index = UINT32_MAX,
	    .desired_present_time_ns = desired_present_time_ns,
	    .present_slop_ns = present_slop_ns,
	    .external_texture = [texture retain],
	    .external_release = image->release,
	    .external_release_data = image->release_data,
	};
	if (!cwm->logged_external_present) {
		cwm->logged_external_present = true;
		COMP_INFO(ct->c, "Presenting an externally composited %lux%lu image (format %lu, %s)",
		          (unsigned long)[texture width], (unsigned long)[texture height],
		          (unsigned long)[texture pixelFormat],
		          comp_macos_present_copy_needs_draw(texture, cwm->metal_images[0]) ? "drawn" : "blitted");
	}
	if (!cwm->present_worker_enabled) {
		return macos_execute_present_job(cwm, &job);
	}
	return macos_enqueue_present_job(cwm, &job);
}

//! Make @p job the single pending job of the present worker.
static VkResult
macos_enqueue_present_job(struct comp_window_macos *cwm, const struct macos_present_job *in_job)
{
	struct macos_present_job job = *in_job;
	struct macos_present_job superseded_job;
	bool superseded = false;
	bool schedule_present = false;
	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (cwm->present_worker_shutdown) {
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		macos_retire_unpresented_job(cwm, &job, "shutdown_drop", 0);
		return VK_ERROR_DEVICE_LOST;
	}
	if (cwm->pending_present_job_valid) {
		/* Keep one pending job: replace it with the newest completed Vulkan submission. */
		superseded_job = cwm->pending_present_job;
		superseded = true;
		cwm->worker_jobs_superseded++;
	}
	cwm->pending_present_job = job;
	cwm->pending_present_job_valid = true;
	cwm->worker_jobs_enqueued++;
	if (!cwm->present_worker_scheduled && (!cwm->drawable_slot_enabled || cwm->prefetched_drawable != nil)) {
		cwm->present_worker_scheduled = true;
		dispatch_group_enter(cwm->present_worker_group);
		schedule_present = true;
	}
	pthread_mutex_unlock(&cwm->present_worker_mutex);

	if (schedule_present) {
		dispatch_async(cwm->present_worker_queue, ^{
		  macos_present_worker_run_one(cwm);
		});
	}
	if (cwm->drawable_slot_enabled) {
		macos_schedule_drawable_slot(cwm);
	}
	if (superseded) {
		macos_retire_unpresented_job(cwm, &superseded_job, "superseded", 1);
	}
	uint64_t handoff_return_ns = os_monotonic_get_ns();
	macos_trace_present_worker(cwm, "enqueued", &job, handoff_return_ns, handoff_return_ns, 0, 0, 0, 0, 1,
	                           cwm->render_complete_event != nil);
	return VK_SUCCESS;
}

static VkResult
comp_window_macos_wait_for_present(struct comp_target *ct, time_duration_ns timeout_ns)
{
	(void)ct;
	(void)timeout_ns;
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VkResult
comp_window_macos_update_timings(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	int64_t observed_period_ns = atomic_load_explicit(&cwm->latest_displaylink_period_ns, memory_order_acquire);
	// Ignore nanosecond conversion noise, and update the existing pacer so outstanding
	// frame IDs and feedback remain valid. The presentation worker reads this atomically.
	if (observed_period_ns > 0 && llabs(observed_period_ns - cwm->display_period_ns) > 100 &&
	    cwm->base.upc != NULL) {
		u_pc_fake_set_frame_period(cwm->base.upc, observed_period_ns);
		cwm->display_period_ns = observed_period_ns;
		ct->c->frame_interval_ns = observed_period_ns;
		COMP_INFO(ct->c, "CADisplayLink measured display period %.6fms (%.3f Hz)",
		          (double)observed_period_ns / 1000000.0, (double)U_TIME_1S_IN_NS / observed_period_ns);
	}
	macos_schedule_drawable_slot(cwm);
	uint64_t vblank_ns = atomic_exchange_explicit(&cwm->latest_vblank_ns, 0, memory_order_acquire);
	uint64_t displaylink_now_host_ns =
	    atomic_load_explicit(&cwm->latest_displaylink_now_host_ns, memory_order_acquire);
	uint64_t displaylink_output_host_ns =
	    atomic_load_explicit(&cwm->latest_displaylink_output_host_ns, memory_order_acquire);
	uint64_t displaylink_now_ns = atomic_load_explicit(&cwm->latest_displaylink_now_ns, memory_order_acquire);
	uint64_t displaylink_output_ns = atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);
	uint64_t displaylink_callback_ns =
	    atomic_load_explicit(&cwm->latest_displaylink_callback_ns, memory_order_acquire);
	int64_t host_to_monotonic_offset_ns =
	    atomic_load_explicit(&cwm->host_to_monotonic_offset_ns, memory_order_acquire);

	uint64_t present_offset_serial = 0;
	int64_t sample_ns = 0;
	if (cwm->presented_state != NULL) {
		present_offset_serial =
		    atomic_load_explicit(&cwm->presented_state->present_offset_sample_serial, memory_order_acquire);
		sample_ns = atomic_load_explicit(&cwm->presented_state->latest_observed_present_offset_ns,
		                                 memory_order_acquire);
	}
	if (present_offset_serial != cwm->consumed_present_offset_sample_serial && cwm->display_period_ns > 0) {
		cwm->consumed_present_offset_sample_serial = present_offset_serial;
		int64_t max_reasonable_ns = cwm->display_period_ns * 4;
		if (sample_ns > 0 && sample_ns <= max_reasonable_ns) {
			if (cwm->present_offset_sample_count == 0) {
				cwm->calibrated_present_offset_ns = sample_ns;
			} else {
				// 1/8 EMA: stable enough to reject callback/clock-conversion noise while adapting
				// quickly.
				cwm->calibrated_present_offset_ns =
				    (cwm->calibrated_present_offset_ns * 7 + sample_ns) / 8;
			}
			if (cwm->present_offset_sample_count < UINT32_MAX) {
				cwm->present_offset_sample_count++;
			}
			if (cwm->present_offset_sample_count >= 8 && cwm->base.upc != NULL) {
				u_pc_update_present_offset(cwm->base.upc, 0, cwm->calibrated_present_offset_ns);
			}
		}
	}
	if (vblank_ns == 0 || vblank_ns == cwm->last_vblank_ns || cwm->base.upc == NULL) {
		return VK_SUCCESS;
	}

	uint64_t consumed_ns = os_monotonic_get_ns();
	uint64_t previous_vblank_ns = cwm->last_vblank_ns;
	if (cwm->trace_vblank != NULL) {
		fprintf(cwm->trace_vblank,
		        "%llu,%llu,%llu,%llu,%" PRIi64 ",%llu,%llu,%llu,%" PRIi64 ",%" PRIi64 ",%" PRIi64
		        ",%llu,%" PRIi64 "\n",
		        (unsigned long long)consumed_ns, (unsigned long long)displaylink_callback_ns,
		        (unsigned long long)displaylink_now_host_ns, (unsigned long long)displaylink_output_host_ns,
		        host_to_monotonic_offset_ns, (unsigned long long)displaylink_now_ns,
		        (unsigned long long)displaylink_output_ns, (unsigned long long)vblank_ns,
		        (int64_t)displaylink_output_ns - (int64_t)displaylink_now_ns,
		        (int64_t)displaylink_callback_ns - (int64_t)displaylink_output_ns,
		        (int64_t)displaylink_callback_ns - (int64_t)vblank_ns,
		        previous_vblank_ns != 0 && vblank_ns > previous_vblank_ns
		            ? (unsigned long long)(vblank_ns - previous_vblank_ns)
			    : 0ULL,
		        cwm->display_period_ns);
		cwm->trace_vblank_rows++;
		if (cwm->trace_vblank_rows % 256 == 0) {
			macos_trace_buffered_fflush(cwm->trace_vblank);
		}
	}

	u_pc_update_vblank_from_display_control(cwm->base.upc, (int64_t)vblank_ns);
	if (cwm->last_vblank_ns != 0 && vblank_ns > cwm->last_vblank_ns) {
		uint64_t interval_ns = vblank_ns - cwm->last_vblank_ns;
		if (cwm->display_period_ns > 0 && interval_ns > (uint64_t)cwm->display_period_ns * 4) {
			cwm->last_vblank_ns = vblank_ns;
			return VK_SUCCESS;
		}
		cwm->cadence_total_ns += interval_ns;
		cwm->cadence_sample_count++;
		if (cwm->cadence_min_ns == 0 || interval_ns < cwm->cadence_min_ns) {
			cwm->cadence_min_ns = interval_ns;
		}
		if (interval_ns > cwm->cadence_max_ns) {
			cwm->cadence_max_ns = interval_ns;
		}

		if (cwm->cadence_sample_count == 240) {
			double average_ms =
			    (double)cwm->cadence_total_ns / (double)cwm->cadence_sample_count / 1000000.0;
			COMP_INFO(ct->c, "PS VR2 display-link cadence: average %.3fms, min %.3fms, max %.3fms",
			          average_ms, (double)cwm->cadence_min_ns / 1000000.0,
			          (double)cwm->cadence_max_ns / 1000000.0);
			cwm->cadence_sample_count = 0;
			cwm->cadence_total_ns = 0;
			cwm->cadence_min_ns = 0;
			cwm->cadence_max_ns = 0;
		}
	}
	cwm->last_vblank_ns = vblank_ns;
	return VK_SUCCESS;
}

static xrt_result_t
comp_window_macos_get_refresh_rates(struct comp_target *ct, uint32_t *out_count, float *out_rates)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	int64_t period_ns = cwm->display_period_ns > 0 ? cwm->display_period_ns : ct->c->frame_interval_ns;
	*out_count = 1;
	out_rates[0] = (float)((double)U_TIME_1S_IN_NS / (double)period_ns);
	return XRT_SUCCESS;
}

static xrt_result_t
comp_window_macos_get_current_refresh_rate(struct comp_target *ct, float *out_rate)
{
	uint32_t count = 0;
	return comp_window_macos_get_refresh_rates(ct, &count, out_rate);
}

static VkResult
comp_window_macos_queue_supports_present(struct comp_target *ct, struct vk_bundle_queue *queue, VkBool32 *out_supported)
{
	(void)ct;
	(void)queue;
	*out_supported = VK_TRUE;
	return VK_SUCCESS;
}

static void
comp_window_macos_flush(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	@autoreleasepool {
		[CATransaction flush];
		if (!cwm->logged_layer_state) {
			CGSize drawable_size = [cwm->metal_layer drawableSize];
			COMP_INFO(
			    ct->c,
			    "macOS presentation: %s front-end visible=%s layer device=%s format=%lu drawable=%.0fx%.0f",
			    comp_macos_frontend_name(cwm->frontend),
			    comp_macos_frontend_is_visible(cwm->frontend) ? "true" : "false",
			    [cwm->metal_layer device] != nil ? "set" : "nil",
			    (unsigned long)[cwm->metal_layer pixelFormat], drawable_size.width, drawable_size.height);
			cwm->logged_layer_state = true;
		}
	}
}

static void
comp_window_macos_set_title(struct comp_target *ct, const char *title)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	@autoreleasepool {
		comp_macos_frontend_set_title(cwm->frontend, title);
	}
}

static void
comp_window_macos_destroy(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;

	atomic_store_explicit(&cwm->passthrough_shutdown, true, memory_order_release);
	struct xrt_device *xdev = ct->c != NULL ? ct->c->xdev : NULL;
	if (cwm->passthrough_sinks_attached && xdev != NULL && xdev->set_passthrough_sinks != NULL) {
		(void)xdev->set_passthrough_sinks(xdev, NULL, NULL);
		cwm->passthrough_sinks_attached = false;
		if (cwm->hosted_client == NULL) {
			u_passthrough_share_set_source_available(false);
		}
	}
	pthread_mutex_lock(&cwm->passthrough_mutex);
	for (uint32_t eye = 0; eye < 2; eye++) {
		xrt_frame_reference(&cwm->passthrough_frames[eye], NULL);
	}
	pthread_mutex_unlock(&cwm->passthrough_mutex);
	macos_display_link_destroy(cwm);
	if (cwm->present_worker_queue != NULL) {
		struct macos_present_job pending_job;
		bool had_pending_job = false;
		pthread_mutex_lock(&cwm->present_worker_mutex);
		cwm->present_worker_shutdown = true;
		if (cwm->pending_present_job_valid) {
			pending_job = cwm->pending_present_job;
			cwm->pending_present_job_valid = false;
			had_pending_job = true;
		}
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		if (had_pending_job) {
			macos_retire_unpresented_job(cwm, &pending_job, "shutdown_drop", 0);
		}
	}
	macos_drain_present_worker(cwm);
	macos_release_prefetched_drawable(cwm, "destroy_release");
	macos_timing_trace_close(cwm);
	comp_window_macos_free_images(cwm);
	if (cwm->render_complete_event != nil) {
		[cwm->render_complete_event release];
		cwm->render_complete_event = nil;
	}
	if (ct->semaphores.render_complete != VK_NULL_HANDLE) {
		struct vk_bundle *vk = get_vk(cwm);
		vk->vkDestroySemaphore(vk->device, ct->semaphores.render_complete, NULL);
		ct->semaphores.render_complete = VK_NULL_HANDLE;
		ct->semaphores.render_complete_is_timeline = false;
	}
	u_pc_destroy(&cwm->base.upc);
	@autoreleasepool {
		if (cwm->frontend != NULL) {
			comp_macos_frontend_destroy(&cwm->frontend);
		}
		[cwm->present_queue release];
		comp_macos_present_copy_destroy(&cwm->present_copy);
		for (uint32_t eye = 0; eye < 2; eye++) {
			[cwm->passthrough_camera_textures[eye] release];
			cwm->passthrough_camera_textures[eye] = nil;
			[cwm->passthrough_uv_maps[eye] release];
			cwm->passthrough_uv_maps[eye] = nil;
		}
		[cwm->passthrough_pipeline release];
		cwm->passthrough_pipeline = nil;
	}
	u_macos_hosted_client_release(cwm->hosted_client);
	pthread_mutex_destroy(&cwm->passthrough_mutex);
	pthread_mutex_destroy(&cwm->present_worker_mutex);
	free(cwm);
}

static struct comp_target *
comp_window_macos_create_base(struct comp_compositor *c, struct u_macos_hosted_client *client)
{
	struct comp_window_macos *cwm = U_TYPED_CALLOC(struct comp_window_macos);
	if (cwm == NULL) {
		return NULL;
	}
	if (pthread_mutex_init(&cwm->present_worker_mutex, NULL) != 0) {
		free(cwm);
		return NULL;
	}
	if (pthread_mutex_init(&cwm->passthrough_mutex, NULL) != 0) {
		pthread_mutex_destroy(&cwm->present_worker_mutex);
		free(cwm);
		return NULL;
	}
	atomic_init(&cwm->passthrough_shutdown, false);
	/* Hosted when this is a client whose IPC connection can ask the service to show it. */
	cwm->hosted_client = client;
	u_macos_hosted_client_reference(client);
	bool want_drawable_slot = debug_get_bool_option_macos_drawable_slot();
	cwm->drawable_slot_enabled = want_drawable_slot;
	/*
	 * Slot mode is itself a newest-frame presentation worker. The producer may
	 * keep rendering and replace the single pending job while asynchronous
	 * nextDrawable acquisition blocks; a missing slot is no longer a reason to
	 * drop the just-rendered frame on the caller thread.
	 */
	cwm->present_worker_enabled = cwm->drawable_slot_enabled;
	cwm->present_command_group = dispatch_group_create();
	if (cwm->present_worker_enabled) {
		dispatch_queue_attr_t worker_attr =
		    dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
		cwm->present_worker_queue =
		    dispatch_queue_create(cwm->drawable_slot_enabled ? "org.monado.macos-drawable-slot-newest"
		                                                     : "org.monado.macos-present-worker",
		                          worker_attr);
		cwm->present_worker_group = dispatch_group_create();
	}
	if (cwm->drawable_slot_enabled) {
		COMP_INFO(c,
		          "macOS diagnostic: asynchronous drawable slot with newest-frame worker enabled; nextDrawable "
		          "stalls supersede pending frames instead of dropping them");
	}
	macos_timing_trace_open(cwm);
	comp_target_swapchain_init_and_set_fnptrs(&cwm->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);
	for (uint32_t i = 0; i < MACOS_TARGET_IMAGE_COUNT; i++) {
		cwm->io_surfaces[i] = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	}
	cwm->base.base.name = "macOS Metal";
	cwm->base.display = VK_NULL_HANDLE;
	cwm->base.base.destroy = comp_window_macos_destroy;
	cwm->base.base.flush = comp_window_macos_flush;
	cwm->base.base.init_pre_vulkan = comp_window_macos_init;
	cwm->base.base.init_post_vulkan = comp_window_macos_init_vulkan;
	cwm->base.base.check_ready = comp_window_macos_check_ready;
	cwm->base.base.create_images = comp_window_macos_create_images;
	cwm->base.base.has_images = comp_window_macos_has_images;
	cwm->base.base.acquire = comp_window_macos_acquire;
	cwm->base.base.present = comp_window_macos_present;
	cwm->base.base.present_external = comp_window_macos_present_external;
	cwm->base.base.wait_for_present = comp_window_macos_wait_for_present;
	cwm->base.base.update_timings = comp_window_macos_update_timings;
	cwm->base.base.queue_supports_present = comp_window_macos_queue_supports_present;
	cwm->base.base.set_title = comp_window_macos_set_title;
	cwm->base.base.get_refresh_rates = comp_window_macos_get_refresh_rates;
	cwm->base.base.get_current_refresh_rate = comp_window_macos_get_current_refresh_rate;
	cwm->base.base.wait_for_present_supported = false;
	cwm->base.base.c = c;
	return &cwm->base.base;
}

static bool
detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	(void)ctf;
	(void)c;
	@autoreleasepool {
		return comp_macos_frontend_detect();
	}
}

static const char *macos_optional_device_extensions[] = {
    VK_EXT_METAL_OBJECTS_EXTENSION_NAME,
};

#pragma clang diagnostic pop


/*
 *
 * Physical refresh-rate switching and target creation.
 *
 */

static void
macos_log_refresh_mode_candidates(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	CGDirectDisplayID display_id = cwm->display_id;
	if (display_id == kCGNullDirectDisplay) {
		COMP_WARN(ct->c, "Could not enumerate PS VR2 refresh modes: display ID is unavailable");
		return;
	}

	CGDisplayModeRef current_mode = CGDisplayCopyDisplayMode(display_id);
	if (current_mode == NULL) {
		COMP_WARN(ct->c, "Could not enumerate PS VR2 refresh modes: current display mode is unavailable");
		return;
	}

	size_t current_width = CGDisplayModeGetWidth(current_mode);
	size_t current_height = CGDisplayModeGetHeight(current_mode);
	size_t current_pixel_width = CGDisplayModeGetPixelWidth(current_mode);
	size_t current_pixel_height = CGDisplayModeGetPixelHeight(current_mode);
	double current_refresh_hz = CGDisplayModeGetRefreshRate(current_mode);
	COMP_INFO(ct->c, "PS VR2 current CoreGraphics mode: logical %zux%zu, pixels %zux%zu, refresh %.3f Hz",
	          current_width, current_height, current_pixel_width, current_pixel_height, current_refresh_hz);

	CFArrayRef modes = CGDisplayCopyAllDisplayModes(display_id, NULL);
	if (modes == NULL) {
		CGDisplayModeRelease(current_mode);
		COMP_WARN(ct->c, "Could not enumerate CoreGraphics display modes for PS VR2");
		return;
	}

	float refresh_rates[XRT_MAX_SUPPORTED_REFRESH_RATES] = {0};
	uint32_t refresh_rate_count = 0;
	CFIndex mode_count = CFArrayGetCount(modes);
	for (CFIndex i = 0; i < mode_count; i++) {
		CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
		if (mode == NULL || CGDisplayModeGetWidth(mode) != current_width ||
		    CGDisplayModeGetHeight(mode) != current_height ||
		    CGDisplayModeGetPixelWidth(mode) != current_pixel_width ||
		    CGDisplayModeGetPixelHeight(mode) != current_pixel_height) {
			continue;
		}

		double refresh_hz = CGDisplayModeGetRefreshRate(mode);
		if (!(refresh_hz > 1.0)) {
			continue;
		}

		COMP_INFO(ct->c, "PS VR2 matching CoreGraphics display mode: %.3f Hz", refresh_hz);
		bool duplicate = false;
		for (uint32_t j = 0; j < refresh_rate_count; j++) {
			if (fabs((double)refresh_rates[j] - refresh_hz) < 0.05) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate && refresh_rate_count < XRT_MAX_SUPPORTED_REFRESH_RATES) {
			refresh_rates[refresh_rate_count++] = (float)refresh_hz;
		}
	}

	for (uint32_t i = 1; i < refresh_rate_count; i++) {
		float value = refresh_rates[i];
		uint32_t j = i;
		while (j > 0 && refresh_rates[j - 1] > value) {
			refresh_rates[j] = refresh_rates[j - 1];
			j--;
		}
		refresh_rates[j] = value;
	}

	if (refresh_rate_count == 0) {
		COMP_WARN(ct->c,
		          "PS VR2 CoreGraphics mode enumeration found no positive refresh rates matching the active "
		          "geometry");
	} else {
		char summary[256] = {0};
		size_t used = 0;
		for (uint32_t i = 0; i < refresh_rate_count && used < sizeof(summary); i++) {
			int written = snprintf(summary + used, sizeof(summary) - used, "%s%.3f", i == 0 ? "" : ", ",
			                       (double)refresh_rates[i]);
			if (written < 0 || (size_t)written >= sizeof(summary) - used) {
				break;
			}
			used += (size_t)written;
		}
		COMP_INFO(ct->c, "PS VR2 candidate physical refresh rates for active mode: [%s] Hz", summary);
	}

	CFRelease(modes);
	CGDisplayModeRelease(current_mode);
}

static uint32_t
macos_collect_refresh_rates(struct comp_window_macos *cwm, float *out_rates)
{
	CGDirectDisplayID display_id = cwm->display_id;
	if (display_id == kCGNullDirectDisplay) {
		return 0;
	}
	CGDisplayModeRef current_mode = CGDisplayCopyDisplayMode(display_id);
	if (current_mode == NULL) {
		return 0;
	}
	size_t width = CGDisplayModeGetWidth(current_mode);
	size_t height = CGDisplayModeGetHeight(current_mode);
	size_t pixel_width = CGDisplayModeGetPixelWidth(current_mode);
	size_t pixel_height = CGDisplayModeGetPixelHeight(current_mode);
	CGDisplayModeRelease(current_mode);

	CFArrayRef modes = CGDisplayCopyAllDisplayModes(display_id, NULL);
	if (modes == NULL) {
		return 0;
	}
	uint32_t count = 0;
	for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
		CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
		if (mode == NULL || CGDisplayModeGetWidth(mode) != width || CGDisplayModeGetHeight(mode) != height ||
		    CGDisplayModeGetPixelWidth(mode) != pixel_width ||
		    CGDisplayModeGetPixelHeight(mode) != pixel_height) {
			continue;
		}
		double refresh_hz = CGDisplayModeGetRefreshRate(mode);
		if (!(refresh_hz > 1.0)) {
			continue;
		}
		bool duplicate = false;
		for (uint32_t j = 0; j < count; j++) {
			if (fabs((double)out_rates[j] - refresh_hz) < 0.05) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate && count < XRT_MAX_SUPPORTED_REFRESH_RATES) {
			out_rates[count++] = (float)refresh_hz;
		}
	}
	CFRelease(modes);
	for (uint32_t i = 1; i < count; i++) {
		float value = out_rates[i];
		uint32_t j = i;
		while (j > 0 && out_rates[j - 1] > value) {
			out_rates[j] = out_rates[j - 1];
			j--;
		}
		out_rates[j] = value;
	}
	return count;
}

static CGDisplayModeRef
macos_copy_refresh_mode(struct comp_window_macos *cwm, float requested_hz, float *out_selected_hz)
{
	CGDirectDisplayID display_id = cwm->display_id;
	CGDisplayModeRef current_mode =
	    display_id != kCGNullDirectDisplay ? CGDisplayCopyDisplayMode(display_id) : NULL;
	if (current_mode == NULL) {
		return NULL;
	}
	size_t width = CGDisplayModeGetWidth(current_mode);
	size_t height = CGDisplayModeGetHeight(current_mode);
	size_t pixel_width = CGDisplayModeGetPixelWidth(current_mode);
	size_t pixel_height = CGDisplayModeGetPixelHeight(current_mode);
	CGDisplayModeRelease(current_mode);

	CFArrayRef modes = CGDisplayCopyAllDisplayModes(display_id, NULL);
	if (modes == NULL) {
		return NULL;
	}
	CGDisplayModeRef selected = NULL;
	double selected_hz = 0.0;
	double best_error = HUGE_VAL;
	for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
		CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
		if (mode == NULL || CGDisplayModeGetWidth(mode) != width || CGDisplayModeGetHeight(mode) != height ||
		    CGDisplayModeGetPixelWidth(mode) != pixel_width ||
		    CGDisplayModeGetPixelHeight(mode) != pixel_height) {
			continue;
		}
		double refresh_hz = CGDisplayModeGetRefreshRate(mode);
		if (!(refresh_hz > 1.0)) {
			continue;
		}
		if (requested_hz <= 0.0f) {
			if (refresh_hz > selected_hz) {
				selected = mode;
				selected_hz = refresh_hz;
			}
		} else {
			double error = fabs(refresh_hz - (double)requested_hz);
			if (error < best_error) {
				best_error = error;
				selected = mode;
				selected_hz = refresh_hz;
			}
		}
	}
	if (selected != NULL && (requested_hz <= 0.0f || best_error < 0.5)) {
		CGDisplayModeRetain(selected);
	} else {
		selected = NULL;
	}
	CFRelease(modes);
	if (out_selected_hz != NULL) {
		*out_selected_hz = (float)selected_hz;
	}
	return selected;
}

static xrt_result_t
comp_window_macos_get_refresh_rates_physical(struct comp_target *ct, uint32_t *out_count, float *out_rates)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	uint32_t count = macos_collect_refresh_rates(cwm, out_rates);
	if (count == 0) {
		return comp_window_macos_get_refresh_rates(ct, out_count, out_rates);
	}
	*out_count = count;
	return XRT_SUCCESS;
}

static xrt_result_t
comp_window_macos_get_current_refresh_rate_physical(struct comp_target *ct, float *out_rate)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	CGDirectDisplayID display_id = cwm->display_id;
	CGDisplayModeRef mode = display_id != kCGNullDirectDisplay ? CGDisplayCopyDisplayMode(display_id) : NULL;
	if (mode != NULL) {
		double refresh_hz = CGDisplayModeGetRefreshRate(mode);
		CGDisplayModeRelease(mode);
		if (refresh_hz > 1.0) {
			*out_rate = (float)refresh_hz;
			return XRT_SUCCESS;
		}
	}
	return comp_window_macos_get_current_refresh_rate(ct, out_rate);
}

static bool
macos_recreate_display_link(struct comp_window_macos *cwm, CGDirectDisplayID display_id, bool start_link)
{
	if (!macos_display_link_create(cwm, display_id)) {
		return false;
	}
	refresh_host_to_monotonic_offset_ns(cwm);
	atomic_store_explicit(&cwm->latest_vblank_ns, 0, memory_order_release);
	atomic_store_explicit(&cwm->latest_displaylink_now_host_ns, 0, memory_order_release);
	atomic_store_explicit(&cwm->latest_displaylink_output_host_ns, 0, memory_order_release);
	atomic_store_explicit(&cwm->latest_displaylink_now_ns, 0, memory_order_release);
	atomic_store_explicit(&cwm->latest_displaylink_output_ns, 0, memory_order_release);
	atomic_store_explicit(&cwm->latest_displaylink_callback_ns, 0, memory_order_release);
	atomic_store_explicit(&cwm->latest_displaylink_period_ns, 0, memory_order_release);
	cwm->last_vblank_ns = 0;
	cwm->cadence_sample_count = 0;
	cwm->cadence_total_ns = 0;
	cwm->cadence_min_ns = 0;
	cwm->cadence_max_ns = 0;
	cwm->present_offset_sample_count = 0;
	cwm->calibrated_present_offset_ns = 0;
	cwm->consumed_present_offset_sample_serial = 0;
	if (start_link && !macos_display_link_start(cwm)) {
		return false;
	}
	return true;
}

static xrt_result_t
comp_window_macos_request_refresh_rate_physical(struct comp_target *ct, float requested_hz)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	CGDirectDisplayID display_id = cwm->display_id;
	if (display_id == kCGNullDirectDisplay) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	float selected_hz = 0.0f;
	CGDisplayModeRef selected_mode = macos_copy_refresh_mode(cwm, requested_hz, &selected_hz);
	if (selected_mode == NULL) {
		COMP_WARN(ct->c, "PS VR2 refresh request %.3f Hz does not match an available same-geometry mode",
		          requested_hz);
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	float current_hz = 0.0f;
	(void)comp_window_macos_get_current_refresh_rate_physical(ct, &current_hz);
	if (fabs((double)current_hz - (double)selected_hz) < 0.05) {
		CGDisplayModeRelease(selected_mode);
		COMP_INFO(ct->c, "PS VR2 physical refresh already %.3f Hz", selected_hz);
		return XRT_SUCCESS;
	}

	macos_drain_present_worker(cwm);
	macos_release_prefetched_drawable(cwm, "refresh_switch_release");
	bool display_link_was_running = macos_display_link_is_running(cwm);
	macos_display_link_stop(cwm);

	CGError cgret = CGDisplaySetDisplayMode(display_id, selected_mode, NULL);
	CGDisplayModeRelease(selected_mode);
	if (cgret != kCGErrorSuccess) {
		COMP_ERROR(ct->c, "Failed to switch PS VR2 physical refresh from %.3f to %.3f Hz (CGError %d)",
		           current_hz, selected_hz, (int)cgret);
		(void)macos_recreate_display_link(cwm, display_id, display_link_was_running);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}

	if (!macos_recreate_display_link(cwm, display_id, display_link_was_running)) {
		COMP_ERROR(ct->c, "PS VR2 switched to %.3f Hz but CVDisplayLink could not be recreated", selected_hz);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	if (cwm->display_period_ns <= 0) {
		cwm->display_period_ns = (int64_t)llround((double)U_TIME_1S_IN_NS / (double)selected_hz);
	}
	ct->c->frame_interval_ns = cwm->display_period_ns;
	if (cwm->base.upc != NULL) {
		u_pc_destroy(&cwm->base.upc);
		u_pc_fake_create(cwm->display_period_ns, os_monotonic_get_ns(), &cwm->base.upc);
	}
	[CATransaction flush];
	macos_schedule_drawable_slot(cwm);
	COMP_INFO(ct->c,
	          "PS VR2 physical refresh switched %.3f -> %.3f Hz; measured compositor period %.3fms (%.3f Hz)",
	          current_hz, selected_hz, (double)cwm->display_period_ns / 1000000.0,
	          (double)U_TIME_1S_IN_NS / (double)cwm->display_period_ns);
	return XRT_SUCCESS;
}

static bool
comp_window_macos_init_with_refresh_rate(struct comp_target *ct)
{
	bool ret = comp_window_macos_init(ct);
	if (!ret) {
		return false;
	}

	macos_log_refresh_mode_candidates(ct);
	int requested_refresh_hz = debug_get_num_option_macos_refresh_rate_hz();
	if (requested_refresh_hz > 0) {
		xrt_result_t refresh_ret =
		    comp_window_macos_request_refresh_rate_physical(ct, (float)requested_refresh_hz);
		if (refresh_ret != XRT_SUCCESS) {
			COMP_WARN(ct->c, "XRT_MACOS_REFRESH_RATE_HZ=%d could not be applied (%d)", requested_refresh_hz,
			          (int)refresh_ret);
		}
	}

	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	CAMetalLayer *layer = cwm->metal_layer;
	if (layer == nil) {
		COMP_WARN(ct->c, "macOS diagnostic: CAMetalLayer missing after init_pre_vulkan");
		return true;
	}

	CFStringRef colorspace_name = [layer colorspace] != NULL ? CGColorSpaceCopyName([layer colorspace]) : NULL;
	COMP_INFO(ct->c,
	          "macOS CAMetalLayer state: framebufferOnly=%s displaySyncEnabled=%s "
	          "presentsWithTransaction=%s maximumDrawableCount=%lu allowsNextDrawableTimeout=%s "
	          "pixelFormat=%lu colorspace=%s",
	          [layer framebufferOnly] ? "true" : "false", [layer displaySyncEnabled] ? "true" : "false",
	          [layer presentsWithTransaction] ? "true" : "false", (unsigned long)[layer maximumDrawableCount],
	          [layer allowsNextDrawableTimeout] ? "true" : "false", (unsigned long)[layer pixelFormat],
	          colorspace_name != NULL ? [(NSString *)colorspace_name UTF8String] : "none");
	if (colorspace_name != NULL) {
		CFRelease(colorspace_name);
	}
	return true;
}

static struct comp_target *
comp_window_macos_create_with_client(struct comp_compositor *c, struct u_macos_hosted_client *client)
{
	struct comp_target *ct = comp_window_macos_create_base(c, client);
	if (ct == NULL) {
		return NULL;
	}
	ct->init_pre_vulkan = comp_window_macos_init_with_refresh_rate;
	ct->get_refresh_rates = comp_window_macos_get_refresh_rates_physical;
	ct->get_current_refresh_rate = comp_window_macos_get_current_refresh_rate_physical;
	ct->request_refresh_rate = comp_window_macos_request_refresh_rate_physical;

	return ct;
}

struct comp_target *
comp_window_macos_create(struct comp_compositor *c)
{
	return comp_window_macos_create_with_client(c, NULL);
}

struct comp_macos_hosted_target_factory
{
	struct comp_target_factory base;
	struct u_macos_hosted_client *client;
};

static bool
create_hosted_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	const struct comp_macos_hosted_target_factory *factory = (const struct comp_macos_hosted_target_factory *)ctf;
	*out_ct = comp_window_macos_create_with_client(c, factory->client);
	return *out_ct != NULL;
}

static bool
create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	(void)ctf;
	struct comp_target *ct = comp_window_macos_create(c);
	if (ct == NULL) {
		return false;
	}
	*out_ct = ct;
	return true;
}

const struct comp_target_factory comp_target_factory_macos = {
    .name = "macOS Metal Window",
    .identifier = "macos",
    .requires_vulkan_for_create = false,
    .is_deferred = false,
    .required_instance_version = 0,
    .required_instance_extensions = NULL,
    .required_instance_extension_count = 0,
    .optional_device_extensions = macos_optional_device_extensions,
    .optional_device_extension_count = ARRAY_SIZE(macos_optional_device_extensions),
    .detect = detect,
    .create_target = create_target,
};

struct comp_target_factory *
comp_window_macos_hosted_factory_create(struct u_macos_hosted_client *client)
{
	struct comp_macos_hosted_target_factory *factory = U_TYPED_CALLOC(struct comp_macos_hosted_target_factory);
	if (factory == NULL) {
		return NULL;
	}
	factory->base = comp_target_factory_macos;
	factory->base.name = "macOS Hosted Metal Layer";
	factory->base.identifier = "macos-hosted";
	factory->base.disable_peek = true;
	factory->base.create_target = create_hosted_target;
	factory->client = client;
	return &factory->base;
}

void
comp_window_macos_hosted_factory_destroy(struct comp_target_factory *factory)
{
	free(factory);
}

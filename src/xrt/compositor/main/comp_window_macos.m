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
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

#include "main/comp_window.h"
#include "util/u_debug.h"
#include "util/u_handles.h"
#include "util/u_misc.h"
#include "util/u_pacing.h"
#include "vk/vk_image_allocator.h"

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
		fflush(state->trace_presented);
		fclose(state->trace_presented);
		state->trace_presented = NULL;
	}
	free(state);
}

DEBUG_GET_ONCE_NUM_OPTION(display_rate_divisor, "XRT_MACOS_DISPLAY_RATE_DIVISOR", 1)
DEBUG_GET_ONCE_BOOL_OPTION(macos_psvr2_timing_trace, "PSVR2_TIMING_TRACE", false)
DEBUG_GET_ONCE_BOOL_OPTION(macos_cvdisplaylink_pacing, "XRT_MACOS_CVDISPLAYLINK_PACING", true)
DEBUG_GET_ONCE_NUM_OPTION(macos_present_min_lead_us, "XRT_MACOS_PRESENT_MIN_LEAD_US", 2000)
DEBUG_GET_ONCE_NUM_OPTION(macos_present_prelatch_us, "XRT_MACOS_PRESENT_PRELATCH_US", 2000)
DEBUG_GET_ONCE_NUM_OPTION(macos_max_drawables, "XRT_MACOS_MAX_DRAWABLES", 3)
DEBUG_GET_ONCE_BOOL_OPTION(macos_async_present, "XRT_MACOS_ASYNC_PRESENT", false)
DEBUG_GET_ONCE_BOOL_OPTION(macos_metal_shared_event_wait, "XRT_MACOS_METAL_SHARED_EVENT_WAIT", true)
DEBUG_GET_ONCE_BOOL_OPTION(macos_present_worker, "XRT_MACOS_PRESENT_WORKER", true)
DEBUG_GET_ONCE_BOOL_OPTION(macos_early_drawable, "XRT_MACOS_EARLY_DRAWABLE", false)
DEBUG_GET_ONCE_BOOL_OPTION(macos_drawable_slot, "XRT_MACOS_DRAWABLE_SLOT", false)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct comp_window_macos
{
	struct comp_target_swapchain base;
	NSScreen *screen;
	NSWindow *window;
	CAMetalLayer *metal_layer;
	id<MTLCommandQueue> present_queue;
	id<MTLTexture> metal_images[MACOS_TARGET_IMAGE_COUNT];
	id<MTLSharedEvent> render_complete_event;
	atomic_bool image_in_flight[MACOS_TARGET_IMAGE_COUNT];
	dispatch_group_t present_command_group;
	dispatch_queue_t present_worker_queue;
	dispatch_group_t present_worker_group;
	pthread_mutex_t present_worker_mutex;
	struct macos_present_job pending_present_job;
	bool pending_present_job_valid;
	bool present_worker_scheduled;
	bool present_worker_shutdown;
	bool async_present;
	bool present_worker_enabled;
	bool early_drawable_enabled;
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
	mach_timebase_info_data_t mach_timebase;
	atomic_uint_fast64_t latest_vblank_ns;
	atomic_uint_fast64_t latest_displaylink_now_host_ns;
	atomic_uint_fast64_t latest_displaylink_output_host_ns;
	atomic_uint_fast64_t latest_displaylink_now_ns;
	atomic_uint_fast64_t latest_displaylink_output_ns;
	atomic_uint_fast64_t latest_displaylink_callback_ns;
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
	uint64_t present_metal_wait_total_ns;
	int64_t display_period_ns;
	uint32_t pixel_width;
	uint32_t pixel_height;
	uint32_t next_image;
	bool logged_layer_state;
	FILE *trace_present;
	FILE *trace_presented;
	FILE *trace_present_complete;
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
	const char *dir = getenv("PSVR2_TIMING_TRACE_DIR");
	if (dir == NULL || dir[0] == '\0') {
		dir = "/tmp";
	}
	char path[1024];
	size_t dir_len = strlen(dir);
	const char *separator = dir_len > 0 && dir[dir_len - 1] == '/' ? "" : "/";
	snprintf(path, sizeof(path), "%s%smonado_psvr2_%d_%s.csv", dir, separator, (int)getpid(), suffix);
	FILE *file = fopen(path, "w");
	if (file == NULL) {
		return NULL;
	}
	setvbuf(file, NULL, _IOFBF, 64 * 1024);
	fputs(header, file);
	fputc('\n', file);
	fflush(file);
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
	    "presented_monotonic_ns,presented_minus_desired_ns,presented_minus_target_ns,observed_present_offset_ns");
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
	cwm->trace_present_worker = macos_timing_trace_open_file(
	    "present_worker",
	    "event,frame_id,event_ns,enqueue_ns,handoff_return_ns,worker_start_ns,next_drawable_begin_ns,"
	    "next_drawable_end_ns,metal_commit_ns,worker_delay_ns,drawable_wait_ns,image_index,timeline_value,"
	    "queue_depth,shared_event_wait,newer_pending,pending_frame_id,pending_timeline_value");
	cwm->trace_drawable_prefetch = macos_timing_trace_open_file(
	    "drawable_prefetch", "event,timeline_value,event_ns,next_drawable_begin_ns,next_drawable_end_ns,drawable_wait_ns");
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
		fflush(cwm->trace_present);
		fclose(cwm->trace_present);
		cwm->trace_present = NULL;
	}
	if (cwm->trace_presented != NULL) {
		fflush(cwm->trace_presented);
		fclose(cwm->trace_presented);
		cwm->trace_presented = NULL;
	}
	if (cwm->trace_present_complete != NULL) {
		fflush(cwm->trace_present_complete);
		fclose(cwm->trace_present_complete);
		cwm->trace_present_complete = NULL;
	}
	if (cwm->trace_present_worker != NULL) {
		fflush(cwm->trace_present_worker);
		fclose(cwm->trace_present_worker);
		cwm->trace_present_worker = NULL;
	}
	if (cwm->trace_drawable_prefetch != NULL) {
		fflush(cwm->trace_drawable_prefetch);
		fclose(cwm->trace_drawable_prefetch);
		cwm->trace_drawable_prefetch = NULL;
	}
	if (cwm->trace_vblank != NULL) {
		fflush(cwm->trace_vblank);
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

	int64_t offset_ns = refresh_host_to_monotonic_offset_ns(cwm);
	uint64_t callback_ns = (uint64_t)os_monotonic_get_ns();
	uint64_t now_host_ns = 0;
	uint64_t output_host_ns = 0;
	uint64_t now_ns = callback_ns;
	uint64_t output_ns = 0;

	if ((in_now->flags & kCVTimeStampHostTimeValid) != 0) {
		now_host_ns = host_time_to_ns(cwm, in_now->hostTime);
		now_ns = host_ns_to_monotonic_ns(now_host_ns, offset_ns);
		atomic_store_explicit(&cwm->latest_displaylink_now_host_ns, now_host_ns, memory_order_release);
		atomic_store_explicit(&cwm->latest_displaylink_now_ns, now_ns, memory_order_release);
	}
	if ((in_output_time->flags & kCVTimeStampHostTimeValid) != 0) {
		output_host_ns = host_time_to_ns(cwm, in_output_time->hostTime);
		output_ns = host_ns_to_monotonic_ns(output_host_ns, offset_ns);
		atomic_store_explicit(&cwm->latest_displaylink_output_host_ns, output_host_ns, memory_order_release);
		atomic_store_explicit(&cwm->latest_displaylink_output_ns, output_ns, memory_order_release);
	}

	if (output_ns != 0) {
		uint64_t last_vblank_ns = derive_last_vblank_ns(cwm, output_ns, now_ns);
		atomic_store_explicit(&cwm->latest_vblank_ns, last_vblank_ns, memory_order_release);
	}
	atomic_store_explicit(&cwm->latest_displaylink_callback_ns, callback_ns, memory_order_release);
	return kCVReturnSuccess;
}

static inline struct vk_bundle *
get_vk(struct comp_window_macos *cwm)
{
	return &cwm->base.base.c->base.vk;
}

static void
macos_release_source_image(struct comp_window_macos *cwm, uint32_t index)
{
	if (index < MACOS_TARGET_IMAGE_COUNT) {
		atomic_store_explicit(&cwm->image_in_flight[index], false, memory_order_release);
	}
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
		fflush(cwm->trace_drawable_prefetch);
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
macos_prefetch_drawable_for_rendering_frame(struct comp_window_macos *cwm)
{
	if (!cwm->early_drawable_enabled || !cwm->async_present || cwm->present_worker_enabled ||
	    cwm->render_complete_event == nil || cwm->metal_layer == nil) {
		return;
	}

	int64_t rendering_frame_id = cwm->base.base.c->frame.rendering.id;
	if (rendering_frame_id < 0) {
		return;
	}
	uint64_t timeline_value = (uint64_t)rendering_frame_id;
	if (cwm->prefetched_drawable != nil && cwm->prefetched_drawable_timeline_value == timeline_value) {
		return;
	}
	if (cwm->prefetched_drawable != nil) {
		macos_release_prefetched_drawable(cwm, "stale_release");
	}

	@autoreleasepool {
		uint64_t begin_ns = os_monotonic_get_ns();
		id<CAMetalDrawable> drawable = [cwm->metal_layer nextDrawable];
		uint64_t end_ns = os_monotonic_get_ns();
		if (drawable == nil) {
			macos_trace_drawable_prefetch(cwm, "acquire_nil", timeline_value, end_ns, begin_ns, end_ns);
			return;
		}
		cwm->prefetched_drawable = [drawable retain];
		cwm->prefetched_drawable_timeline_value = timeline_value;
		cwm->prefetched_drawable_begin_ns = begin_ns;
		cwm->prefetched_drawable_end_ns = end_ns;
		macos_trace_drawable_prefetch(cwm, "acquired", timeline_value, end_ns, begin_ns, end_ns);
	}
}

static void
macos_schedule_drawable_slot(struct comp_window_macos *cwm)
{
	if (!cwm->drawable_slot_enabled || cwm->present_worker_queue == NULL || cwm->present_worker_group == NULL ||
	    cwm->render_complete_event == nil || cwm->metal_layer == nil) {
		return;
	}

	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (cwm->present_worker_shutdown || cwm->present_worker_scheduled || cwm->prefetched_drawable != nil) {
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		return;
	}
	cwm->present_worker_scheduled = true;
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

			pthread_mutex_lock(&cwm->present_worker_mutex);
			cwm->present_worker_scheduled = false;
			if (!cwm->present_worker_shutdown && retained_drawable != nil && cwm->prefetched_drawable == nil) {
				cwm->prefetched_drawable = retained_drawable;
				cwm->prefetched_drawable_timeline_value = 0;
				cwm->prefetched_drawable_begin_ns = begin_ns;
				cwm->prefetched_drawable_end_ns = end_ns;
				retained_drawable = nil;
				stored = true;
			}
			pthread_mutex_unlock(&cwm->present_worker_mutex);

			if (retained_drawable != nil) {
				[retained_drawable release];
			}
			macos_trace_drawable_prefetch(cwm, stored ? "slot_ready" : (drawable == nil ? "slot_acquire_nil" : "slot_discard"),
			                               0, end_ns, begin_ns, end_ns);
			dispatch_group_leave(cwm->present_worker_group);
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
	        (unsigned long long)job->frame_id, (unsigned long long)event_ns,
	        (unsigned long long)job->enqueue_ns, (unsigned long long)handoff_return_ns,
	        (unsigned long long)worker_start_ns, (unsigned long long)next_drawable_begin_ns,
	        (unsigned long long)next_drawable_end_ns, (unsigned long long)metal_commit_ns,
	        (unsigned long long)worker_delay_ns, (unsigned long long)drawable_wait_ns, job->image_index,
	        (unsigned long long)job->timeline_value, queue_depth, shared_event_wait ? 1u : 0u,
	        newer_pending ? 1u : 0u, (unsigned long long)pending_frame_id,
	        (unsigned long long)pending_timeline_value);
	cwm->trace_present_worker_rows++;
	if (cwm->trace_present_worker_rows % 256 == 0) {
		fflush(cwm->trace_present_worker);
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

static CGDirectDisplayID
get_display_id(NSScreen *screen)
{
	NSNumber *number = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
	return number != nil ? (CGDirectDisplayID)[number unsignedIntValue] : kCGNullDirectDisplay;
}

static NSScreen *
find_psvr2_screen(struct comp_compositor *c)
{
	NSScreen *width_fallback = nil;
	for (NSScreen *screen in [NSScreen screens]) {
		CGDirectDisplayID display_id = get_display_id(screen);
		size_t width = display_id != kCGNullDirectDisplay ? CGDisplayPixelsWide(display_id) : 0;
		size_t height = display_id != kCGNullDirectDisplay ? CGDisplayPixelsHigh(display_id) : 0;
		NSString *name = [screen localizedName];
		if (c != NULL) {
			COMP_INFO(c, "macOS display: '%s' %zux%zu", [name UTF8String], width, height);
		}
		if ([name caseInsensitiveCompare:@"PS VR2"] == NSOrderedSame) {
			return screen;
		}
		if (width_fallback == nil && width == 4000) {
			width_fallback = screen;
		}
	}
	return width_fallback;
}

static bool
comp_window_macos_init(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	@autoreleasepool {
		NSScreen *screen = find_psvr2_screen(ct->c);
		if (screen == nil) {
			COMP_ERROR(ct->c, "Could not find a display named 'PS VR2' or a 4000-pixel-wide fallback");
			return false;
		}

		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		[NSApp finishLaunching];

		CGDirectDisplayID display_id = get_display_id(screen);
		if (display_id == kCGNullDirectDisplay) {
			COMP_ERROR(ct->c, "Could not get the CoreGraphics display ID for '%s'", [[screen localizedName] UTF8String]);
			return false;
		}
		size_t pixel_width = CGDisplayPixelsWide(display_id);
		size_t pixel_height = CGDisplayPixelsHigh(display_id);
		if (pixel_width == 0 || pixel_height == 0) {
			COMP_ERROR(ct->c, "Selected macOS display has an invalid pixel size");
			return false;
		}

		NSWindow *window = [[NSWindow alloc] initWithContentRect:[screen frame]
		                                                styleMask:NSWindowStyleMaskBorderless
		                                                  backing:NSBackingStoreBuffered
		                                                    defer:NO
		                                                   screen:screen];
		if (window == nil) {
			COMP_ERROR(ct->c, "Failed to create the macOS PS VR2 window");
			return false;
		}

		id<MTLDevice> metal_device = MTLCreateSystemDefaultDevice();
		if (metal_device == nil) {
			[window release];
			COMP_ERROR(ct->c, "Failed to create the default Metal device");
			return false;
		}
		MTKView *metal_view = [[MTKView alloc] initWithFrame:[screen frame] device:metal_device];
		[metal_device release];
		if (metal_view == nil) {
			[window release];
			COMP_ERROR(ct->c, "Failed to create the PS VR2 MTKView");
			return false;
		}
		[metal_view setPaused:YES];
		[metal_view setEnableSetNeedsDisplay:NO];
		[metal_view setColorPixelFormat:MTLPixelFormatBGRA8Unorm];
		[metal_view setFramebufferOnly:NO];
		[window setContentView:metal_view];

		CAMetalLayer *metal_layer = [(CAMetalLayer *)[metal_view layer] retain];
		[metal_layer setContentsScale:[screen backingScaleFactor]];
		[metal_layer setDrawableSize:CGSizeMake(pixel_width, pixel_height)];
		[metal_layer setOpaque:YES];
		[metal_layer setDisplaySyncEnabled:YES];
		[metal_layer setAllowsNextDrawableTimeout:YES];
		int max_drawables = debug_get_num_option_macos_max_drawables();
		if (max_drawables != 2 && max_drawables != 3) {
			COMP_WARN(ct->c, "XRT_MACOS_MAX_DRAWABLES must be 2 or 3; using default 3 instead of %d", max_drawables);
			max_drawables = 3;
		}
		[metal_layer setMaximumDrawableCount:(NSUInteger)max_drawables];
		COMP_INFO(ct->c, "macOS CAMetalLayer maximumDrawableCount=%lu",
		          (unsigned long)[metal_layer maximumDrawableCount]);
		id<MTLCommandQueue> present_queue = [[metal_layer device] newCommandQueue];
		[metal_view release];
		if (present_queue == nil) {
			[metal_layer release];
			[window release];
			COMP_ERROR(ct->c, "Failed to create the macOS Metal presentation queue");
			return false;
		}

		[window setBackgroundColor:[NSColor blackColor]];
		[window setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
		                              NSWindowCollectionBehaviorFullScreenAuxiliary |
		                              NSWindowCollectionBehaviorStationary];
		[window setHasShadow:NO];
		[window setHidesOnDeactivate:NO];
		[window setIgnoresMouseEvents:YES];
		[window setLevel:NSMainMenuWindowLevel + 1];
		[window setFrame:[screen frame] display:YES];
		[window orderFrontRegardless];
		[NSApp activateIgnoringOtherApps:YES];
		[CATransaction flush];

		cwm->screen = [screen retain];
		cwm->window = window;
		cwm->metal_layer = metal_layer;
		cwm->present_queue = present_queue;
		cwm->pixel_width = (uint32_t)pixel_width;
		cwm->pixel_height = (uint32_t)pixel_height;

		mach_timebase_info(&cwm->mach_timebase);
		refresh_host_to_monotonic_offset_ns(cwm);
		CVReturn cvret = CVDisplayLinkCreateWithCGDisplay(display_id, &cwm->display_link);
		if (cvret == kCVReturnSuccess) {
			cvret = CVDisplayLinkSetOutputCallback(cwm->display_link, display_link_callback, cwm);
		}
		if (cvret != kCVReturnSuccess) {
			if (cwm->display_link != NULL) {
				CVDisplayLinkRelease(cwm->display_link);
				cwm->display_link = NULL;
			}
			COMP_WARN(ct->c, "Could not create the PS VR2 display link (%d); using estimated pacing", cvret);
		} else {
			CVTime period = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(cwm->display_link);
			if ((period.flags & kCVTimeIsIndefinite) == 0 && period.timeValue > 0 && period.timeScale > 0) {
				cwm->display_period_ns =
				    (int64_t)(((__int128)period.timeValue * U_TIME_1S_IN_NS) / period.timeScale);
				int divisor = debug_get_num_option_display_rate_divisor();
				if (divisor < 1) {
					divisor = 1;
				}
				ct->c->frame_interval_ns = cwm->display_period_ns * divisor;
				COMP_INFO(ct->c, "PS VR2 display period %.3fms; compositor rate divisor %d (%.2f Hz)",
				          (double)cwm->display_period_ns / 1000000.0, divisor,
				          (double)U_TIME_1S_IN_NS / (double)ct->c->frame_interval_ns);
			}
		}

		VkExtent2D extent = {.width = cwm->pixel_width, .height = cwm->pixel_height};
		comp_target_swapchain_override_extents(&cwm->base, extent);
		COMP_INFO(ct->c, "Selected macOS display '%s' at %ux%u", [[screen localizedName] UTF8String], extent.width,
		          extent.height);
	}
	return true;
}

static bool
comp_window_macos_init_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	(void)preferred_width;
	(void)preferred_height;
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	struct vk_bundle *vk = get_vk(cwm);

	if (!vk->features.timeline_semaphore || vk->vkWaitSemaphores == NULL) {
		COMP_WARN(ct->c, "Timeline semaphores unavailable; macOS presentation will fall back to queue-idle waits");
		return true;
	}

	bool want_shared_event = cwm->async_present && debug_get_bool_option_macos_metal_shared_event_wait() &&
	                         vk->has_EXT_metal_objects && vk->vkExportMetalObjectsEXT != NULL;

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
		COMP_WARN(ct->c, "Could not create macOS render-complete timeline semaphore: %s; using queue-idle fallback",
		          vk_result_string(ret));
		ct->semaphores.render_complete = VK_NULL_HANDLE;
		ct->semaphores.render_complete_is_timeline = false;
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
			COMP_WARN(ct->c, "VK_EXT_metal_objects did not export an MTLSharedEvent; retaining CPU Vulkan wait fallback");
		}
	} else if (cwm->async_present && debug_get_bool_option_macos_metal_shared_event_wait()) {
		COMP_WARN(ct->c, "VK_EXT_metal_objects unavailable; asynchronous present will retain the CPU Vulkan wait");
	}

	COMP_INFO(ct->c, "macOS target using render-complete timeline semaphore%s",
	          cwm->render_complete_event != nil ? " with Metal shared-event handoff" : "");
	if (cwm->early_drawable_enabled && cwm->render_complete_event == nil) {
		COMP_WARN(ct->c, "XRT_MACOS_EARLY_DRAWABLE requested but MTLSharedEvent handoff is unavailable; early drawable prefetch is disabled");
		cwm->early_drawable_enabled = false;
	}
	if (cwm->drawable_slot_enabled && cwm->render_complete_event == nil) {
		COMP_WARN(ct->c, "XRT_MACOS_DRAWABLE_SLOT requested but MTLSharedEvent handoff is unavailable; drawable slot is disabled");
		cwm->drawable_slot_enabled = false;
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
			if (ct->images[i].storage_view != VK_NULL_HANDLE && ct->images[i].storage_view != ct->images[i].view) {
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
	ct->final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	ct->surface_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	cwm->next_image = 0;
	if (cwm->base.upc == NULL) {
		u_pc_fake_create(ct->c->frame_interval_ns, os_monotonic_get_ns(), &cwm->base.upc);
	}
	if (cwm->display_link != NULL && !CVDisplayLinkIsRunning(cwm->display_link)) {
		CVReturn cvret = CVDisplayLinkStart(cwm->display_link);
		if (cvret != kCVReturnSuccess) {
			COMP_WARN(ct->c, "Could not start the PS VR2 display link (%d); using estimated pacing", cvret);
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

	if (!cwm->async_present) {
		*out_index = cwm->next_image;
		cwm->next_image = (cwm->next_image + 1) % ct->image_count;
		cwm->last_image_acquire_wait_ns = 0;
		return VK_SUCCESS;
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
macos_execute_present_job(struct comp_window_macos *cwm, const struct macos_present_job *job, bool async_present)
{
	struct comp_target *ct = &cwm->base.base;
	struct vk_bundle *vk = get_vk(cwm);
	struct vk_bundle_queue *present_queue = job->present_queue;
	uint64_t frame_id = job->frame_id;
	uint32_t index = job->image_index;
	uint64_t timeline_semaphore_value = job->timeline_value;
	int64_t desired_present_time_ns = job->desired_present_time_ns;
	int64_t present_slop_ns = job->present_slop_ns;
	uint64_t worker_start_ns = async_present ? os_monotonic_get_ns() : 0;
	uint64_t next_drawable_begin_ns = 0;
	uint64_t after_drawable_ns = 0;
	uint64_t before_present_call_ns = 0;
	uint64_t after_present_call_ns = 0;
	uint64_t after_commit_ns = 0;
	uint64_t after_metal_wait_ns = 0;
	uint64_t target_output_ns = 0;
	uint64_t metal_request_ns = 0;
	const char *wait_mode = "queue_idle";
	bool shared_event_wait = async_present && debug_get_bool_option_macos_metal_shared_event_wait() &&
	                         cwm->render_complete_event != nil;
	uint64_t image_reuse_wait_ns = job->image_reuse_wait_ns;
	double scheduled_present_host_s = 0.0;
	double gpu_start_time_s = 0.0;
	double gpu_end_time_s = 0.0;
	if (async_present) {
		macos_trace_present_worker(cwm, "worker_start", job, worker_start_ns, 0, worker_start_ns, 0, 0, 0, 0,
		                           shared_event_wait);
	}
	assert(present_queue != NULL);
	if (index >= ct->image_count || cwm->metal_images[index] == nil) {
		if (async_present) {
			macos_retire_unpresented_job(cwm, job, "invalid", 0);
		}
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	uint64_t host_call_ns = job->enqueue_ns;
	uint64_t before_vk_wait_ns = os_monotonic_get_ns();
	VkResult ret = VK_SUCCESS;
	if (shared_event_wait) {
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
		if (async_present) {
			macos_release_source_image(cwm, index);
		}
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
				uint64_t drop_ns = os_monotonic_get_ns();
				macos_trace_drawable_prefetch(cwm, "slot_drop", timeline_semaphore_value, drop_ns, 0, 0);
				macos_schedule_drawable_slot(cwm);
				macos_retire_unpresented_job(cwm, job, "drawable_slot_drop", 0);
				return VK_SUCCESS;
			}

			drawable = [retained_drawable autorelease];
			macos_trace_drawable_prefetch(cwm, "slot_consumed", timeline_semaphore_value, os_monotonic_get_ns(),
			                               next_drawable_begin_ns, after_drawable_ns);
			macos_schedule_drawable_slot(cwm);
		} else if (cwm->early_drawable_enabled && cwm->prefetched_drawable != nil &&
		           cwm->prefetched_drawable_timeline_value == timeline_semaphore_value) {
			next_drawable_begin_ns = cwm->prefetched_drawable_begin_ns;
			after_drawable_ns = cwm->prefetched_drawable_end_ns;
			id<CAMetalDrawable> retained_drawable = cwm->prefetched_drawable;
			cwm->prefetched_drawable = nil;
			cwm->prefetched_drawable_timeline_value = 0;
			cwm->prefetched_drawable_begin_ns = 0;
			cwm->prefetched_drawable_end_ns = 0;
			drawable = [retained_drawable autorelease];
			macos_trace_drawable_prefetch(cwm, "consumed", timeline_semaphore_value, os_monotonic_get_ns(),
			                               next_drawable_begin_ns, after_drawable_ns);
		} else {
			if (cwm->prefetched_drawable != nil) {
				macos_release_prefetched_drawable(cwm, "present_mismatch_release");
			}
			uint64_t drawable_trace_begin_ns = os_monotonic_get_ns();
			if (async_present) {
				/*
				 * Do not include trace-writing latency in the nextDrawable measurement.
				 * The actual call start is captured only after this row has been written;
				 * drawable_end records both the real call begin and end timestamps.
				 */
				macos_trace_present_worker(cwm, "drawable_trace_begin", job, drawable_trace_begin_ns, 0,
				                           worker_start_ns, 0, 0, 0, 0, shared_event_wait);
			}
			next_drawable_begin_ns = os_monotonic_get_ns();
			drawable = [cwm->metal_layer nextDrawable];
			after_drawable_ns = os_monotonic_get_ns();
			if (async_present) {
				macos_trace_present_worker(cwm, "drawable_end", job, after_drawable_ns, 0, worker_start_ns,
				                           next_drawable_begin_ns, after_drawable_ns, 0, 0, shared_event_wait);
			}
		}
		if (drawable == nil) {
			COMP_ERROR(ct->c, "Could not acquire a CAMetalDrawable");
			if (async_present) {
				macos_retire_unpresented_job(cwm, job, "drawable_error", 0);
			}
			return VK_ERROR_OUT_OF_DATE_KHR;
		}
		id<MTLCommandBuffer> command_buffer = [cwm->present_queue commandBuffer];
		if (command_buffer == nil) {
			COMP_ERROR(ct->c, "Could not create a Metal presentation command buffer");
			if (async_present) {
				macos_retire_unpresented_job(cwm, job, "command_buffer_error", 0);
			}
			return VK_ERROR_DEVICE_LOST;
		}
		if (shared_event_wait) {
			[command_buffer encodeWaitForEvent:cwm->render_complete_event value:timeline_semaphore_value];
		}
		id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
		if (blit == nil) {
			COMP_ERROR(ct->c, "Could not create Metal blit encoder");
			if (async_present) {
				macos_retire_unpresented_job(cwm, job, "blit_error", 0);
			}
			return VK_ERROR_DEVICE_LOST;
		}
		MTLSize size = MTLSizeMake(ct->width, ct->height, 1);
		[blit copyFromTexture:cwm->metal_images[index]
		             sourceSlice:0
		             sourceLevel:0
		            sourceOrigin:MTLOriginMake(0, 0, 0)
		              sourceSize:size
		               toTexture:[drawable texture]
		        destinationSlice:0
		        destinationLevel:0
		       destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];

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
			[drawable addPresentedHandler:^(id<MTLDrawable> presented_drawable) {
				double presented_time_s = [presented_drawable presentedTime];
				int64_t handler_ns = os_monotonic_get_ns();
				double host_frequency = CVGetHostClockFrequency();
				uint64_t current_host_ticks = CVGetCurrentHostTime();
				int64_t current_host_ns =
				    host_frequency > 0.0 ? (int64_t)llround((double)current_host_ticks * 1e9 / host_frequency) : 0;
				int64_t handler_offset_ns = handler_ns - current_host_ns;
				int64_t presented_host_ns =
				    presented_time_s > 0.0 ? (int64_t)llround(presented_time_s * (double)U_TIME_1S_IN_NS) : 0;
				int64_t presented_monotonic_ns =
				    presented_host_ns != 0 ? presented_host_ns + handler_offset_ns : 0;
				int64_t presented_minus_desired_ns =
				    presented_monotonic_ns != 0 ? presented_monotonic_ns - traced_desired_present_ns : 0;
				int64_t presented_minus_target_ns =
				    presented_monotonic_ns != 0 ? presented_monotonic_ns - (int64_t)traced_target_output_ns : 0;
				int64_t observed_present_offset_ns =
				    presented_monotonic_ns != 0 ? presented_monotonic_ns - traced_desired_present_ns : 0;
				if (observed_present_offset_ns > 0) {
					atomic_store_explicit(&presented_state->latest_observed_present_offset_ns,
					                      observed_present_offset_ns, memory_order_release);
					atomic_fetch_add_explicit(&presented_state->present_offset_sample_serial, 1,
					                          memory_order_release);
				}
				flockfile(trace_file);
				fprintf(trace_file,
				        "%llu,%" PRIi64 ",%" PRIi64 ",%llu,%.17g,%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 "\n",
				        (unsigned long long)traced_frame_id, handler_ns, traced_desired_present_ns,
				        (unsigned long long)traced_target_output_ns, presented_time_s, presented_monotonic_ns,
				        presented_minus_desired_ns, presented_minus_target_ns, observed_present_offset_ns);
				fflush(trace_file);
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
		if (scheduled_present_host_s > 0.0) {
			[command_buffer presentDrawable:drawable atTime:scheduled_present_host_s];
		} else {
			[command_buffer presentDrawable:drawable];
		}
		after_present_call_ns = os_monotonic_get_ns();
		if (async_present) {
			uint64_t traced_frame_id = frame_id;
			uint32_t traced_index = index;
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
					        (unsigned long long)traced_frame_id, (unsigned long long)completion_ns,
					        traced_index, (unsigned long long)traced_timeline_value, (unsigned long)status,
					        (unsigned long long)(completion_ns - commit_begin_ns), completed_gpu_start_s,
					        completed_gpu_end_s, traced_shared_event_wait ? 1u : 0u);
					funlockfile(complete_trace);
				}
				macos_release_source_image(cwm, traced_index);
				dispatch_group_leave(command_group);
			}];
			[command_buffer commit];
			after_commit_ns = os_monotonic_get_ns();
			after_metal_wait_ns = after_commit_ns;
			macos_trace_present_worker(cwm, "submitted", job, after_commit_ns, 0, worker_start_ns,
			                           next_drawable_begin_ns, after_drawable_ns, after_commit_ns, 0,
			                           shared_event_wait);
			pthread_mutex_lock(&cwm->present_worker_mutex);
			cwm->worker_jobs_submitted++;
			uint64_t worker_delay_ns = worker_start_ns - job->enqueue_ns;
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
				COMP_INFO(ct->c,
				          "macOS present worker: submitted %llu, superseded %llu, drawable >half-refresh %llu, >5ms "
				          "%llu, queue delay avg %.3fms max %.3fms",
				          (unsigned long long)submitted, (unsigned long long)superseded,
				          (unsigned long long)half_refresh_stalls, (unsigned long long)stalls,
				          (double)average_delay_ns / 1000000.0,
				          (double)max_delay_ns / 1000000.0);
			}
		} else {
			[command_buffer commit];
			after_commit_ns = os_monotonic_get_ns();
			[command_buffer waitUntilCompleted];
			after_metal_wait_ns = os_monotonic_get_ns();
			gpu_start_time_s = [command_buffer GPUStartTime];
			gpu_end_time_s = [command_buffer GPUEndTime];
			if ([command_buffer status] == MTLCommandBufferStatusError) {
				COMP_ERROR(ct->c, "Metal presentation failed: %s",
				           [[[command_buffer error] localizedDescription] UTF8String]);
				return VK_ERROR_DEVICE_LOST;
			}
		}
		cwm->present_vk_wait_total_ns += after_vk_wait_ns - before_vk_wait_ns;
		cwm->present_drawable_wait_total_ns +=
		    after_drawable_ns > next_drawable_begin_ns ? after_drawable_ns - next_drawable_begin_ns : 0;
		if (!async_present) {
			cwm->present_metal_wait_total_ns += after_metal_wait_ns - after_drawable_ns;
		}
	}

	if (cwm->trace_present != NULL) {
		uint64_t latest_output_ns = atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);
		fprintf(cwm->trace_present,
		        "%llu,%llu,%" PRIi64 ",%" PRIi64 ",%llu,%" PRIi64 ",%llu,%" PRIi64 ",%" PRIi64 ",%.17g,%" PRIi64 ",%u,%llu,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.17g,%.17g,%u,%u,%llu\n",
		        (unsigned long long)frame_id, (unsigned long long)host_call_ns, desired_present_time_ns,
		        desired_present_time_ns - (int64_t)host_call_ns, (unsigned long long)target_output_ns,
		        (int64_t)target_output_ns - desired_present_time_ns, (unsigned long long)metal_request_ns,
		        (int64_t)metal_request_ns - (int64_t)target_output_ns,
		        (int64_t)metal_request_ns - (int64_t)before_present_call_ns, scheduled_present_host_s, present_slop_ns, index,
		        (unsigned long long)timeline_semaphore_value, wait_mode, (unsigned long long)after_vk_wait_ns,
		        (unsigned long long)after_drawable_ns, (unsigned long long)before_present_call_ns,
		        (unsigned long long)after_present_call_ns, (unsigned long long)after_commit_ns,
		        (unsigned long long)after_metal_wait_ns, (unsigned long long)latest_output_ns, gpu_start_time_s, gpu_end_time_s,
		        async_present ? 1u : 0u, shared_event_wait ? 1u : 0u, (unsigned long long)image_reuse_wait_ns);
		cwm->trace_present_rows++;
		if (cwm->trace_present_rows % 256 == 0) {
			fflush(cwm->trace_present);
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
		const char *cadence_label = cwm->present_worker_enabled
		                                ? "macOS present-worker completion cadence"
		                                : (async_present ? "macOS async present completion cadence"
		                                                 : "macOS present-call return cadence");
		COMP_INFO(ct->c, "%s: average %.3fms, min %.3fms, max %.3fms, late %llu/240",
		          cadence_label,
		          average_ms, (double)cwm->present_min_ns / 1000000.0, (double)cwm->present_max_ns / 1000000.0,
		          (unsigned long long)cwm->present_missed_intervals);
		COMP_INFO(ct->c, "macOS presentation CPU waits: Vulkan %.3fms, drawable %.3fms, synchronous Metal %.3fms",
		          (double)cwm->present_vk_wait_total_ns / 240.0 / 1000000.0,
		          (double)cwm->present_drawable_wait_total_ns / 240.0 / 1000000.0,
		          (double)cwm->present_metal_wait_total_ns / 240.0 / 1000000.0);
		cwm->present_sample_count = 0;
		cwm->present_total_ns = 0;
		cwm->present_min_ns = 0;
		cwm->present_max_ns = 0;
		cwm->present_missed_intervals = 0;
		cwm->present_vk_wait_total_ns = 0;
		cwm->present_drawable_wait_total_ns = 0;
		cwm->present_metal_wait_total_ns = 0;
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
	bool shared_event_wait = debug_get_bool_option_macos_metal_shared_event_wait() &&
	                         cwm->render_complete_event != nil;
	uint64_t event_ns = os_monotonic_get_ns();
	macos_trace_present_worker(cwm, trace_event, &retired_job, event_ns, 0, 0, 0, 0, 0, queue_depth,
	                           shared_event_wait);

	/*
	 * Dropping presentation does not mean Vulkan has stopped writing the source.
	 * Retire it behind the captured render-complete value before making it
	 * acquirable again. These tasks are bounded by the three in-flight images.
	 */
	dispatch_group_enter(cwm->present_command_group);
	dispatch_queue_t retirement_queue = cwm->present_worker_enabled ? cwm->present_worker_queue : NULL;
	if (retirement_queue == NULL) {
		retirement_queue = dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0);
	}
	dispatch_async(retirement_queue, ^{
		@autoreleasepool {
			bool released_by_metal = false;
			if (shared_event_wait) {
				id<MTLCommandBuffer> command_buffer = [cwm->present_queue commandBuffer];
				if (command_buffer != nil) {
					[command_buffer encodeWaitForEvent:cwm->render_complete_event value:retired_job.timeline_value];
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
	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (cwm->present_worker_shutdown || !cwm->pending_present_job_valid) {
		cwm->present_worker_scheduled = false;
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		dispatch_group_leave(cwm->present_worker_group);
		return;
	}
	job = cwm->pending_present_job;
	cwm->pending_present_job_valid = false;
	pthread_mutex_unlock(&cwm->present_worker_mutex);

	(void)macos_execute_present_job(cwm, &job, true);

	/* Schedule one job at a time so already-queued dropped-image retirement work cannot starve. */
	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (!cwm->present_worker_shutdown && cwm->pending_present_job_valid) {
		dispatch_async(cwm->present_worker_queue, ^{ macos_present_worker_run_one(cwm); });
	} else {
		cwm->present_worker_scheduled = false;
		dispatch_group_leave(cwm->present_worker_group);
	}
	pthread_mutex_unlock(&cwm->present_worker_mutex);
}

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
	};
	if (!cwm->async_present) {
		return macos_execute_present_job(cwm, &job, false);
	}
	if (!cwm->present_worker_enabled) {
		/* Diagnostic A/B: keep async Metal/shared-event handoff but acquire drawable on caller. */
		return macos_execute_present_job(cwm, &job, true);
	}
	if (index >= ct->image_count || cwm->metal_images[index] == nil || present_queue == NULL) {
		macos_retire_unpresented_job(cwm, &job, "invalid", 0);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	struct macos_present_job superseded_job;
	bool superseded = false;
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
	if (!cwm->present_worker_scheduled) {
		cwm->present_worker_scheduled = true;
		dispatch_group_enter(cwm->present_worker_group);
		dispatch_async(cwm->present_worker_queue, ^{ macos_present_worker_run_one(cwm); });
	}
	pthread_mutex_unlock(&cwm->present_worker_mutex);

	if (superseded) {
		macos_retire_unpresented_job(cwm, &superseded_job, "superseded", 1);
	}
	uint64_t handoff_return_ns = os_monotonic_get_ns();
	macos_trace_present_worker(cwm, "enqueued", &job, handoff_return_ns, handoff_return_ns, 0, 0, 0, 0, 1,
	                           debug_get_bool_option_macos_metal_shared_event_wait() &&
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
	macos_prefetch_drawable_for_rendering_frame(cwm);
	macos_schedule_drawable_slot(cwm);
	uint64_t vblank_ns = atomic_exchange_explicit(&cwm->latest_vblank_ns, 0, memory_order_acquire);
	uint64_t displaylink_now_host_ns =
	    atomic_load_explicit(&cwm->latest_displaylink_now_host_ns, memory_order_acquire);
	uint64_t displaylink_output_host_ns =
	    atomic_load_explicit(&cwm->latest_displaylink_output_host_ns, memory_order_acquire);
	uint64_t displaylink_now_ns = atomic_load_explicit(&cwm->latest_displaylink_now_ns, memory_order_acquire);
	uint64_t displaylink_output_ns = atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);
	uint64_t displaylink_callback_ns = atomic_load_explicit(&cwm->latest_displaylink_callback_ns, memory_order_acquire);
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
				// 1/8 EMA: stable enough to reject callback/clock-conversion noise while adapting quickly.
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
		        "%llu,%llu,%llu,%llu,%" PRIi64 ",%llu,%llu,%llu,%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%llu,%" PRIi64 "\n",
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
			fflush(cwm->trace_vblank);
		}
	}

	if (debug_get_bool_option_macos_cvdisplaylink_pacing()) {
		u_pc_update_vblank_from_display_control(cwm->base.upc, (int64_t)vblank_ns);
	}
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
			double average_ms = (double)cwm->cadence_total_ns / (double)cwm->cadence_sample_count / 1000000.0;
			COMP_INFO(ct->c, "PS VR2 display-link cadence: average %.3fms, min %.3fms, max %.3fms", average_ms,
			          (double)cwm->cadence_min_ns / 1000000.0, (double)cwm->cadence_max_ns / 1000000.0);
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
comp_window_macos_queue_supports_present(struct comp_target *ct,
	                                     struct vk_bundle_queue *queue,
	                                     VkBool32 *out_supported)
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
			COMP_INFO(ct->c, "macOS presentation: window visible=%s layer device=%s format=%lu drawable=%.0fx%.0f",
			          [cwm->window isVisible] ? "true" : "false", [cwm->metal_layer device] != nil ? "set" : "nil",
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
		[cwm->window setTitle:[NSString stringWithUTF8String:title]];
	}
}

static void
comp_window_macos_destroy(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	if (cwm->display_link != NULL) {
		CVDisplayLinkStop(cwm->display_link);
		CVDisplayLinkRelease(cwm->display_link);
		cwm->display_link = NULL;
	}
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
		[cwm->window orderOut:nil];
		[cwm->window close];
		[cwm->window release];
		[cwm->present_queue release];
		[cwm->metal_layer release];
		[cwm->screen release];
	}
	pthread_mutex_destroy(&cwm->present_worker_mutex);
	free(cwm);
}

struct comp_target *
comp_window_macos_create(struct comp_compositor *c)
{
	struct comp_window_macos *cwm = U_TYPED_CALLOC(struct comp_window_macos);
	if (cwm == NULL) {
		return NULL;
	}
	if (pthread_mutex_init(&cwm->present_worker_mutex, NULL) != 0) {
		free(cwm);
		return NULL;
	}
	cwm->async_present = debug_get_bool_option_macos_async_present();
	cwm->present_worker_enabled = cwm->async_present && debug_get_bool_option_macos_present_worker();
	bool want_drawable_slot = debug_get_bool_option_macos_drawable_slot();
	cwm->drawable_slot_enabled = cwm->async_present && !cwm->present_worker_enabled && want_drawable_slot;
	cwm->early_drawable_enabled = cwm->async_present && !cwm->present_worker_enabled && !cwm->drawable_slot_enabled &&
	                               debug_get_bool_option_macos_early_drawable();
	cwm->present_command_group = dispatch_group_create();
	if (cwm->present_worker_enabled || cwm->drawable_slot_enabled) {
		dispatch_queue_attr_t worker_attr =
		    dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
		cwm->present_worker_queue = dispatch_queue_create(cwm->present_worker_enabled ? "org.monado.macos-present-worker"
		                                                                            : "org.monado.macos-drawable-slot",
		                                                  worker_attr);
		cwm->present_worker_group = dispatch_group_create();
	}
	if (want_drawable_slot) {
		if (cwm->drawable_slot_enabled) {
			COMP_INFO(c, "macOS diagnostic: asynchronous one-drawable slot enabled; frames drop rather than block when empty");
		} else {
			COMP_WARN(c, "XRT_MACOS_DRAWABLE_SLOT requires async presentation with XRT_MACOS_PRESENT_WORKER=0; slot is disabled");
		}
	}
	if (debug_get_bool_option_macos_early_drawable()) {
		if (cwm->drawable_slot_enabled) {
			COMP_WARN(c, "XRT_MACOS_DRAWABLE_SLOT and XRT_MACOS_EARLY_DRAWABLE are both set; using drawable slot mode");
		} else if (cwm->early_drawable_enabled) {
			COMP_INFO(c, "macOS diagnostic: early CAMetalDrawable prefetch enabled");
		} else {
			COMP_WARN(c, "XRT_MACOS_EARLY_DRAWABLE requires async presentation with XRT_MACOS_PRESENT_WORKER=0; prefetch is disabled");
		}
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
		return find_psvr2_screen(NULL) != nil;
	}
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

static const char *macos_optional_device_extensions[] = {
	VK_EXT_METAL_OBJECTS_EXTENSION_NAME,
};

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

#pragma clang diagnostic pop

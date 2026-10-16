// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS Metal surface with generic Vulkan swapchain presentation and pacing.
 * @ingroup comp_main
 */
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#import <QuartzCore/QuartzCore.h>

#include "main/comp_window.h"
#include "main/comp_window_macos_hosted.h"
#include "util/u_macos_hosted_client.h"
#include "util/comp_metal_gpu_priority.h"
#include <math.h>
#include <string.h>
#include "main/comp_macos_frontend.h"
#include "main/comp_macos_wsi_passthrough.h"
#include "main/comp_macos_display.h"
#include "main/comp_macos_display_timing.h"
#include "main/comp_present_feedback_correction.h"
#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_pacing.h"
#include "util/u_timing_trace.h"
#include "os/os_time.h"
#include <inttypes.h>
#include <stdio.h>

DEBUG_GET_ONCE_NUM_OPTION(macos_generic_refresh, "XRT_MACOS_REFRESH_RATE_HZ", 0)

DEBUG_GET_ONCE_BOOL_OPTION(macos_wsi_passthrough, "XRT_MACOS_WSI_PASSTHROUGH", false)

DEBUG_GET_ONCE_BOOL_OPTION(macos_generic_wsi, "XRT_MACOS_WSI_GENERIC", false)

// Keep the validated CV source opt-in while comparing Apple's replacement APIs.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
DEBUG_GET_ONCE_BOOL_OPTION(macos_generic_cv, "XRT_MACOS_WSI_GENERIC_CV", false)
DEBUG_GET_ONCE_BOOL_OPTION(macos_generic_calibrate, "XRT_MACOS_WSI_GENERIC_CALIBRATE", false)
DEBUG_GET_ONCE_BOOL_OPTION(macos_generic_adaptive, "XRT_MACOS_WSI_GENERIC_ADAPTIVE", false)
DEBUG_GET_ONCE_NUM_OPTION(macos_generic_latch_us, "XRT_MACOS_WSI_GENERIC_LATCH_US", 0)

#define WSI_TIMING_HISTORY 2048

struct comp_window_macos_wsi
{
	struct comp_target_swapchain base;
	struct comp_macos_frontend *frontend;
	struct u_macos_hosted_client *hosted_client;
	atomic_bool display_lost;
	bool passthrough_warning;
	float physical_rate_hz;
	int64_t last_mode_poll_ns;
	struct comp_macos_frontend_info frontend_info;

	struct comp_macos_display_link *display_link;
	mach_timebase_info_data_t timebase;
	atomic_uint_fast64_t latest_vblank_ns;
	int64_t period_ns;
	bool use_cv, calibrate;

	//! Corrects feedback for the adaptive display-timing pacer, without CVDisplayLink.
	bool adaptive;
	struct comp_present_feedback_correction correction;
	int64_t base_present_offset_ns;
	int64_t applied_delay_ns;
	struct comp_macos_wsi_passthrough *passthrough;
	uint64_t last_vblank_ns;
	int64_t calibrated_offset_ns;
	uint32_t offset_samples;
	struct
	{
		int64_t id, desired_ns, gpu_end_ns;
		uint64_t ticket;
		bool valid, has_gpu_end;
	} history[WSI_TIMING_HISTORY];

	// Experiment instrumentation; never write files from the display-link callback.
	struct comp_target ops;
	FILE *feedback, *acquire, *gpu, *refresh, *vblank, *offset, *adaptive_trace;
};

static struct comp_window_macos_wsi *
from_target(struct comp_target *ct)
{
	return (struct comp_window_macos_wsi *)ct;
}

// Mirror the native target's host-clock conversion and output-phase projection.
// Only publish a timestamp here: the compositor thread owns the pacer and traces.
static void
cv_tick(void *context, uint64_t now_host_ns, uint64_t output_host_ns)
{
	struct comp_window_macos_wsi *w = context;
	if (now_host_ns == 0 || output_host_ns == 0 || w->period_ns <= 0) {
		return;
	}
	int64_t before_ns = os_monotonic_get_ns();
	uint64_t host_ns = (uint64_t)(((__uint128_t)CVGetCurrentHostTime() * w->timebase.numer) / w->timebase.denom);
	int64_t after_ns = os_monotonic_get_ns();
	int64_t offset_ns = before_ns + (after_ns - before_ns) / 2 - (int64_t)host_ns;
	int64_t now_ns = (int64_t)now_host_ns + offset_ns;
	int64_t output_ns = (int64_t)output_host_ns + offset_ns;
	if (now_ns <= 0 || output_ns <= 0) {
		return;
	}
	uint64_t vblank_ns = comp_macos_last_vblank_ns((uint64_t)output_ns, (uint64_t)now_ns, (uint64_t)w->period_ns);
	if (vblank_ns > 0) {
		atomic_store_explicit(&w->latest_vblank_ns, vblank_ns, memory_order_release);
	}
}

static VkResult
update_timings(struct comp_target *ct)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	if (w->display_lost) {
		return VK_ERROR_OUT_OF_DATE_KHR;
	}
	// Preserve generic feedback delivery, including the optional calibration observer.
	VkResult ret = w->ops.update_timings(ct);
	uint64_t vblank_ns = atomic_exchange_explicit(&w->latest_vblank_ns, 0, memory_order_acquire);
	if (vblank_ns != 0 && vblank_ns > w->last_vblank_ns && w->base.upc != NULL) {
		u_pc_update_vblank_from_display_control(w->base.upc, (int64_t)vblank_ns);
		if (w->vblank != NULL) {
			fprintf(w->vblank, "%llu,%llu,%" PRIi64 "\n", (unsigned long long)os_monotonic_get_ns(),
			        (unsigned long long)vblank_ns, w->period_ns);
		}
		w->last_vblank_ns = vblank_ns;
	}
	return ret;
}

static xrt_result_t
get_rates(struct comp_target *ct, uint32_t *count, float *rates)
{
	*count =
	    comp_macos_display_rates(from_target(ct)->frontend_info.display_id, XRT_MAX_SUPPORTED_REFRESH_RATES, rates);
	return *count > 0 ? XRT_SUCCESS : XRT_ERROR_OUTPUT_REQUEST_FAILURE;
}

static xrt_result_t
get_rate(struct comp_target *ct, float *rate)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	CGDirectDisplayID display = w->frontend_info.display_id;
	CGDisplayModeRef mode = CGDisplayIsActive(display) ? CGDisplayCopyDisplayMode(display) : NULL;
	if (mode == NULL) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	if (CGDisplayModeGetPixelWidth(mode) != w->frontend_info.pixel_width ||
	    CGDisplayModeGetPixelHeight(mode) != w->frontend_info.pixel_height) {
		CGDisplayModeRelease(mode);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	*rate = (float)CGDisplayModeGetRefreshRate(mode);
	CGDisplayModeRelease(mode);
	return *rate > 0 ? XRT_SUCCESS : XRT_ERROR_OUTPUT_REQUEST_FAILURE;
}

/*
 * Called at initialization, and by the compositor between frames or while no
 * session runs (see comp_refresh_request), so the pacer and display link are idle.
 */
static xrt_result_t
apply_refresh(struct comp_target *ct, float hz)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	float selected_hz = 0, current_hz = 0;
	CGDirectDisplayID display = w->frontend_info.display_id;
	CGDisplayModeRef mode = comp_macos_display_copy_mode(display, hz, &selected_hz);
	if (mode == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	(void)get_rate(ct, &current_hz);
	if (fabsf(current_hz - selected_hz) < 0.05f && fabsf(w->physical_rate_hz - selected_hz) < 0.05f) {
		CGDisplayModeRelease(mode);
		return XRT_SUCCESS;
	}
	struct vk_bundle *vk = &ct->c->base.vk;
	if (w->base.swapchain.handle != VK_NULL_HANDLE) {
		if (vk->vkDeviceWaitIdle(vk->device) != VK_SUCCESS) {
			CGDisplayModeRelease(mode);
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
		}
		(void)w->ops.update_timings(ct);
	}
	bool restart = comp_macos_display_link_running(w->display_link);
	comp_macos_display_link_destroy(&w->display_link);
	bool result = fabsf(current_hz - selected_hz) < 0.05f || comp_macos_display_set_mode(display, mode);
	CGDisplayModeRelease(mode);
	if (!result) {
		COMP_ERROR(ct->c, "WSI physical mode did not settle at %.3f Hz", selected_hz);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	float actual_hz = 0;
	if (get_rate(ct, &actual_hz) != XRT_SUCCESS) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	w->physical_rate_hz = actual_hz;
	w->period_ns = (int64_t)llround(1e9 / actual_hz);
	if (w->use_cv) {
		w->display_link = comp_macos_display_link_create(display, cv_tick, w);
		if (w->display_link == NULL || comp_macos_display_link_period(w->display_link) <= 0) {
			w->display_lost = true;
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
		}
		w->period_ns = comp_macos_display_link_period(w->display_link);
	}
	atomic_store_explicit(&w->latest_vblank_ns, 0, memory_order_release);
	w->last_vblank_ns = 0;
	w->offset_samples = 0;
	w->calibrated_offset_ns = 0;
	memset(w->history, 0, sizeof(w->history));
	comp_present_feedback_correction_reset(&w->correction, w->period_ns);
	if (w->adaptive && w->applied_delay_ns != 0 && w->base.upc != NULL) {
		u_pc_update_present_offset(w->base.upc, 0, w->base_present_offset_ns);
	}
	w->applied_delay_ns = 0;
	ct->c->frame_interval_ns = (uint64_t)w->period_ns;
	if (w->base.upc != NULL && !u_pc_update_frame_period(w->base.upc, w->period_ns)) {
		w->display_lost = true;
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	if (restart && w->display_link != NULL && !comp_macos_display_link_start(w->display_link)) {
		w->display_lost = true;
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	COMP_INFO(ct->c, "WSI physical refresh %.3f -> %.3f Hz; pacing IDs preserved", current_hz, actual_hz);
	return XRT_SUCCESS;
}

static xrt_result_t
request_rate(struct comp_target *ct, float hz)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	if (!isfinite(hz) || hz <= 0) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	// Unsupported requests change nothing.
	float selected;
	CGDisplayModeRef mode = comp_macos_display_copy_mode(w->frontend_info.display_id, hz, &selected);
	if (mode == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	CGDisplayModeRelease(mode);
	return apply_refresh(ct, hz);
}

static void
calc_frame_pacing(struct comp_target *ct, int64_t *id, int64_t *wake, int64_t *desired, int64_t *slop, int64_t *display)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	// Follow a mode change made outside Monado, such as in System Settings.
	int64_t now = os_monotonic_get_ns();
	if (!w->display_lost && now - w->last_mode_poll_ns > 500000000) {
		w->last_mode_poll_ns = now;
		float physical = 0;
		if (CGDisplayIsActive(w->frontend_info.display_id) && get_rate(ct, &physical) == XRT_SUCCESS &&
		    fabsf(physical - w->physical_rate_hz) > 0.05f) {
			if (apply_refresh(ct, physical) != XRT_SUCCESS) {
				w->display_lost = true;
			}
		}
	}
	w->ops.calc_frame_pacing(ct, id, wake, desired, slop, display);
}

static bool
check_ready(struct comp_target *ct)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	if (w->display_lost || !CGDisplayIsActive(w->frontend_info.display_id)) {
		if (!w->display_lost) {
			COMP_ERROR(ct->c, "WSI display is inactive; restart the target after reconnecting");
			w->display_lost = true;
		}
		return false;
	}
	return w->ops.check_ready(ct);
}

static bool
init_window(struct comp_target *ct)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	w->frontend = comp_macos_frontend_create(ct->c, w->hosted_client, &w->frontend_info);
	if (w->frontend == NULL) {
		return false;
	}
	CAMetalLayer *layer = w->frontend_info.metal_layer;
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (device == nil) {
		return false;
	}
	layer.device = device;
	[device release];
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = NO;
	layer.displaySyncEnabled = YES;
	layer.maximumDrawableCount = 3;
	layer.drawableSize = CGSizeMake(w->frontend_info.pixel_width, w->frontend_info.pixel_height);
	CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	layer.colorspace = color;
	CGColorSpaceRelease(color);
	w->base.override.compositor_extent = true;
	w->base.override.extent = (VkExtent2D){w->frontend_info.pixel_width, w->frontend_info.pixel_height};
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(w->frontend_info.display_id);
	double hz = mode != NULL ? CGDisplayModeGetRefreshRate(mode) : 0;
	if (mode != NULL) {
		CGDisplayModeRelease(mode);
	}
	if (hz <= 0) {
		COMP_ERROR(ct->c, "Generic WSI requires a display mode with a known refresh rate");
		return false;
	}
	w->physical_rate_hz = (float)hz;
	ct->c->frame_interval_ns = (uint64_t)(1e9 / hz);
	w->period_ns = (int64_t)ct->c->frame_interval_ns;
	if (w->use_cv) {
		mach_timebase_info(&w->timebase);
		w->display_link = comp_macos_display_link_create(w->frontend_info.display_id, cv_tick, w);
		w->period_ns = comp_macos_display_link_period(w->display_link);
		if (w->display_link == NULL || w->period_ns <= 0) {
			COMP_ERROR(ct->c, "Could not initialize generic WSI CVDisplayLink with a valid period");
			return false;
		}
		ct->c->frame_interval_ns = (uint64_t)w->period_ns;
	}
	comp_present_feedback_correction_reset(&w->correction, w->period_ns);
	// GOOGLE display timing needs no conversion: MoltenVK reports Mach absolute time,
	// the same CLOCK_UPTIME_RAW domain as os_monotonic_get_ns() on macOS.
	int requested_hz = debug_get_num_option_macos_generic_refresh();
	if (requested_hz > 0 && apply_refresh(ct, (float)requested_hz) != XRT_SUCCESS) {
		COMP_ERROR(ct->c, "Could not apply WSI startup refresh %d Hz", requested_hz);
		return false;
	}
	comp_macos_frontend_show(w->frontend);
	return true;
}

static bool
init_surface(struct comp_target *ct, uint32_t width, uint32_t height)
{
	(void)width;
	(void)height;
	struct comp_window_macos_wsi *w = from_target(ct);
	struct vk_bundle *vk = &ct->c->base.vk;
	if (!vk->has_GOOGLE_display_timing) {
		COMP_ERROR(ct->c, "Generic WSI comparison requires GOOGLE display timing");
		return false;
	}
	// Same XRT_MACOS_GPU_QUEUE_PRIORITY handling as the native target; MoltenVK presents on this queue.
	comp_metal_gpu_priority_apply(vk, NULL);
	VkMetalSurfaceCreateInfoEXT info = {
	    .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
	    .pLayer = w->frontend_info.metal_layer,
	};
	return vk->vkCreateMetalSurfaceEXT(vk->instance, &info, NULL, &w->base.surface.handle) == VK_SUCCESS;
}

static void
flush_window(struct comp_target *ct)
{
	(void)ct;
	[CATransaction flush];
}

static void
set_title(struct comp_target *ct, const char *title)
{
	comp_macos_frontend_set_title(from_target(ct)->frontend, title);
}

static void
destroy(struct comp_target *ct)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	if (w->display_link != NULL) {
		comp_macos_display_link_destroy(&w->display_link);
	}
	comp_macos_wsi_passthrough_destroy(&w->passthrough);
	comp_target_swapchain_cleanup(&w->base);
	comp_macos_frontend_destroy(&w->frontend);
	u_macos_hosted_client_release(w->hosted_client);

	FILE *files[] = {w->feedback, w->acquire, w->gpu, w->refresh, w->vblank, w->offset, w->adaptive_trace};
	for (uint32_t i = 0; i < ARRAY_SIZE(files); ++i) {
		if (files[i] != NULL) {
			fclose(files[i]);
		}
	}
	free(w);
}

// Called for each feedback entry before the generic helper delivers it to the pacer.
static void
trace_feedback(struct comp_target_swapchain *cts, VkPastPresentationTimingGOOGLE *t)
{
	struct comp_window_macos_wsi *w = (struct comp_window_macos_wsi *)cts;
	// Trace what the driver reported, before any correction.
	if (w->feedback != NULL) {
		fprintf(w->feedback, "feedback,%u,%llu,%llu,%llu,0,0,0,0,0,%llu,%llu,0\n", t->presentID,
		        (unsigned long long)os_monotonic_get_ns(), (unsigned long long)t->desiredPresentTime,
		        (unsigned long long)t->actualPresentTime, (unsigned long long)t->earliestPresentTime,
		        (unsigned long long)t->presentMargin);
	}
	uint32_t slot = t->presentID % WSI_TIMING_HISTORY;
	bool known = w->history[slot].valid && w->history[slot].id == t->presentID;
	if (known && t->actualPresentTime > 0) {
		int64_t sample_ns = (int64_t)t->actualPresentTime - w->history[slot].desired_ns;
		comp_macos_frontend_note_presented(w->frontend, w->history[slot].ticket);
		bool accepted = w->calibrate && sample_ns > 0 && sample_ns <= w->period_ns * 4;
		if (accepted) {
			w->calibrated_offset_ns =
			    w->offset_samples == 0 ? sample_ns : (w->calibrated_offset_ns * 7 + sample_ns) / 8;
			if (w->offset_samples < UINT32_MAX) {
				w->offset_samples++;
			}
			if (w->offset_samples >= 8) {
				u_pc_update_present_offset(cts->upc, t->presentID, w->calibrated_offset_ns);
			}
		}
		if (w->offset != NULL) {
			fprintf(w->offset, "%u,%" PRIi64 ",%" PRIi64 ",%u,%d\n", t->presentID, sample_ns,
			        w->calibrated_offset_ns, w->offset_samples, accepted);
		}
	}
	if (w->adaptive && known) {
		int64_t desired_ns = w->history[slot].desired_ns;
		if (t->actualPresentTime > 0) {
			comp_present_feedback_correction_observe(&w->correction, desired_ns,
			                                         (int64_t)t->actualPresentTime);
		}
		uint64_t raw_margin = t->presentMargin;
		comp_present_feedback_correction_apply(&w->correction, desired_ns, w->history[slot].has_gpu_end,
		                                       w->history[slot].gpu_end_ns, &t->actualPresentTime,
		                                       &t->earliestPresentTime, &t->presentMargin);
		// Predicted display times must include the delay removed from the feedback.
		int64_t delay_ns = w->correction.delay_ns;
		if (llabs(delay_ns - w->applied_delay_ns) > 100000 && cts->upc != NULL) {
			u_pc_update_present_offset(cts->upc, t->presentID, w->base_present_offset_ns + delay_ns);
			w->applied_delay_ns = delay_ns;
		}
		if (w->adaptive_trace != NULL) {
			fprintf(w->adaptive_trace, "%u,%" PRIi64 ",%llu,%llu,%d,%" PRIi64 ",%llu\n", t->presentID,
			        delay_ns, (unsigned long long)raw_margin, (unsigned long long)t->presentMargin,
			        w->history[slot].has_gpu_end, w->history[slot].gpu_end_ns,
			        (unsigned long long)t->actualPresentTime);
		}
	}
	if (known) {
		w->history[slot].valid = false;
	}
}

static void
create_images(struct comp_target *ct, const struct comp_target_create_images_info *info, struct vk_bundle_queue *queue)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	struct comp_target_create_images_info fifo = *info;
	comp_macos_wsi_passthrough_destroy(&w->passthrough);
	if (debug_get_bool_option_macos_wsi_passthrough()) {
		fifo.image_usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	}
	fifo.present_mode = VK_PRESENT_MODE_FIFO_KHR;
	w->ops.create_images(ct, &fifo, queue);
	if (w->base.swapchain.handle == VK_NULL_HANDLE) {
		return;
	}
	if (w->display_link != NULL && !comp_macos_display_link_running(w->display_link)) {
		if (!comp_macos_display_link_start(w->display_link)) {
			COMP_ERROR(ct->c, "Could not start generic WSI CVDisplayLink");
			return;
		}
	}
	ct->passthrough_unsupported = true;
	if (debug_get_bool_option_macos_wsi_passthrough()) {
		w->passthrough = comp_macos_wsi_passthrough_create(ct, queue, [w->frontend_info.metal_layer device],
		                                                   w->hosted_client == NULL);
		ct->passthrough_unsupported = w->passthrough == NULL;
		if (w->passthrough == NULL) {
			COMP_ERROR(ct->c, "Could not initialize WSI passthrough interop");
		}
	}
	struct vk_bundle *vk = &ct->c->base.vk;
	VkRefreshCycleDurationGOOGLE duration = {0};
	PFN_vkGetRefreshCycleDurationGOOGLE get_refresh =
	    (PFN_vkGetRefreshCycleDurationGOOGLE)vk->vkGetDeviceProcAddr(vk->device, "vkGetRefreshCycleDurationGOOGLE");
	VkResult ret = get_refresh != NULL ? get_refresh(vk->device, w->base.swapchain.handle, &duration)
	                                   : VK_ERROR_EXTENSION_NOT_PRESENT;
	if (ret == VK_SUCCESS && w->refresh != NULL) {
		fprintf(w->refresh, "%llu\n", (unsigned long long)duration.refreshDuration);
	}
	CAMetalLayer *layer = w->frontend_info.metal_layer;
	CFStringRef color = layer.colorspace != NULL ? CGColorSpaceCopyName(layer.colorspace) : NULL;
	COMP_INFO(
	    ct->c,
	    "EXPERIMENT generic WSI: CVDisplayLink=%d calibration=%d adaptive=%d, %u images, %ux%u, refresh=%llu ns",
	    w->use_cv, w->calibrate, w->adaptive, ct->image_count, ct->width, ct->height,
	    (unsigned long long)duration.refreshDuration);
	COMP_INFO(ct->c,
	          "EXPERIMENT effective layer: framebufferOnly=%d displaySync=%d transaction=%d drawables=%lu "
	          "timeout=%d format=%lu colorspace=%s",
	          layer.framebufferOnly, layer.displaySyncEnabled, layer.presentsWithTransaction,
	          (unsigned long)layer.maximumDrawableCount, layer.allowsNextDrawableTimeout,
	          (unsigned long)layer.pixelFormat, color != NULL ? [(NSString *)color UTF8String] : "none");
	if (color != NULL) {
		CFRelease(color);
	}
	// Properties that decide whether WindowServer can scan the layer out directly.
	COMP_INFO(ct->c,
	          "EXPERIMENT direct-display layer: opaque=%d edr=%d contentsScale=%.2f gravity=%s bounds=%.0fx%.0f "
	          "drawable=%.0fx%.0f hidden=%d opacity=%.2f sublayers=%lu superlayer=%d",
	          layer.opaque, layer.wantsExtendedDynamicRangeContent, layer.contentsScale,
	          [layer.contentsGravity UTF8String], layer.bounds.size.width, layer.bounds.size.height,
	          layer.drawableSize.width, layer.drawableSize.height, layer.hidden, layer.opacity,
	          (unsigned long)layer.sublayers.count, layer.superlayer != nil);
}

static VkResult
acquire(struct comp_target *ct, uint32_t *index)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	if (w->display_lost) {
		return VK_ERROR_SURFACE_LOST_KHR;
	}
	uint64_t begin = os_monotonic_get_ns();
	VkResult ret = w->ops.acquire(ct, index);
	if (w->acquire != NULL) {
		fprintf(w->acquire, "%" PRIi64 ",%llu,%llu,%u,%d\n", w->base.current_frame_id,
		        (unsigned long long)begin, (unsigned long long)os_monotonic_get_ns(),
		        ret == VK_SUCCESS || ret == VK_SUBOPTIMAL_KHR ? *index : UINT32_MAX, ret);
	}
	return ret;
}

static VkResult
present(struct comp_target *ct,
        struct vk_bundle_queue *queue,
        uint32_t index,
        uint64_t value,
        int64_t desired,
        int64_t slop)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	{
		uint32_t slot = (uint64_t)w->base.current_frame_id % WSI_TIMING_HISTORY;
		w->history[slot].id = w->base.current_frame_id;
		w->history[slot].desired_ns = desired;
		w->history[slot].ticket = comp_macos_frontend_present_ticket(w->frontend);
		w->history[slot].has_gpu_end = false;
		w->history[slot].valid = true;
	}
	VkResult composed = VK_SUCCESS;
	if (ct->c->passthrough_active) {
		bool present_ready = true;
		if (w->passthrough == NULL) {
			if (!w->passthrough_warning) {
				COMP_WARN(ct->c,
				          "WSI passthrough unavailable; create must be rejected unless the bridge is "
				          "enabled");
				w->passthrough_warning = true;
			}
		} else {
			composed = comp_macos_wsi_passthrough_compose(
			    w->passthrough, index, ct->c->passthrough_has_application_layers, &present_ready);
		}
		if (!present_ready) {
			// Do not acquire more images with uncertain layout/semaphore state.
			// Swapchain destruction on target teardown releases the acquired image.
			w->display_lost = true;
			return composed;
		}
	}
	uint64_t begin = os_monotonic_get_ns();
	VkResult ret = w->ops.present(ct, queue, index, value, desired, slop);
	uint64_t end = os_monotonic_get_ns();
	if (ret == VK_SUCCESS && composed != VK_SUCCESS && composed != VK_NOT_READY) {
		COMP_ERROR(ct->c, "WSI passthrough failed (%d); acquired image presented after restoration", composed);
		ret = composed;
	}
	if (w->feedback != NULL) {
		fprintf(w->feedback, "submit,%" PRIi64 ",%llu,%" PRIi64 ",0,%llu,%llu,0,0,%d,0,0,%" PRIi64 "\n",
		        w->base.current_frame_id, (unsigned long long)end, desired, (unsigned long long)begin,
		        (unsigned long long)end, ret, slop);
	}
	return ret;
}

static void
info_gpu(struct comp_target *ct, int64_t id, int64_t start, int64_t end, int64_t when)
{
	struct comp_window_macos_wsi *w = from_target(ct);
	w->ops.info_gpu(ct, id, start, end, when);
	uint32_t slot = (uint64_t)id % WSI_TIMING_HISTORY;
	if (w->history[slot].valid && w->history[slot].id == id) {
		w->history[slot].gpu_end_ns = end;
		w->history[slot].has_gpu_end = true;
	}
	if (w->gpu != NULL) {
		fprintf(w->gpu, "%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 "\n", id, start, end, when);
	}
}

static FILE *
open_trace(const char *name, const char *header)
{
	if (!u_timing_trace_enabled()) {
		return NULL;
	}
	FILE *f = u_timing_trace_open(name, 65536);
	if (f != NULL) {
		fprintf(f, "%s\n", header);
	}
	return f;
}

static bool
detect(const struct comp_target_factory *factory, struct comp_compositor *c)
{
	(void)factory;
	(void)c;
	return debug_get_bool_option_macos_generic_wsi() && comp_macos_frontend_detect();
}

bool
comp_window_macos_wsi_create_hosted(struct comp_compositor *c,
                                    struct u_macos_hosted_client *client,
                                    struct comp_target **out)
{
	struct comp_window_macos_wsi *w = U_TYPED_CALLOC(struct comp_window_macos_wsi);
	if (w == NULL) {
		return false;
	}
	w->hosted_client = client;
	u_macos_hosted_client_reference(client);
	w->use_cv = debug_get_bool_option_macos_generic_cv();
	w->calibrate = w->use_cv && debug_get_bool_option_macos_generic_calibrate();
	w->adaptive = debug_get_bool_option_macos_generic_adaptive();
	if (w->adaptive && w->use_cv) {
		COMP_WARN(c, "XRT_MACOS_WSI_GENERIC_ADAPTIVE needs the display-timing pacer; ignored with _CV");
		w->adaptive = false;
	}
	struct u_pc_display_timing_config pacer_config = u_pc_display_timing_get_default_config();
	w->correction.latch_ns = (int64_t)debug_get_num_option_macos_generic_latch_us() * 1000;
	w->correction.neutral_margin_ns = pacer_config.margin_ns;
	w->base_present_offset_ns = pacer_config.present_to_display_offset_ns;
	atomic_init(&w->latest_vblank_ns, 0);
	atomic_init(&w->display_lost, false);
	comp_target_swapchain_init_and_set_fnptrs(&w->base, w->use_cv ? COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING
	                                                              : COMP_TARGET_USE_DISPLAY_IF_AVAILABLE);
	w->ops = w->base.base;
	struct comp_target *ct = &w->base.base;
	ct->c = c;
	ct->name = "macOS generic Vulkan WSI (experimental)";
	ct->init_pre_vulkan = init_window;
	ct->init_post_vulkan = init_surface;
	ct->destroy = destroy;
	ct->flush = flush_window;
	ct->set_title = set_title;
	ct->create_images = create_images;
	ct->acquire = acquire;
	ct->present = present;
	ct->info_gpu = info_gpu;
	ct->update_timings = update_timings;
	ct->calc_frame_pacing = calc_frame_pacing;
	ct->check_ready = check_ready;
	ct->get_refresh_rates = get_rates;
	ct->get_current_refresh_rate = get_rate;
	ct->request_refresh_rate = request_rate;
	ct->passthrough_unsupported = true;
	w->base.on_present_feedback = trace_feedback;
	w->feedback = open_trace("wsi",
	                         "event,frame_id,event_ns,desired_present_ns,actual_present_ns,begin_ns,end_ns,credit_"
	                         "wait_ns,pending,result,earliest_present_ns,present_margin_ns,slop_ns");
	w->acquire = open_trace("wsi_acquire", "frame_id,begin_ns,end_ns,image_index,result");
	w->gpu = open_trace("compositor_gpu", "frame_id,start_ns,end_ns,when_ns");
	w->refresh = open_trace("refresh", "display_period_ns");
	if (w->use_cv) {
		w->vblank = open_trace("cv_vblank", "consumed_ns,vblank_ns,display_period_ns");
	}
	if (w->calibrate) {
		w->offset = open_trace("cv_offset", "frame_id,sample_ns,filtered_ns,sample_count,accepted");
	}
	if (w->adaptive) {
		w->adaptive_trace = open_trace("adaptive_feedback",
		                               "frame_id,delay_ns,raw_margin_ns,margin_ns,has_gpu_end,"
		                               "gpu_end_ns,corrected_actual_ns");
	}
	*out = ct;
	return true;
}

static bool
create(const struct comp_target_factory *factory, struct comp_compositor *c, struct comp_target **out)
{
	(void)factory;
	return comp_window_macos_wsi_create_hosted(c, NULL, out);
}

static const char *device_extensions[] = {VK_EXT_METAL_OBJECTS_EXTENSION_NAME};
static const char *instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME};
const struct comp_target_factory comp_target_factory_macos_wsi_generic = {
    .name = "macOS generic Vulkan WSI (experimental)",
    .identifier = "macos-wsi-generic",
    .required_instance_extensions = instance_extensions,
    .required_instance_extension_count = ARRAY_SIZE(instance_extensions),
    .optional_device_extensions = device_extensions,
    .optional_device_extension_count = ARRAY_SIZE(device_extensions),
    .disable_peek = true,
    .detect = detect,
    .create_target = create,
};

#pragma clang diagnostic pop

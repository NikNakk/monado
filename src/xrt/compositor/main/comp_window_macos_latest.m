// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief PS VR2 physical refresh-rate switching and CAMetalLayer diagnostics
 *        layered over the macOS Metal target.
 */

/* Wrap the base target; this file overrides its refresh-rate and layer-init hooks. */
#define comp_window_macos_create comp_window_macos_create_legacy
#define comp_target_factory_macos comp_target_factory_macos_legacy
#include "comp_window_macos.m"
#undef comp_target_factory_macos
#undef comp_window_macos_create

struct comp_target *
comp_window_macos_create(struct comp_compositor *c);
extern const struct comp_target_factory comp_target_factory_macos;

DEBUG_GET_ONCE_NUM_OPTION(macos_refresh_rate_hz, "XRT_MACOS_REFRESH_RATE_HZ", 0)

static void
macos_log_refresh_mode_candidates(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	if (cwm->screen == nil) {
		return;
	}

	CGDirectDisplayID display_id = get_display_id(cwm->screen);
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
	COMP_INFO(ct->c,
	          "PS VR2 current CoreGraphics mode: logical %zux%zu, pixels %zux%zu, refresh %.3f Hz",
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
		    CGDisplayModeGetHeight(mode) != current_height || CGDisplayModeGetPixelWidth(mode) != current_pixel_width ||
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
		          "PS VR2 CoreGraphics mode enumeration found no positive refresh rates matching the active geometry");
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
	if (cwm->screen == nil) {
		return 0;
	}
	CGDirectDisplayID display_id = get_display_id(cwm->screen);
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
		    CGDisplayModeGetPixelWidth(mode) != pixel_width || CGDisplayModeGetPixelHeight(mode) != pixel_height) {
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
	if (cwm->screen == nil) {
		return NULL;
	}
	CGDirectDisplayID display_id = get_display_id(cwm->screen);
	CGDisplayModeRef current_mode = display_id != kCGNullDirectDisplay ? CGDisplayCopyDisplayMode(display_id) : NULL;
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
		    CGDisplayModeGetPixelWidth(mode) != pixel_width || CGDisplayModeGetPixelHeight(mode) != pixel_height) {
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
	CGDirectDisplayID display_id = cwm->screen != nil ? get_display_id(cwm->screen) : kCGNullDirectDisplay;
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
	if (cwm->display_link != NULL) {
		CVDisplayLinkStop(cwm->display_link);
		CVDisplayLinkRelease(cwm->display_link);
		cwm->display_link = NULL;
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
		return false;
	}
	CVTime period = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(cwm->display_link);
	if ((period.flags & kCVTimeIsIndefinite) == 0 && period.timeValue > 0 && period.timeScale > 0) {
		cwm->display_period_ns = (int64_t)(((__int128)period.timeValue * U_TIME_1S_IN_NS) / period.timeScale);
	}
	refresh_host_to_monotonic_offset_ns(cwm);
	atomic_store_explicit(&cwm->latest_vblank_ns, 0, memory_order_release);
	atomic_store_explicit(&cwm->latest_displaylink_now_host_ns, 0, memory_order_release);
	atomic_store_explicit(&cwm->latest_displaylink_output_host_ns, 0, memory_order_release);
	atomic_store_explicit(&cwm->latest_displaylink_now_ns, 0, memory_order_release);
	atomic_store_explicit(&cwm->latest_displaylink_output_ns, 0, memory_order_release);
	atomic_store_explicit(&cwm->latest_displaylink_callback_ns, 0, memory_order_release);
	cwm->last_vblank_ns = 0;
	cwm->cadence_sample_count = 0;
	cwm->cadence_total_ns = 0;
	cwm->cadence_min_ns = 0;
	cwm->cadence_max_ns = 0;
	cwm->present_offset_sample_count = 0;
	cwm->calibrated_present_offset_ns = 0;
	cwm->consumed_present_offset_sample_serial = 0;
	if (start_link) {
		cvret = CVDisplayLinkStart(cwm->display_link);
		if (cvret != kCVReturnSuccess) {
			return false;
		}
	}
	return true;
}

static xrt_result_t
comp_window_macos_request_refresh_rate_physical(struct comp_target *ct, float requested_hz)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	if (cwm->screen == nil) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	CGDirectDisplayID display_id = get_display_id(cwm->screen);
	if (display_id == kCGNullDirectDisplay) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	float selected_hz = 0.0f;
	CGDisplayModeRef selected_mode = macos_copy_refresh_mode(cwm, requested_hz, &selected_hz);
	if (selected_mode == NULL) {
		COMP_WARN(ct->c, "PS VR2 refresh request %.3f Hz does not match an available same-geometry mode", requested_hz);
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
	bool display_link_was_running = cwm->display_link != NULL && CVDisplayLinkIsRunning(cwm->display_link);
	if (cwm->display_link != NULL) {
		CVDisplayLinkStop(cwm->display_link);
	}

	CGError cgret = CGDisplaySetDisplayMode(display_id, selected_mode, NULL);
	CGDisplayModeRelease(selected_mode);
	if (cgret != kCGErrorSuccess) {
		COMP_ERROR(ct->c, "Failed to switch PS VR2 physical refresh from %.3f to %.3f Hz (CGError %d)", current_hz,
		           selected_hz, (int)cgret);
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
	COMP_INFO(ct->c, "PS VR2 physical refresh switched %.3f -> %.3f Hz; measured compositor period %.3fms (%.3f Hz)",
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
		xrt_result_t refresh_ret = comp_window_macos_request_refresh_rate_physical(ct, (float)requested_refresh_hz);
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

	COMP_INFO(ct->c,
	          "macOS CAMetalLayer state: framebufferOnly=%s displaySyncEnabled=%s "
	          "presentsWithTransaction=%s maximumDrawableCount=%lu allowsNextDrawableTimeout=%s",
	          [layer framebufferOnly] ? "true" : "false", [layer displaySyncEnabled] ? "true" : "false",
	          [layer presentsWithTransaction] ? "true" : "false", (unsigned long)[layer maximumDrawableCount],
	          [layer allowsNextDrawableTimeout] ? "true" : "false");
	return true;
}

struct comp_target *
comp_window_macos_create(struct comp_compositor *c)
{
	struct comp_target *ct = comp_window_macos_create_legacy(c);
	if (ct == NULL) {
		return NULL;
	}
	ct->init_pre_vulkan = comp_window_macos_init_with_refresh_rate;
	ct->get_refresh_rates = comp_window_macos_get_refresh_rates_physical;
	ct->get_current_refresh_rate = comp_window_macos_get_current_refresh_rate_physical;
	ct->request_refresh_rate = comp_window_macos_request_refresh_rate_physical;

	return ct;
}

static bool
create_target_physical_refresh(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
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
	.create_target = create_target_physical_refresh,
};
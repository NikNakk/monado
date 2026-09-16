// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental CAMetalDisplayLink trigger for the Multi Client Module.
 *
 * Force-include this after comp_multi_system_macos_trace.h. It does not replace
 * multi_main_loop(): it replaces that loop's predict-frame call with a blocking
 * wait for one CAMetalDisplayLink tick and suppresses the loop's u_wait_until()
 * only for a frame that actually consumed such a tick. The existing layer
 * transfer and native compositor render stay on the established Multi Client
 * Module thread.
 */
#pragma once

#include "multi/comp_multi_macos_displaylink.h"
#include "os/os_time.h"
#include "util/u_wait.h"

static _Thread_local bool g_macos_displaylink_tick_active = false;

/* The trace header has already defined its predict wrapper and source macro. */
#ifdef xrt_comp_predict_frame
#undef xrt_comp_predict_frame
#endif

static inline void
macos_xrt_comp_predict_frame_from_displaylink(struct xrt_compositor *xc,
                                              int64_t *out_frame_id,
                                              int64_t *out_wake_up_time_ns,
                                              int64_t *out_predicted_gpu_time_ns,
                                              int64_t *out_predicted_display_time_ns,
                                              int64_t *out_predicted_display_period_ns)
{
	uint64_t callback_ns = 0;
	uint64_t target_ns = 0;
	uint64_t presentation_ns = 0;
	bool driven = comp_multi_macos_displaylink_wait_tick(&callback_ns, &target_ns, &presentation_ns);
	g_macos_displaylink_tick_active = driven;

	/* Preserve Monado's native frame-id/pacing bookkeeping. */
	macos_xrt_comp_predict_frame_with_time_constraint(xc, out_frame_id, out_wake_up_time_ns,
	                                                  out_predicted_gpu_time_ns,
	                                                  out_predicted_display_time_ns,
	                                                  out_predicted_display_period_ns);

	if (!driven) {
		return;
	}

	/*
	 * CAMetalDisplayLink is now the pacing primitive. Keep the native compositor's
	 * predicted display bookkeeping intact for this first experiment; the CAMetal
	 * timestamps are independently available to the target and can replace native
	 * prediction in a later experiment if needed.
	 */
	(void)target_ns;
	(void)presentation_ns;
	*out_wake_up_time_ns = (int64_t)(callback_ns != 0 ? callback_ns : os_monotonic_get_ns());
}

#define xrt_comp_predict_frame(xc, out_frame_id, out_wake_up_time_ns, out_predicted_gpu_time_ns,                     \
                               out_predicted_display_time_ns, out_predicted_display_period_ns)                       \
	macos_xrt_comp_predict_frame_from_displaylink((xc), (out_frame_id), (out_wake_up_time_ns),                       \
	                                              (out_predicted_gpu_time_ns),                                         \
	                                              (out_predicted_display_time_ns),                                     \
	                                              (out_predicted_display_period_ns))

/*
 * This is the strict "no mach_wait_until/no full spin" part of the experiment.
 * comp_multi_system.c has one u_wait_until() in its render loop. If this frame
 * was released by CAMetalDisplayLink, the callback itself is the pacing primitive
 * and the platform wait must not run. A rare 100-ms bridge timeout deliberately
 * falls back to the ordinary Monado wait path for clean shutdown/failure handling.
 */
static inline void
macos_u_wait_until_displaylink(struct os_precise_sleeper *sleeper, int64_t wake_up_time_ns)
{
	if (g_macos_displaylink_tick_active) {
		(void)sleeper;
		(void)wake_up_time_ns;
		return;
	}
	u_wait_until(sleeper, wake_up_time_ns);
}

#define u_wait_until(sleeper, wake_up_time_ns) macos_u_wait_until_displaylink((sleeper), (wake_up_time_ns))

static inline xrt_result_t
macos_xrt_comp_layer_commit_complete_displaylink(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	xrt_result_t ret = xrt_comp_layer_commit(xc, sync_handle);
	if (g_macos_displaylink_tick_active) {
		comp_multi_macos_displaylink_complete_tick();
		g_macos_displaylink_tick_active = false;
	}
	return ret;
}

#define xrt_comp_layer_commit(xc, sync_handle)                                                                         \
	macos_xrt_comp_layer_commit_complete_displaylink((xc), (sync_handle))

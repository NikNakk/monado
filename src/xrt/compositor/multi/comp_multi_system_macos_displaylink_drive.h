// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental CAMetalDisplayLink trigger for the Multi Client Module.
 *
 * Force-include this after comp_multi_system_macos_trace.h. It does not replace
 * multi_main_loop(): it replaces that loop's predict-frame call with a blocking
 * wait for one CAMetalDisplayLink tick and suppresses the loop's u_wait_until()
 * while the experiment is enabled. The existing layer transfer and native
 * compositor render stay on the established Multi Client Module thread.
 *
 * Tick completion is deliberately NOT signalled from xrt_comp_layer_commit().
 * On macOS that call may hand presentation to an asynchronous present worker.
 * The CAMetalDisplayLink callback must retain update.drawable until the target
 * has actually scheduled that exact drawable for presentation; the macOS target
 * therefore calls comp_multi_macos_displaylink_complete_tick() from its plain
 * presentation path instead.
 */
#pragma once

#include "multi/comp_multi_macos_displaylink.h"
#include "os/os_time.h"
#include "util/u_wait.h"

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
	if (comp_multi_macos_displaylink_active()) {
		/* The native compositor reads the consumed tick before saving its frame
		 * state, so its renderer and these client-facing outputs stay aligned. */
		(void)comp_multi_macos_displaylink_wait_tick(NULL, NULL, NULL);
	}
	macos_xrt_comp_predict_frame_with_time_constraint(xc, out_frame_id, out_wake_up_time_ns,
	                                                  out_predicted_gpu_time_ns,
	                                                  out_predicted_display_time_ns,
	                                                  out_predicted_display_period_ns);
}

#define xrt_comp_predict_frame(xc, out_frame_id, out_wake_up_time_ns, out_predicted_gpu_time_ns,                     \
                               out_predicted_display_time_ns, out_predicted_display_period_ns)                       \
	macos_xrt_comp_predict_frame_from_displaylink((xc), (out_frame_id), (out_wake_up_time_ns),                       \
	                                              (out_predicted_gpu_time_ns),                                         \
	                                              (out_predicted_display_time_ns),                                     \
	                                              (out_predicted_display_period_ns))

/*
 * Strict experiment invariant: when CAMetalDisplayLink drive mode is enabled,
 * comp_multi_system.c must never enter its legacy u_wait_until()/mach_wait_until
 * path. The bridge's condition-variable wait is the only CPU pacing primitive.
 * If callbacks stop, its bounded failure/teardown timeout may produce a dropped
 * frame, but must not silently fall back to the old display scheduler.
 */
static inline void
macos_u_wait_until_displaylink(struct os_precise_sleeper *sleeper, int64_t wake_up_time_ns)
{
	if (comp_multi_macos_displaylink_active()) {
		(void)sleeper;
		(void)wake_up_time_ns;
		return;
	}
	u_wait_until(sleeper, wake_up_time_ns);
}

#define u_wait_until(sleeper, wake_up_time_ns) macos_u_wait_until_displaylink((sleeper), (wake_up_time_ns))

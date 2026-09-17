// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental CAMetalDisplayLink trigger for the Multi Client Module.
 *
 * Force-include this after comp_multi_system_macos_trace.h. It does not replace
 * multi_main_loop(): it changes only where CAMetalDisplayLink participates in
 * the established predict/wait/render sequence.
 *
 * Driven mode must consume the CAMetal tick before xrt_comp_predict_frame(): the
 * native compositor uses that callback's target/presentation timestamps and its
 * callback-owned drawable for the frame.
 *
 * Hybrid mode is deliberately narrower. Prediction remains completely native/
 * legacy. The ordinary u_wait_until(wake_up_time_ns) call inside wait_frame() is
 * replaced with a condition-variable wait for the independent child-layer
 * CAMetalDisplayLink callback. Thus the only intended difference from legacy is
 * the CPU wake primitive at the existing wake point.
 *
 * Tick completion is deliberately NOT signalled from xrt_comp_layer_commit().
 * In driven mode that call may hand presentation to an asynchronous present
 * worker, so the callback must retain update.drawable until the target has
 * actually scheduled that exact drawable. Hybrid owns no HMD drawable and its
 * callback returns immediately.
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
	/* Driven mode needs the callback timing before native prediction. Hybrid does
	 * not: it predicts first and substitutes its callback for u_wait_until below. */
	if (comp_multi_macos_displaylink_active() && comp_multi_macos_displaylink_driven_mode()) {
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
 * Preserve the source-level wait point so hybrid differs from legacy only in the
 * primitive used to release wait_frame():
 *
 *   legacy: u_wait_until(sleeper, native_wake_time)
 *   hybrid: predict native timing, then wait for the child CAMetal callback here
 *   driven: callback was already consumed before prediction, so do not wait twice
 *
 * The bridge's 100 ms condition-variable timeout remains only a failure/teardown
 * escape hatch; healthy CAMetal cadence is callback-driven.
 */
static inline void
macos_u_wait_until_displaylink(struct os_precise_sleeper *sleeper, int64_t wake_up_time_ns)
{
	if (comp_multi_macos_displaylink_active()) {
		if (comp_multi_macos_displaylink_hybrid_mode()) {
			(void)comp_multi_macos_displaylink_wait_tick(NULL, NULL, NULL);
		}
		/* Driven already waited before prediction. Hybrid just waited above. */
		(void)sleeper;
		(void)wake_up_time_ns;
		return;
	}
	u_wait_until(sleeper, wake_up_time_ns);
}

#define u_wait_until(sleeper, wake_up_time_ns) macos_u_wait_until_displaylink((sleeper), (wake_up_time_ns))

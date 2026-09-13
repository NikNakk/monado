// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Optional phase-locked application pacing diagnostic wrapper.
 *
 * The ordinary application pacer remains unchanged unless
 * U_PACING_APP_FORCED_FRAME_DIVISOR is set to a positive integer. In that
 * diagnostic mode the application is released on an exact integer divisor of
 * the physical/system compositor cadence (for example divisor=2 gives 60 Hz on
 * a 120 Hz headset) while the system compositor continues to run every refresh.
 */

#define u_pa_factory_create u_pa_factory_create_legacy
#include "u_pacing_app_base.c"
#undef u_pa_factory_create

DEBUG_GET_ONCE_NUM_OPTION(forced_frame_divisor, "U_PACING_APP_FORCED_FRAME_DIVISOR", 0)

static int
forced_frame_divisor(void)
{
	int divisor = debug_get_num_option_forced_frame_divisor();
	if (divisor <= 0) {
		return 0;
	}
	if (divisor > 16) {
		divisor = 16;
	}
	return divisor;
}

static void
pa_predict_phase_locked(struct u_pacing_app *upa,
                        int64_t now_ns,
                        int64_t *out_frame_id,
                        int64_t *out_wake_up_time,
                        int64_t *out_predicted_display_time,
                        int64_t *out_predicted_display_period)
{
	struct pacing_app *pa = pacing_app(upa);
	int divisor = forced_frame_divisor();
	assert(divisor > 0);

	int64_t frame_id = ++pa->frame_counter;
	*out_frame_id = frame_id;
	DEBUG_PRINT_ID(frame_id);

	int64_t display_period_ns = display_period(pa);
	if (display_period_ns == 0) {
		assert(false && "Have not yet received any samples from timing driver.");
		display_period_ns = U_TIME_1MS_IN_NS * 16;
	}

	int64_t app_period_ns = display_period_ns * divisor;
	int64_t app_and_compositor_time_ns = total_app_and_compositor_time_ns(pa);
	int64_t predict_ns = 0;

	/*
	 * Anchor the first app frame to a real system-compositor prediction, then
	 * advance only by complete app periods. A missed 60 Hz slot on a 120 Hz
	 * display therefore becomes a real 33.3 ms miss, not a 25 ms interval
	 * followed by an 8.3 ms catch-up that shifts phase relative to scanout.
	 */
	if (pa->last_returned_ns > 0) {
		predict_ns = pa->last_returned_ns + app_period_ns;
	} else {
		predict_ns = last_sample_displayed(pa);
		if (predict_ns <= 0) {
			predict_ns = now_ns + app_period_ns;
		}
	}
	while ((predict_ns - app_and_compositor_time_ns) <= now_ns) {
		predict_ns += app_period_ns;
	}

	int64_t frame_time_ns = total_app_time_ns(pa);
	int64_t wake_up_time_ns = should_return_immediately(frame_time_ns, display_period_ns)
	                              ? now_ns
	                              : predict_ns - app_and_compositor_time_ns;
	int64_t gpu_done_time_ns = wake_up_time_ns + frame_time_ns;

	pa->last_returned_ns = predict_ns;
	*out_wake_up_time = wake_up_time_ns;
	*out_predicted_display_time = predict_ns;
	*out_predicted_display_period = app_period_ns;

	size_t index = GET_INDEX_FROM_ID(pa, frame_id);
	struct u_pa_frame *f = &pa->frames[index];
	assert(f->frame_id == -1);
	assert(f->state == U_PA_READY);
	f->state = U_RT_PREDICTED;
	f->frame_id = frame_id;
	f->predicted_frame_time_ns = frame_time_ns;
	f->predicted_wake_up_time_ns = wake_up_time_ns;
	f->predicted_gpu_done_time_ns = gpu_done_time_ns;
	f->predicted_display_time_ns = predict_ns;
	f->predicted_display_period_ns = app_period_ns;
	f->when.predicted_ns = now_ns;

#ifdef U_TRACE_TRACY
	TracyCPlot("App time(ms)", time_ns_to_ms_f(total_app_time_ns(pa)));
#endif
}

static void
pa_info_phase_locked(struct u_pacing_app *upa,
                     int64_t predicted_display_time_ns,
                     int64_t predicted_display_period_ns,
                     int64_t extra_ns)
{
	struct pacing_app *pa = pacing_app(upa);
	int64_t old_period_ns = pa->last_input.predicted_display_period_ns;
	pa_info(upa, predicted_display_time_ns, predicted_display_period_ns, extra_ns);
	if (old_period_ns != 0 && old_period_ns != predicted_display_period_ns) {
		/* Re-anchor after a physical 90/120 Hz mode change. */
		pa->last_returned_ns = 0;
	}
}

static xrt_result_t
paf_create_phase_locked(struct u_pacing_app_factory *upaf, struct u_pacing_app **out_upa)
{
	xrt_result_t ret = paf_create(upaf, out_upa);
	if (ret != XRT_SUCCESS || *out_upa == NULL) {
		return ret;
	}

	int divisor = forced_frame_divisor();
	if (divisor > 0) {
		(*out_upa)->predict = pa_predict_phase_locked;
		(*out_upa)->info = pa_info_phase_locked;
		U_LOG_I("App pacing diagnostic: forcing phase-locked 1:%d cadence relative to the system display", divisor);
	}
	return ret;
}

xrt_result_t
u_pa_factory_create(struct u_pacing_app_factory **out_upaf)
{
	xrt_result_t ret = u_pa_factory_create_legacy(out_upaf);
	if (ret != XRT_SUCCESS || *out_upaf == NULL) {
		return ret;
	}
	if (forced_frame_divisor() > 0) {
		(*out_upaf)->create = paf_create_phase_locked;
	}
	return ret;
}

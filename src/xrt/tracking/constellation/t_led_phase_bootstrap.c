// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Brightness-driven LED phase bootstrap for constellation-tracked controllers.
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "t_led_phase_bootstrap.h"

#include "math/m_api.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>


#define LOG_I(b, ...) U_LOG_IFL_I((b)->options.log_level, __VA_ARGS__)
#define LOG_W(b, ...) U_LOG_IFL_W((b)->options.log_level, __VA_ARGS__)


/*
 *
 * Helpers
 *
 */

time_duration_ns
t_led_phase_bootstrap_wrap(time_duration_ns offset_ns, time_duration_ns period_ns)
{
	if (period_ns <= 0) {
		return offset_ns;
	}
	time_duration_ns r = offset_ns % period_ns;
	return r < 0 ? r + period_ns : r;
}

static const char *
state_name(enum t_led_phase_bootstrap_state state)
{
	switch (state) {
	case T_LED_PHASE_BOOTSTRAP_IDLE: return "idle";
	case T_LED_PHASE_BOOTSTRAP_WIDE_SCAN: return "wide";
	case T_LED_PHASE_BOOTSTRAP_NARROW_SCAN: return "narrow";
	case T_LED_PHASE_BOOTSTRAP_LOCKED: return "locked";
	case T_LED_PHASE_BOOTSTRAP_BASELINE: return "baseline";
	default: return "unknown";
	}
}

static void
set_output(struct t_led_phase_bootstrap *b, time_duration_ns fudge_offset_ns, time_duration_ns blink_ns)
{
	b->fudge_offset_ns = t_led_phase_bootstrap_wrap(fudge_offset_ns, b->period_ns);
	b->blink_ns = blink_ns;
	b->output_generation++;
}

static void
reset_step_window(struct t_led_phase_bootstrap *b)
{
	memset(b->blob_sum, 0, sizeof(b->blob_sum));
	b->exposures_in_step = 0;
	b->window_open = false;
	b->window_pending = false;
}

static bool
frame_lit(const struct t_led_phase_bootstrap *b, uint32_t camera_index, uint32_t blob_count)
{
	return blob_count >= b->baseline_blobs[camera_index] + b->options.min_blobs_per_camera;
}

static time_duration_ns
step_fudge(const struct t_led_phase_bootstrap *b, uint32_t index)
{
	return b->scan_start_ns + (time_duration_ns)index * b->scan_step_ns;
}

static time_duration_ns
current_scan_blink(const struct t_led_phase_bootstrap *b)
{
	return b->state == T_LED_PHASE_BOOTSTRAP_WIDE_SCAN ? b->options.wide_blink_ns : b->options.narrow_blink_ns;
}

static void
begin_step(struct t_led_phase_bootstrap *b)
{
	struct t_led_phase_bootstrap_step *step = &b->steps[b->step_index];
	memset(step, 0, sizeof(*step));
	step->fudge_offset_ns = step_fudge(b, b->step_index);
	step->blink_ns = current_scan_blink(b);

	reset_step_window(b);

	set_output(b, step->fudge_offset_ns, step->blink_ns);
}

static void
begin_scan(struct t_led_phase_bootstrap *b,
           enum t_led_phase_bootstrap_state state,
           time_duration_ns start_ns,
           time_duration_ns step_ns,
           uint32_t count)
{
	b->state = state;
	b->scan_start_ns = start_ns;
	b->scan_step_ns = step_ns;
	b->step_count = MIN(MAX(count, 1u), (uint32_t)T_LED_PHASE_BOOTSTRAP_MAX_STEPS);
	b->step_index = 0;

	LOG_I(b,
	      "LED_BOOTSTRAP side=%c event=scan_start stage=%s steps=%u start_us=%.1f step_us=%.1f pulse_us=%.1f "
	      "period_us=%.1f",
	      b->options.label, state_name(state), b->step_count, (double)start_ns / 1000.0, (double)step_ns / 1000.0,
	      (double)current_scan_blink(b) / 1000.0, (double)b->period_ns / 1000.0);

	begin_step(b);
}

static void
fail_scan(struct t_led_phase_bootstrap *b, const char *reason)
{
	LOG_W(b, "LED_BOOTSTRAP side=%c event=scan_failed stage=%s reason=%s", b->options.label, state_name(b->state),
	      reason);
	b->state = T_LED_PHASE_BOOTSTRAP_IDLE;
	uint32_t shift = MIN(b->consecutive_failures, 16u);
	uint64_t backoff = (uint64_t)b->options.failed_backoff_frames << shift;
	b->idle_backoff_frames = (uint32_t)MIN(backoff, (uint64_t)b->options.max_failed_backoff_frames);
	b->consecutive_failures++;
	b->output_generation++;
}

static int
compare_u32(const void *a, const void *b)
{
	uint32_t ua = *(const uint32_t *)a;
	uint32_t ub = *(const uint32_t *)b;
	return (ua > ub) - (ua < ub);
}

static int
compare_float(const void *a, const void *b)
{
	float fa = *(const float *)a;
	float fb = *(const float *)b;
	return (fa > fb) - (fa < fb);
}

static void
finish_wide_scan(struct t_led_phase_bootstrap *b)
{
	const uint32_t n = b->step_count;
	float sorted[T_LED_PHASE_BOOTSTRAP_MAX_STEPS];
	uint32_t best = 0;
	float best_smoothed = -1.0f;

	for (uint32_t i = 0; i < n; i++) {
		sorted[i] = b->steps[i].score;

		// The wide scan covers the whole period, so neighbours wrap around.
		float prev = b->steps[(i + n - 1) % n].score;
		float next = b->steps[(i + 1) % n].score;
		float smoothed = 0.25f * prev + 0.5f * b->steps[i].score + 0.25f * next;
		if (smoothed > best_smoothed) {
			best_smoothed = smoothed;
			best = i;
		}
	}

	qsort(sorted, n, sizeof(float), compare_float);
	float median = sorted[n / 2];
	float peak = b->steps[best].score;

	LOG_I(b, "LED_BOOTSTRAP side=%c event=wide_result best_step=%u fudge_us=%.1f peak=%.3f median=%.3f",
	      b->options.label, best, (double)b->steps[best].fudge_offset_ns / 1000.0, peak, median);

	if (peak < b->options.min_peak_score) {
		fail_scan(b, "wide_peak_below_minimum");
		return;
	}
	if (peak - median < b->options.min_peak_contrast) {
		fail_scan(b, "wide_peak_not_distinct");
		return;
	}

	time_duration_ns start = b->steps[best].fudge_offset_ns - b->options.narrow_margin_ns;
	time_duration_ns span = b->options.wide_blink_ns + 2 * b->options.narrow_margin_ns;
	uint32_t count = (uint32_t)(span / b->options.narrow_step_ns) + 1;

	begin_scan(b, T_LED_PHASE_BOOTSTRAP_NARROW_SCAN, start, b->options.narrow_step_ns, count);
}

static void
finish_narrow_scan(struct t_led_phase_bootstrap *b)
{
	const uint32_t n = b->step_count;
	uint32_t peak_index = 0;
	for (uint32_t i = 1; i < n; i++) {
		if (b->steps[i].score > b->steps[peak_index].score) {
			peak_index = i;
		}
	}

	float peak = b->steps[peak_index].score;
	if (peak < b->options.min_peak_score) {
		fail_scan(b, "narrow_peak_below_minimum");
		return;
	}

	// Grow the lit run around the peak while steps stay above half the peak score.
	float threshold = 0.5f * peak;
	uint32_t left = peak_index;
	uint32_t right = peak_index;
	while (left > 0 && b->steps[left - 1].score >= threshold) {
		left--;
	}
	while (right + 1 < n && b->steps[right + 1].score >= threshold) {
		right++;
	}

	if (left == 0 || right == n - 1) {
		// The lit run reaches the end of the scanned range, so an edge was not observed. Still usable, but
		// worth knowing when reading logs.
		LOG_W(b, "LED_BOOTSTRAP side=%c event=narrow_edge_unbounded left=%u right=%u steps=%u",
		      b->options.label, left, right, n);
	}

	// Steps are narrow pulse *start* offsets. Centre the locked pulse on the middle of the lit run.
	b->lit_start_ns = step_fudge(b, left);
	b->lit_end_ns = step_fudge(b, right);
	time_duration_ns centre = (b->lit_start_ns + b->lit_end_ns) / 2 + b->options.narrow_blink_ns / 2;
	time_duration_ns lock_fudge = centre - b->options.lock_blink_ns / 2;

	b->state = T_LED_PHASE_BOOTSTRAP_LOCKED;
	b->have_lock = true;
	b->locks_acquired++;
	b->consecutive_failures = 0;
	b->frames_since_lit = 0;
	b->locked_reports = 0;
	b->locked_lit_reports = 0;
	set_output(b, lock_fudge, b->options.lock_blink_ns);

	LOG_I(b,
	      "LED_BOOTSTRAP side=%c event=locked lit_start_us=%.1f lit_end_us=%.1f window_us=%.1f centre_us=%.1f "
	      "lock_fudge_us=%.1f lock_pulse_us=%.1f peak=%.3f",
	      b->options.label, (double)t_led_phase_bootstrap_wrap(b->lit_start_ns, b->period_ns) / 1000.0,
	      (double)t_led_phase_bootstrap_wrap(b->lit_end_ns, b->period_ns) / 1000.0,
	      (double)(b->lit_end_ns - b->lit_start_ns + b->options.narrow_blink_ns) / 1000.0,
	      (double)t_led_phase_bootstrap_wrap(centre, b->period_ns) / 1000.0, (double)b->fudge_offset_ns / 1000.0,
	      (double)b->options.lock_blink_ns / 1000.0, peak);
}

static void
begin_wide_scan(struct t_led_phase_bootstrap *b)
{
	uint32_t count = (uint32_t)((b->period_ns + b->options.wide_step_ns - 1) / b->options.wide_step_ns);
	begin_scan(b, T_LED_PHASE_BOOTSTRAP_WIDE_SCAN, 0, b->options.wide_step_ns, count);
}

static void
finish_baseline(struct t_led_phase_bootstrap *b)
{
	// The median ignores a stray lit frame from a controller that has not finished going dark.
	for (uint32_t c = 0; c < b->options.camera_count; c++) {
		uint32_t n = MIN(b->baseline_reported[c], (uint32_t)T_LED_PHASE_BOOTSTRAP_MAX_BASELINE_REPORTS);
		if (n == 0) {
			b->baseline_blobs[c] = 0;
			continue;
		}
		qsort(b->baseline_samples[c], n, sizeof(uint32_t), compare_u32);
		b->baseline_blobs[c] = b->baseline_samples[c][n / 2];
	}
	LOG_I(b,
	      "LED_BOOTSTRAP side=%c event=baseline blobs=%u,%u,%u,%u reported=%u,%u,%u,%u", b->options.label,
	      b->baseline_blobs[0], b->baseline_blobs[1], b->baseline_blobs[2], b->baseline_blobs[3],
	      b->baseline_reported[0], b->baseline_reported[1], b->baseline_reported[2], b->baseline_reported[3]);
	begin_wide_scan(b);
}

static void
finish_step(struct t_led_phase_bootstrap *b)
{
	if (b->state == T_LED_PHASE_BOOTSTRAP_BASELINE) {
		finish_baseline(b);
		return;
	}

	struct t_led_phase_bootstrap_step *step = &b->steps[b->step_index];

	step->score = 0.0f;
	step->mean_blobs = 0.0f;
	for (uint32_t c = 0; c < b->options.camera_count; c++) {
		if (step->reported[c] == 0) {
			continue;
		}
		step->score += (float)step->lit[c] / (float)step->reported[c];
		step->mean_blobs += (float)b->blob_sum[c] / (float)step->reported[c];
	}

	LOG_I(b,
	      "LED_BOOTSTRAP side=%c event=step stage=%s step=%u/%u fudge_us=%.1f pulse_us=%.1f score=%.3f "
	      "mean_blobs=%.2f lit=%u/%u,%u/%u,%u/%u,%u/%u",
	      b->options.label, state_name(b->state), b->step_index + 1, b->step_count,
	      (double)t_led_phase_bootstrap_wrap(step->fudge_offset_ns, b->period_ns) / 1000.0,
	      (double)step->blink_ns / 1000.0, step->score, step->mean_blobs, step->lit[0], step->reported[0],
	      step->lit[1], step->reported[1], step->lit[2], step->reported[2], step->lit[3], step->reported[3]);

	b->step_index++;
	if (b->step_index < b->step_count) {
		begin_step(b);
		return;
	}

	if (b->state == T_LED_PHASE_BOOTSTRAP_WIDE_SCAN) {
		finish_wide_scan(b);
	} else {
		finish_narrow_scan(b);
	}
}


/*
 *
 * Exported functions
 *
 */

void
t_led_phase_bootstrap_default_options(struct t_led_phase_bootstrap_options *options)
{
	*options = (struct t_led_phase_bootstrap_options){
	    .log_level = U_LOGGING_INFO,
	    .label = '?',
	    .camera_count = 4,
	    // PS Sense period id 42 (the widest pulse) lit all four cameras over a ~3 ms window in the static sweep.
	    .wide_blink_ns = 2100 * U_TIME_1US_IN_NS,
	    .wide_step_ns = 1000 * U_TIME_1US_IN_NS,
	    // Period id 9, the driver's normal pulse.
	    .narrow_blink_ns = 450 * U_TIME_1US_IN_NS,
	    .narrow_step_ns = 250 * U_TIME_1US_IN_NS,
	    .narrow_margin_ns = 1500 * U_TIME_1US_IN_NS,
	    // Period id 20, the protocol's stable minimum; more tolerant of clock wander than 450 us.
	    .lock_blink_ns = 1000 * U_TIME_1US_IN_NS,
	    .settle_frames = 8,
	    .baseline_settle_frames = 24,
	    .measure_frames = 8,
	    .grace_frames = 4,
	    .min_blobs_per_camera = 3,
	    .min_peak_score = 1.0f,
	    .min_peak_contrast = 0.75f,
	    .lost_frames = 300,
	    .failed_backoff_frames = 60,
	    .max_failed_backoff_frames = 600,
	};
}

void
t_led_phase_bootstrap_init(struct t_led_phase_bootstrap *b, const struct t_led_phase_bootstrap_options *options)
{
	memset(b, 0, sizeof(*b));
	b->options = *options;
	b->options.camera_count = MIN(b->options.camera_count, (uint32_t)T_LED_PHASE_BOOTSTRAP_MAX_CAMERAS);
	b->state = T_LED_PHASE_BOOTSTRAP_IDLE;
	b->blink_ns = options->wide_blink_ns;
}

void
t_led_phase_bootstrap_start(struct t_led_phase_bootstrap *b, time_duration_ns period_ns)
{
	if (period_ns <= 0 || b->options.wide_step_ns <= 0 || b->options.narrow_step_ns <= 0) {
		return;
	}

	b->period_ns = period_ns;
	b->scans_attempted++;
	b->idle_backoff_frames = 0;

	// Measure the background with the LEDs dark before scanning.
	b->state = T_LED_PHASE_BOOTSTRAP_BASELINE;
	memset(b->baseline_blobs, 0, sizeof(b->baseline_blobs));
	memset(b->baseline_reported, 0, sizeof(b->baseline_reported));
	memset(b->baseline_samples, 0, sizeof(b->baseline_samples));
	reset_step_window(b);
	b->output_generation++;
}

void
t_led_phase_bootstrap_stop(struct t_led_phase_bootstrap *b)
{
	if (b->state != T_LED_PHASE_BOOTSTRAP_IDLE) {
		b->state = T_LED_PHASE_BOOTSTRAP_IDLE;
		b->output_generation++;
	}
}

bool
t_led_phase_bootstrap_push_exposure(struct t_led_phase_bootstrap *b, int64_t exposure_timestamp_ns)
{
	uint32_t generation = b->output_generation;

	switch (b->state) {
	case T_LED_PHASE_BOOTSTRAP_IDLE:
		if (b->idle_backoff_frames > 0) {
			b->idle_backoff_frames--;
		}
		break;

	case T_LED_PHASE_BOOTSTRAP_LOCKED:
		b->frames_since_lit++;
		if (b->frames_since_lit > b->options.lost_frames) {
			LOG_W(b,
			      "LED_BOOTSTRAP side=%c event=lost frames_since_lit=%u locked_lit=%u/%u, rescanning",
			      b->options.label, b->frames_since_lit, b->locked_lit_reports, b->locked_reports);
			t_led_phase_bootstrap_start(b, b->period_ns);
		}
		break;

	case T_LED_PHASE_BOOTSTRAP_BASELINE:
	case T_LED_PHASE_BOOTSTRAP_WIDE_SCAN:
	case T_LED_PHASE_BOOTSTRAP_NARROW_SCAN: {
		uint32_t settle = b->state == T_LED_PHASE_BOOTSTRAP_BASELINE ? b->options.baseline_settle_frames
		                                                              : b->options.settle_frames;
		b->exposures_in_step++;
		if (b->exposures_in_step == settle + 1) {
			// Open the measurement window on the next blob report rather than on this exposure's
			// timestamp, so the timing-event clock and the frame clock never need to agree.
			b->window_pending = true;
		}
		if (b->exposures_in_step >= settle + b->options.measure_frames + b->options.grace_frames) {
			finish_step(b);
		}
		break;
	}
	}

	return generation != b->output_generation;
}

void
t_led_phase_bootstrap_push_blob_count(struct t_led_phase_bootstrap *b,
                                      uint32_t camera_index,
                                      int64_t exposure_timestamp_ns,
                                      uint32_t blob_count)
{
	if (camera_index >= b->options.camera_count) {
		return;
	}

	bool lit = frame_lit(b, camera_index, blob_count);

	if (b->state == T_LED_PHASE_BOOTSTRAP_LOCKED) {
		b->locked_reports++;
		if (lit) {
			b->locked_lit_reports++;
			b->frames_since_lit = 0;
		}
		return;
	}

	if (!t_led_phase_bootstrap_is_scanning(b)) {
		return;
	}
	if (b->window_pending) {
		// Reports lag exposures by far less than the settle time, so this frame was exposed after the new
		// setting took effect. Half a period of slack keeps the other cameras' copies of it in the window.
		b->window_start_ns = exposure_timestamp_ns - b->period_ns / 2;
		b->window_end_ns = b->window_start_ns + (int64_t)b->options.measure_frames * b->period_ns;
		b->window_open = true;
		b->window_pending = false;
	}
	if (!b->window_open) {
		return;
	}
	if (exposure_timestamp_ns < b->window_start_ns || exposure_timestamp_ns >= b->window_end_ns) {
		return;
	}

	if (b->state == T_LED_PHASE_BOOTSTRAP_BASELINE) {
		uint32_t i = b->baseline_reported[camera_index]++;
		if (i < T_LED_PHASE_BOOTSTRAP_MAX_BASELINE_REPORTS) {
			b->baseline_samples[camera_index][i] = blob_count;
		}
		return;
	}

	struct t_led_phase_bootstrap_step *step = &b->steps[b->step_index];
	step->reported[camera_index]++;
	b->blob_sum[camera_index] += blob_count;
	if (lit) {
		step->lit[camera_index]++;
	}
}

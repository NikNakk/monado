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
	case T_LED_PHASE_BOOTSTRAP_STUCK_LIT: return "stuck_lit";
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
	if (b->quick_check) {
		return b->options.lock_blink_ns;
	}
	return b->state == T_LED_PHASE_BOOTSTRAP_WIDE_SCAN ? b->options.wide_blink_ns : b->options.narrow_blink_ns;
}

static void
begin_step(struct t_led_phase_bootstrap *b)
{
	struct t_led_phase_bootstrap_step *step = &b->steps[b->step_index];
	memset(step, 0, sizeof(*step));
	step->fudge_offset_ns = step_fudge(b, b->step_index);
	step->blink_ns = current_scan_blink(b);
	if (b->quick_check) {
		// The lock pulse exactly as apply_lock would place it for a lit run at this (narrow-pulse) start.
		step->fudge_offset_ns += (b->options.narrow_blink_ns - b->options.lock_blink_ns) / 2;
	}

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
begin_wide_scan(struct t_led_phase_bootstrap *b);

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

static bool
own_lit(const struct t_led_phase_bootstrap *b, uint32_t frames, uint32_t reports)
{
	return reports > 0 && (float)frames >= b->options.stuck_own_fraction * (float)reports;
}

static void
enter_stuck_lit(struct t_led_phase_bootstrap *b, const char *reason)
{
	LOG_W(
	    b,
	    "LED_BOOTSTRAP side=%c event=stuck_lit stage=%s reason=%s: the LEDs ignore their schedule (power-cycle the "
	    "controller to clear it); scanning and probing stop, tracking continues",
	    b->options.label, state_name(b->state), reason);
	b->state = T_LED_PHASE_BOOTSTRAP_STUCK_LIT;
	b->track_stage = T_LED_PHASE_BOOTSTRAP_TRACK_NONE;
	b->track_wants_probe = false;
	b->stuck_detections++;
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

	if (b->options.detect_stuck_lit) {
		uint32_t own_lit_steps = 0;
		for (uint32_t i = 0; i < n; i++) {
			own_lit_steps += own_lit(b, b->steps[i].own_frames, b->steps[i].own_reports) ? 1 : 0;
		}
		if ((float)own_lit_steps >= b->options.stuck_wide_step_fraction * (float)n) {
			enter_stuck_lit(b, "own_ring_lit_at_every_phase");
			return;
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
apply_lock(struct t_led_phase_bootstrap *b, uint32_t left, uint32_t right, uint32_t peak_index);

//! Hold the LEDs dark for one baseline, as at the start of a scan, without starting a new scan.
static void
begin_dark_check(struct t_led_phase_bootstrap *b)
{
	b->state = T_LED_PHASE_BOOTSTRAP_BASELINE;
	b->baseline_own_reports = 0;
	b->baseline_own_frames = 0;
	memset(b->baseline_blobs, 0, sizeof(b->baseline_blobs));
	memset(b->baseline_reported, 0, sizeof(b->baseline_reported));
	memset(b->baseline_samples, 0, sizeof(b->baseline_samples));
	reset_step_window(b);
	b->output_generation++;
}

static void
begin_hinted_scan(struct t_led_phase_bootstrap *b);

static void
finish_quick_check(struct t_led_phase_bootstrap *b)
{
	b->quick_check = false;
	const struct t_led_phase_bootstrap_step *step = &b->steps[0];
	if (step->score >= b->options.quick_lock_min_score) {
		LOG_I(b, "LED_BOOTSTRAP side=%c event=quick_lock score=%.3f min=%.3f", b->options.label,
		      (double)step->score, (double)b->options.quick_lock_min_score);
		b->quick_locks++;
		apply_lock(b, 0, 0, 0);
		return;
	}
	LOG_I(b, "LED_BOOTSTRAP side=%c event=quick_lock_failed score=%.3f min=%.3f, scanning", b->options.label,
	      (double)step->score, (double)b->options.quick_lock_min_score);
	begin_hinted_scan(b);
}

static void
finish_narrow_scan(struct t_led_phase_bootstrap *b)
{
	if (b->quick_check) {
		finish_quick_check(b);
		return;
	}
	const uint32_t n = b->step_count;
	uint32_t peak_index = 0;
	for (uint32_t i = 1; i < n; i++) {
		if (b->steps[i].score > b->steps[peak_index].score) {
			peak_index = i;
		}
	}

	if (b->options.detect_stuck_lit && n >= 6) {
		uint32_t own_lit_steps = 0;
		for (uint32_t i = 0; i < n; i++) {
			own_lit_steps += own_lit(b, b->steps[i].own_frames, b->steps[i].own_reports) ? 1 : 0;
		}
		// A healthy lit window covers ~7 narrow steps; a ring lit across the whole 3-5 ms range is stuck.
		if (own_lit_steps + 1 >= n) {
			enter_stuck_lit(b, "own_ring_lit_across_narrow_scan");
			return;
		}
	}

	float peak = b->steps[peak_index].score;
	// Grow the lit run around the peak while steps stay above half the peak score, bridging up to narrow_gap_steps
	// weak steps when a lit one follows them.
	float threshold = 0.5f * peak;
	uint32_t left = peak_index;
	uint32_t right = peak_index;
	for (uint32_t i = left; i > 0 && left - (i - 1) <= b->options.narrow_gap_steps + 1; i--) {
		if (b->steps[i - 1].score >= threshold) {
			left = i - 1;
		}
	}
	for (uint32_t i = right + 1; i < n && i - right <= b->options.narrow_gap_steps + 1; i++) {
		if (b->steps[i].score >= threshold) {
			right = i;
		}
	}

	const char *weak = NULL;
	bool narrow_window = false;
	if (peak < b->options.min_peak_score) {
		weak = "narrow_peak_below_minimum";
	} else if (b->options.min_lock_peak_score > 0.0f && peak < b->options.min_lock_peak_score) {
		LOG_W(b, "LED_BOOTSTRAP side=%c event=narrow_peak_weak peak=%.3f min=%.3f", b->options.label, peak,
		      b->options.min_lock_peak_score);
		weak = "narrow_peak_weak";
	} else if (right - left + 1 < b->options.narrow_min_lit_steps) {
		/*
		 * A real lit window spans the exposure plus the pulse, 6-11 narrow steps on hardware. A run of one or
		 * two means the ring was only seen briefly (moving, turning, out of view), and centring on it locks at
		 * an unknown point of the window: on 4 Oct (A-steady, OpenBrush) the right locked on a single lit step,
		 * about 1 ms from the left's centre, and was lit in 68% of frames, then 7-29% after 113 s.
		 */
		LOG_W(b, "LED_BOOTSTRAP side=%c event=narrow_window_narrow lit_steps=%u min=%u", b->options.label,
		      right - left + 1, b->options.narrow_min_lit_steps);
		weak = "narrow_window_below_minimum";
		narrow_window = true;
	}
	if (weak != NULL) {
		// Without the full-scan fallback, retry indefinitely, except that a weak but placed run is locked
		// below.
		if (b->hinted_scan && (b->hint_failures < b->options.hint_retries ||
		                       (!b->options.full_scan_fallback && !narrow_window))) {
			/*
			 * Retry the short scan after a dark pause before trying the long one. On 26 Sep the right's
			 * failed hinted scans lit only briefly after each setting change (3/8 then 1/8 frames, the same
			 * on every camera), and twice the full scan that followed put it into the always-lit fault.
			 */
			b->hint_failures++;
			LOG_W(b,
			      "LED_BOOTSTRAP side=%c event=hint_failed reason=%s, retrying the hinted scan after a "
			      "pause (%u/%u%s)",
			      b->options.label, weak, b->hint_failures, b->options.hint_retries,
			      b->options.full_scan_fallback ? "" : ", no full-scan fallback");
			b->hinted_scan = false;
			b->state = T_LED_PHASE_BOOTSTRAP_IDLE;
			// Doubling pauses: a controller that is only out of view gets time to come back before the full
			// scan, whose long wide pulses have preceded every always-lit fault so far.
			uint64_t pause = (uint64_t)b->options.failed_backoff_frames << MIN(b->hint_failures - 1, 16u);
			b->idle_backoff_frames = (uint32_t)MIN(pause, (uint64_t)b->options.max_failed_backoff_frames);
			b->output_generation++;
			return;
		}
		if (b->hinted_scan && narrow_window) {
			/*
			 * The ring is lit where the hint says, but too briefly to measure the window's width: it is
			 * only weakly visible. Lock on the run and let tracking probes refine it rather than falling
			 * back to the full scan. On 5 Oct (083028) a right ring seen by two cameras failed five hinted
			 * scans this way, and the full scans that followed put it into the always-lit fault.
			 */
			LOG_W(
			    b,
			    "LED_BOOTSTRAP side=%c event=narrow_window_accepted lit_steps=%u after %u hinted retries, "
			    "locking instead of falling back to the full scan",
			    b->options.label, right - left + 1, b->hint_failures);
			apply_lock(b, left, right, peak_index);
			return;
		}
		if (b->hinted_scan) {
			// The window has moved from the hint (or the ring was out of view): fall back to the full scan.
			LOG_W(b, "LED_BOOTSTRAP side=%c event=hint_failed reason=%s, falling back to the full scan",
			      b->options.label, weak);
			b->hinted_scan = false;
			b->next_hint_ns = -1;
			begin_wide_scan(b);
			return;
		}
		fail_scan(b, weak);
		return;
	}

	if (left == 0 || right == n - 1) {
		// The lit run reaches the end of the scanned range, so an edge was not observed. Still usable, but
		// worth knowing when reading logs.
		LOG_W(b, "LED_BOOTSTRAP side=%c event=narrow_edge_unbounded left=%u right=%u steps=%u",
		      b->options.label, left, right, n);
	}

	const uint32_t run = right - left + 1;
	if (b->options.detect_stuck_lit && b->options.stuck_check_unbounded_steps > 0 &&
	    (left == 0 || right == n - 1) && run >= b->options.stuck_check_unbounded_steps) {
		LOG_W(b,
		      "LED_BOOTSTRAP side=%c event=stuck_check lit_steps=%u steps=%u, holding the LEDs dark before "
		      "locking",
		      b->options.label, run, n);
		b->verifying_lock = true;
		b->pending_left = left;
		b->pending_right = right;
		b->pending_peak = peak_index;
		begin_dark_check(b);
		return;
	}
	apply_lock(b, left, right, peak_index);
}

static void
apply_lock(struct t_led_phase_bootstrap *b, uint32_t left, uint32_t right, uint32_t peak_index)
{
	const float peak = b->steps[peak_index].score;
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
	b->dim_window_frames = 0;
	b->dim_window_reports = 0;
	b->dim_window_lit_reports = 0;
	set_output(b, lock_fudge, b->options.lock_blink_ns);

	// Half the span of lock-pulse starts that light the exposure, from the narrow run plus the wider pulse.
	time_duration_ns half_span = (b->lit_end_ns - b->lit_start_ns + b->options.narrow_step_ns +
	                              b->options.lock_blink_ns - b->options.narrow_blink_ns) /
	                             2;
	b->lock_fudge_ns = b->fudge_offset_ns;
	if (b->options.hint_fudge_ns >= 0) {
		// The lock pulse centre, expressed as a narrow-pulse start like the hint.
		b->next_hint_ns = centre - b->options.narrow_blink_ns / 2;
	}
	b->hinted_scan = false;
	b->hint_failures = 0;
	float background = 0.0f;
	for (uint32_t c = 0; c < b->options.camera_count; c++) {
		background += (float)b->baseline_blobs[c];
	}
	b->ring_blobs = b->steps[peak_index].mean_blobs / (float)MAX(b->options.camera_count, 1u) -
	                background / (float)MAX(b->options.camera_count, 1u);
	b->track_offset_ns = (time_duration_ns)((float)half_span * b->options.track_probe_fraction);
	if (b->options.track_max_probe_ns > 0) {
		b->track_offset_ns = MIN(b->track_offset_ns, b->options.track_max_probe_ns);
	}
	b->track_stage = T_LED_PHASE_BOOTSTRAP_TRACK_NONE;
	b->track_countdown = 0;
	b->track_wants_probe = false;

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
begin_track_stage(struct t_led_phase_bootstrap *b, enum t_led_phase_bootstrap_track_stage stage)
{
	b->track_stage = stage;
	b->track_blob_sum = 0;
	b->track_coverage_sum = 0.0f;
	b->track_reports = 0;
	reset_step_window(b);

	switch (stage) {
	case T_LED_PHASE_BOOTSTRAP_TRACK_EARLY:
		set_output(b, b->lock_fudge_ns - b->track_offset_ns, b->options.lock_blink_ns);
		break;
	case T_LED_PHASE_BOOTSTRAP_TRACK_LATE:
		set_output(b, b->lock_fudge_ns + b->track_offset_ns, b->options.lock_blink_ns);
		break;
	default: break;
	}
}

static void
finish_track(struct t_led_phase_bootstrap *b)
{
	float ref = b->track_means[0];
	float early = b->track_means[1];
	float late = b->track_means[2];

	// Normalise by the ring's size at lock time, not by the current background: another controller that lit
	// up after this one's baseline would otherwise make the ring look larger and shrink every correction.
	float ring = b->options.track_use_pose_coverage ? ref : b->ring_blobs;
	float imbalance = 0.0f;
	time_duration_ns shift = 0;
	const char *result = "centred";
	if (b->options.track_use_pose_coverage && ref < b->options.track_min_reference_coverage) {
		/*
		 * Not tracked at the lock: the ring may be dark because the lit window has slid off the lock. The blob
		 * counts still see it: on 25 Sep (234624) the left's untracked probes read e.g. 0.2/1.9/7.2 blobs
		 * (ref/early/late) as its window slid later, while coverage read 0/0/0 and the lock never moved.
		 */
		result = "reference_not_tracked";
		if (b->options.track_blob_fallback && b->ring_blobs >= b->options.track_min_ring_blobs) {
			ring = b->ring_blobs;
			imbalance = (b->track_blob_means[2] - b->track_blob_means[1]) / ring;
			imbalance = imbalance > 1.0f ? 1.0f : (imbalance < -1.0f ? -1.0f : imbalance);
			// Above the dark baseline, as ring_blobs is.
			float background = 0.0f;
			for (uint32_t c = 0; c < b->options.camera_count; c++) {
				background += (float)b->baseline_blobs[c];
			}
			background /= (float)MAX(b->options.camera_count, 1u);
			float brighter = (b->track_blob_means[1] > b->track_blob_means[2] ? b->track_blob_means[1]
			                                                                  : b->track_blob_means[2]) -
			                 background;
			result = "blob_centred";
			if (brighter < b->options.track_blob_fallback_min_fraction * ring) {
				// Too little light either side to steer by: the ring is out of view or covered, not
				// off-phase. On 26 Sep (004424) the right's fallback moved 175 us at a time
				// on 1.0/2.0/0.1 blobs of a 4.9 ring.
				result = "blob_too_dim";
				imbalance = 0.0f;
			} else if (imbalance > b->options.track_deadband || imbalance < -b->options.track_deadband) {
				shift =
				    (time_duration_ns)(b->options.track_gain * imbalance * (float)b->track_offset_ns);
				shift = CLAMP(shift, -b->options.track_max_step_ns, b->options.track_max_step_ns);
				result = "blob_moved";
			}
		}
	} else if (!b->options.track_use_pose_coverage && ring < b->options.track_min_ring_blobs) {
		result = "ring_too_small";
	} else {
		imbalance = (late - early) / ring;
		imbalance = imbalance > 1.0f ? 1.0f : (imbalance < -1.0f ? -1.0f : imbalance);
		bool confirmed = true;
		if (b->options.track_use_pose_coverage && b->options.track_coverage_min_blob_imbalance > 0.0f &&
		    b->ring_blobs > 0.0f) {
			// The side the coverage says is dimmer must also have lost blobs; otherwise its solves just
			// dropped out.
			float blob_imbalance = (b->track_blob_means[2] - b->track_blob_means[1]) / b->ring_blobs;
			confirmed = imbalance > 0.0f ? blob_imbalance >= b->options.track_coverage_min_blob_imbalance
			                             : blob_imbalance <= -b->options.track_coverage_min_blob_imbalance;
		}
		if (!confirmed && (imbalance > b->options.track_deadband || imbalance < -b->options.track_deadband)) {
			result = "unconfirmed";
		} else if (imbalance > b->options.track_deadband || imbalance < -b->options.track_deadband) {
			shift = (time_duration_ns)(b->options.track_gain * imbalance * (float)b->track_offset_ns);
			shift = CLAMP(shift, -b->options.track_max_step_ns, b->options.track_max_step_ns);
			result = "moved";
		}
	}

	b->track_cycles++;
	if (shift != 0) {
		b->track_moves++;
		b->track_total_shift_ns += shift;
		b->lock_fudge_ns = t_led_phase_bootstrap_wrap(b->lock_fudge_ns + shift, b->period_ns);
	}

	LOG_I(b,
	      "LED_BOOTSTRAP side=%c event=track result=%s ref=%.2f early=%.2f late=%.2f ring=%.2f "
	      "imbalance=%.3f blobs=%.2f/%.2f/%.2f shift_us=%.1f lock_fudge_us=%.1f offset_us=%.1f total_shift_us=%.1f",
	      b->options.label, result, ref, early, late, ring, imbalance, b->track_blob_means[0],
	      b->track_blob_means[1], b->track_blob_means[2], (double)shift / 1000.0, (double)b->lock_fudge_ns / 1000.0,
	      (double)b->track_offset_ns / 1000.0, (double)b->track_total_shift_ns / 1000.0);

	b->track_stage = T_LED_PHASE_BOOTSTRAP_TRACK_NONE;
	b->track_countdown = 0;
	b->track_wants_probe = false;
	set_output(b, b->lock_fudge_ns, b->options.lock_blink_ns);
}

static void
finish_track_stage(struct t_led_phase_bootstrap *b)
{
	uint32_t index = (uint32_t)b->track_stage - 1;
	b->track_blob_means[index] = b->track_reports ? (float)b->track_blob_sum / (float)b->track_reports : 0.0f;
	if (b->options.track_use_pose_coverage) {
		// Per exposure of the window, so exposures that did not solve count as dark.
		b->track_means[index] = b->track_coverage_sum / (float)MAX(b->options.measure_frames, 1u);
	} else {
		b->track_means[index] = b->track_reports ? (float)b->track_blob_sum / (float)b->track_reports : 0.0f;
	}

	switch (b->track_stage) {
	case T_LED_PHASE_BOOTSTRAP_TRACK_REF: begin_track_stage(b, T_LED_PHASE_BOOTSTRAP_TRACK_EARLY); break;
	case T_LED_PHASE_BOOTSTRAP_TRACK_EARLY: begin_track_stage(b, T_LED_PHASE_BOOTSTRAP_TRACK_LATE); break;
	default: finish_track(b); break;
	}
}

static bool
window_accepts(struct t_led_phase_bootstrap *b, int64_t exposure_timestamp_ns)
{
	if (b->window_pending) {
		// Reports lag exposures by far less than the settle time, so this frame was exposed after the new
		// setting took effect. Half a period of slack keeps the other cameras' copies of it in the window.
		b->window_start_ns = exposure_timestamp_ns - b->period_ns / 2;
		b->window_end_ns = b->window_start_ns + (int64_t)b->options.measure_frames * b->period_ns;
		b->window_open = true;
		b->window_pending = false;
	}
	return b->window_open && exposure_timestamp_ns >= b->window_start_ns &&
	       exposure_timestamp_ns < b->window_end_ns;
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
	LOG_I(b, "LED_BOOTSTRAP side=%c event=baseline blobs=%u,%u,%u,%u reported=%u,%u,%u,%u own=%u/%u",
	      b->options.label, b->baseline_blobs[0], b->baseline_blobs[1], b->baseline_blobs[2], b->baseline_blobs[3],
	      b->baseline_reported[0], b->baseline_reported[1], b->baseline_reported[2], b->baseline_reported[3],
	      b->baseline_own_frames, b->baseline_own_reports);
	const bool verifying = b->verifying_lock;
	b->verifying_lock = false;
	if (b->options.detect_stuck_lit && b->baseline_own_reports >= 8 &&
	    own_lit(b, b->baseline_own_frames, b->baseline_own_reports)) {
		enter_stuck_lit(b,
		                verifying ? "own_ring_lit_after_unbounded_scan" : "own_ring_lit_while_commanded_off");
		return;
	}
	if (verifying) {
		LOG_I(b, "LED_BOOTSTRAP side=%c event=stuck_check result=dark", b->options.label);
		apply_lock(b, b->pending_left, b->pending_right, b->pending_peak);
		return;
	}
	if (b->next_hint_ns >= 0 && b->options.hint_span_ns > 0) {
		if (b->options.quick_lock && !b->quick_tried) {
			b->quick_tried = true;
			b->quick_check = true;
			LOG_I(b, "LED_BOOTSTRAP side=%c event=quick_check hint_us=%.1f pulse_us=%.1f", b->options.label,
			      (double)t_led_phase_bootstrap_wrap(b->next_hint_ns, b->period_ns) / 1000.0,
			      (double)b->options.lock_blink_ns / 1000.0);
			begin_scan(b, T_LED_PHASE_BOOTSTRAP_NARROW_SCAN, b->next_hint_ns, b->options.narrow_step_ns, 1);
			return;
		}
		begin_hinted_scan(b);
		return;
	}
	begin_wide_scan(b);
}

static void
begin_hinted_scan(struct t_led_phase_bootstrap *b)
{
	b->hinted_scan = true;
	time_duration_ns start = b->next_hint_ns - b->options.hint_span_ns;
	uint32_t count = (uint32_t)(2 * b->options.hint_span_ns / b->options.narrow_step_ns) + 1;
	LOG_I(b, "LED_BOOTSTRAP side=%c event=hinted_scan hint_us=%.1f span_us=%.1f", b->options.label,
	      (double)t_led_phase_bootstrap_wrap(b->next_hint_ns, b->period_ns) / 1000.0,
	      (double)b->options.hint_span_ns / 1000.0);
	begin_scan(b, T_LED_PHASE_BOOTSTRAP_NARROW_SCAN, start, b->options.narrow_step_ns, count);
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
	    .narrow_gap_steps = 1,
	    .narrow_min_lit_steps = 3,
	    .lost_frames = 300,
	    .lost_lit_fraction = 0.0f,
	    .dim_rescan_cooldown_frames = 0,
	    .stuck_check_unbounded_steps = 0,
	    .failed_backoff_frames = 60,
	    .max_failed_backoff_frames = 600,
	    .track_interval_frames = 0,
	    .track_probe_fraction = 0.6f,
	    .track_gain = 1.5f,
	    .track_max_step_ns = 400 * U_TIME_1US_IN_NS,
	    // Blob counts of a moving ring change on their own between probe windows 0.6 s apart.
	    .track_deadband = 0.2f,
	    .track_max_probe_ns = 300 * U_TIME_1US_IN_NS,
	    .track_min_ring_blobs = 1.0f,
	    .track_use_pose_coverage = false,
	    .track_min_reference_coverage = 0.5f,
	    .track_coverage_min_blob_imbalance = 0.25f,
	    .track_blob_fallback = false,
	    .track_blob_fallback_min_fraction = 0.5f,
	    .detect_stuck_lit = false,
	    .stuck_min_matched = 2,
	    .stuck_own_fraction = 0.25f,
	    .stuck_wide_step_fraction = 0.6f,
	    .hint_fudge_ns = -1,
	    .hint_span_ns = 1500 * U_TIME_1US_IN_NS,
	    .hint_retries = 0,
	    .full_scan_fallback = true,
	    .quick_lock = false,
	    .quick_lock_min_score = 2.0f,
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
	b->next_hint_ns = options->hint_fudge_ns;
}

void
t_led_phase_bootstrap_start(struct t_led_phase_bootstrap *b, time_duration_ns period_ns)
{
	if (period_ns <= 0 || b->options.wide_step_ns <= 0 || b->options.narrow_step_ns <= 0) {
		return;
	}

	if (b->state == T_LED_PHASE_BOOTSTRAP_STUCK_LIT) {
		return;
	}
	b->period_ns = period_ns;
	b->scans_attempted++;
	b->baseline_own_reports = 0;
	b->baseline_own_frames = 0;
	b->hinted_scan = false;
	b->idle_backoff_frames = 0;
	b->verifying_lock = false;
	b->quick_check = false;
	b->quick_tried = false;

	// Measure the background with the LEDs dark before scanning.
	b->state = T_LED_PHASE_BOOTSTRAP_BASELINE;
	b->track_stage = T_LED_PHASE_BOOTSTRAP_TRACK_NONE;
	b->track_wants_probe = false;
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
	b->quick_check = false;
	b->track_stage = T_LED_PHASE_BOOTSTRAP_TRACK_NONE;
	b->track_wants_probe = false;
}

void
t_led_phase_bootstrap_begin_probe(struct t_led_phase_bootstrap *b)
{
	if (!t_led_phase_bootstrap_wants_probe(b)) {
		return;
	}
	b->track_wants_probe = false;
	begin_track_stage(b, T_LED_PHASE_BOOTSTRAP_TRACK_REF);
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

	case T_LED_PHASE_BOOTSTRAP_STUCK_LIT:
		// Nothing to do: the ring is lit regardless, and changing its settings has only ever made this happen.
		break;

	case T_LED_PHASE_BOOTSTRAP_LOCKED:
		b->frames_since_lit++;
		if (b->frames_since_lit > b->options.lost_frames) {
			LOG_W(b, "LED_BOOTSTRAP side=%c event=lost frames_since_lit=%u locked_lit=%u/%u, rescanning",
			      b->options.label, b->frames_since_lit, b->locked_lit_reports, b->locked_reports);
			t_led_phase_bootstrap_start(b, b->period_ns);
			break;
		}
		if (b->dim_rescan_cooldown > 0) {
			b->dim_rescan_cooldown--;
		}
		if (b->options.lost_lit_fraction > 0.0f && ++b->dim_window_frames >= b->options.lost_frames) {
			uint32_t reports = b->dim_window_reports;
			uint32_t lit_reports = b->dim_window_lit_reports;
			b->dim_window_frames = 0;
			b->dim_window_reports = 0;
			b->dim_window_lit_reports = 0;
			if (reports > 0 && (float)lit_reports < b->options.lost_lit_fraction * (float)reports &&
			    b->dim_rescan_cooldown == 0) {
				b->dim_rescan_cooldown = b->options.dim_rescan_cooldown_frames;
				LOG_W(
				    b,
				    "LED_BOOTSTRAP side=%c event=lost reason=dim lit_reports=%u/%u min_fraction=%.2f, "
				    "rescanning",
				    b->options.label, lit_reports, reports, (double)b->options.lost_lit_fraction);
				t_led_phase_bootstrap_start(b, b->period_ns);
				break;
			}
		}
		if (b->track_stage == T_LED_PHASE_BOOTSTRAP_TRACK_NONE) {
			if (b->options.track_interval_frames > 0 && !b->track_wants_probe &&
			    ++b->track_countdown >= b->options.track_interval_frames) {
				b->track_wants_probe = true;
			}
			break;
		} else {
			// The reference window measures the unchanged lock, so it needs no settling.
			uint32_t settle =
			    b->track_stage == T_LED_PHASE_BOOTSTRAP_TRACK_REF ? 0 : b->options.baseline_settle_frames;
			b->exposures_in_step++;
			if (b->exposures_in_step == settle + 1) {
				b->window_pending = true;
			}
			if (b->exposures_in_step >= settle + b->options.measure_frames + b->options.grace_frames) {
				finish_track_stage(b);
			}
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
t_led_phase_bootstrap_push_pose_coverage(struct t_led_phase_bootstrap *b, int64_t exposure_timestamp_ns, float coverage)
{
	if (!b->options.track_use_pose_coverage || !t_led_phase_bootstrap_is_probing(b)) {
		return;
	}
	if (window_accepts(b, exposure_timestamp_ns)) {
		b->track_coverage_sum += coverage < 0.0f ? 0.0f : (coverage > 1.0f ? 1.0f : coverage);
	}
}

void
t_led_phase_bootstrap_push_own_matched(struct t_led_phase_bootstrap *b,
                                       uint32_t camera_index,
                                       int64_t exposure_timestamp_ns,
                                       uint32_t matched)
{
	if (!b->options.detect_stuck_lit || camera_index >= b->options.camera_count ||
	    !t_led_phase_bootstrap_is_scanning(b) || !window_accepts(b, exposure_timestamp_ns)) {
		return;
	}
	bool seen = matched >= b->options.stuck_min_matched;
	if (b->state == T_LED_PHASE_BOOTSTRAP_BASELINE) {
		b->baseline_own_reports++;
		b->baseline_own_frames += seen ? 1 : 0;
		return;
	}
	struct t_led_phase_bootstrap_step *step = &b->steps[b->step_index];
	step->own_reports++;
	step->own_frames += seen ? 1 : 0;
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
		b->dim_window_reports++;
		if (lit) {
			b->locked_lit_reports++;
			b->dim_window_lit_reports++;
			b->frames_since_lit = 0;
		}
		if (t_led_phase_bootstrap_is_probing(b) && window_accepts(b, exposure_timestamp_ns)) {
			b->track_blob_sum += blob_count;
			b->track_reports++;
		}
		return;
	}

	if (!t_led_phase_bootstrap_is_scanning(b)) {
		return;
	}
	if (!window_accepts(b, exposure_timestamp_ns)) {
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

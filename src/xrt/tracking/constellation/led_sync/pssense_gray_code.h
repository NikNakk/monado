// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Routines to help decoding PS VR2 controller Gray codes + brightness curves
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup tracking
 */

#pragma once

#include "constellation/t_led_sync_refinement.h"

#include "util/u_time.h"

#include "math/m_api.h"

#include "led_sync_internal.h"

#include <float.h>


/*!
 * Whether to enable the "spew" for brightness-decoding related routines.
 *
 * @note This should never be upstreamed set to `1`!
 */
#define GRAY_SPEW 0

#if GRAY_SPEW
#define LOG_GRAY_SPEW(refinement, ...) LOG_DEBUG(refinement, __VA_ARGS__)
#else
#define LOG_GRAY_SPEW(refinement, ...)
#endif

#define TIME_BETWEEN_LED_SELECTIONS_NS (U_TIME_1MS_IN_NS * 2500LL)

//! The jump amount for an invalid code
#define INVALID_GRAY_CODE_JUMP INT64_MAX
//! This gray code isn't in the official table.
#define INVALID_GRAY_CODE (0x4)
//! How many identical gray codes do we need to decode in a row to accept it.
#define GRAY_CODE_MATCHES_NEEDED 3
//! How long to wait without an accepted gray code before resetting back to phase 1
#define GRAY_CODE_TIMEOUT_NS (U_TIME_1S_IN_NS * 10LL)

#define BG_NUM_FRAMES T_LED_SYNC_BRIGHTNESS_CURVE_NUM_SAMPLES
//! Each frame of the BG curve is shifted this much further than the last.
#define BG_STEP_NS (U_TIME_1US_IN_NS * 50)
//! The frame the dip of the BG curve should be centered on when in sync.
#define BG_TARGET_STEP 10.0f
//! Proportional gain for the BG clock-offset servo.
#define BG_OFFSET_GAIN 0.5f
//! Errors within this are left to the cycle trim, wide enough that measurement noise doesn't trigger sends.
#define BG_DEADBAND_STEPS 0.25f
//! BG measurements per skew least-squares window.
#define BG_SKEW_WINDOW_SAMPLES 20
//! Clamp on the estimated clock skew, in ns per ns.
#define BG_MAX_SKEW 1e-3
//! How long BG may go without an accepted measurement before the lock is given up.
#define BG_NO_MEASUREMENT_TIMEOUT_NS (U_TIME_1S_IN_NS * 13LL)
//! Smoothing gain on the LEDs' measured offset from the exposure, trading measurement noise against lag.
#define BG_LED_ERROR_SMOOTHING 0.2
//! How far the clock model may move before it's republished, every publish resends the LED settings.
#define BG_MODEL_PUBLISH_THRESHOLD_NS (U_TIME_1US_IN_NS * 3)
//! The fitted rate has to stay under this to enter STABLE, in ns per ns.
#define BG_STABLE_MAX_RATE 2e-6
//! How long the rate has to stay converged before STABLE, first time and coming back from it.
#define BG_STABLE_DWELL_NS (U_TIME_1S_IN_NS * 15LL)
#define BG_STABLE_DWELL_WARM_NS (U_TIME_1S_IN_NS * 3LL)
//! How long STABLE runs unmeasured before BG checks the LEDs again.
#define STABLE_RECHECK_NS (U_TIME_1S_IN_NS * 100LL)
//! How long STABLE can go without seeing the controller before starting over.
#define STABLE_NOT_SEEN_TIMEOUT_NS (U_TIME_1S_IN_NS * 15LL)

//! A table of how much time to jump for each gray code
static const time_duration_ns gray_code_jumps_ns[] = {
    U_TIME_1US_IN_NS * -500, // 000
    U_TIME_1US_IN_NS * -250, // 001
    U_TIME_1US_IN_NS * 250,  // 010
    0,                       // 011 <- Target code
    INVALID_GRAY_CODE_JUMP,  // 100 <- Invalid, never emit.
    U_TIME_1US_IN_NS * 750,  // 101
    U_TIME_1US_IN_NS * 500,  // 110
    U_TIME_1US_IN_NS * 750,  // 111
};

static void
handle_successful_gray_decode(struct t_led_sync_refinement *refinement, uint16_t gray_code)
{
	timepoint_ns jump_amount_ns = gray_code_jumps_ns[gray_code];
	assert(jump_amount_ns != INVALID_GRAY_CODE_JUMP);

	refinement->decode_gray.last_accepted_gray_code_ns = os_monotonic_get_ns();

	if (jump_amount_ns == 0) {
		LOG_DEBUG(refinement, "Got the \"done\" gray code (%d), moving to next phase.", gray_code);

		// We're done!
		set_phase_locked(refinement, T_LED_SYNC_PHASE_OPTIMIZE_BRIGHTNESS_CURVE);

		return;
	}

	LOG_DEBUG(refinement, "Accepting gray code %x after %d identical matches, moving %" PRIi64 "us", gray_code,
	          refinement->decode_gray.matching_decodes, (jump_amount_ns / 1000L));

	bool success = advance_clock_offset_pssense_locked(refinement, jump_amount_ns);
	// @todo should we have a "busy" recovery strategy here?
	(void)success;
}

static void
handle_decoded_gray_code(struct t_led_sync_refinement *refinement, uint16_t decoded_gray_code)
{
	LOG_GRAY_SPEW(refinement, "Decoded gray code! Got %x", decoded_gray_code);

	// The code is invalid if it doesn't match the current and isn't the invalid gray code
	bool invalid_code = decoded_gray_code != refinement->decode_gray.decoded_gray_code || //
	                    decoded_gray_code == INVALID_GRAY_CODE;

	if (invalid_code) {
		// Reset since the code didn't match.
		refinement->decode_gray.decoded_gray_code = decoded_gray_code;
		refinement->decode_gray.matching_decodes = 1;
	} else {
		refinement->decode_gray.matching_decodes += 1;

		if (refinement->decode_gray.matching_decodes == GRAY_CODE_MATCHES_NEEDED) {
			// Success!
			handle_successful_gray_decode(refinement, decoded_gray_code);

			// Re-arm, so a stable code keeps being accepted instead of only once.
			refinement->decode_gray.matching_decodes = 0;
		}
	}
}

static void
handle_leds_seen(struct t_led_sync_refinement *refinement, const struct t_constellation_tracker_sample *sample)
{
	// Whether the sequence ID is being incremented or not
	bool clear_slot = sample->sequence_id > refinement->leds_seen.cur_sequence_id;

	// Clear the brightnesses for all cameras in the old slot.
	if (clear_slot) {
		for (uint32_t camera_idx = 0; camera_idx < XRT_TRACKING_MAX_CAMS; camera_idx++) {
			for (uint32_t led_idx = 0; led_idx < T_LED_SYNC_SELECTED_LEDS; led_idx++) {
				size_t led_buffer_index =
				    T_LED_SYNC_GRAY_CODE_BUFFER_INDEX(camera_idx, sample->sequence_id, led_idx);

				refinement->decode_gray.led_brightnesses[led_buffer_index] = 0.0f;
				// 1.0 would be "facing the same direction as the camera", so the worst case, we
				// want facing *toward* the camera, which will be -1.
				refinement->decode_gray.led_facing_dots[led_buffer_index] = 1.0f;
			}
		}

		refinement->leds_seen.num_samples += 1;
	}

	uint32_t seen_led_idx =
	    T_LED_SYNC_SEEN_LED_BUFFER_INDEX(refinement->leds_seen.cur_sequence_id = sample->sequence_id);

	struct t_led_sync_seen_led *led_buf =
	    refinement->leds_seen.buf + (seen_led_idx * XRT_CONSTELLATION_MAX_LEDS_PER_DEVICE);

	static_assert(ARRAY_SIZE(sample->leds) == XRT_CONSTELLATION_MAX_LEDS_PER_DEVICE,
	              "LED sync depends on these being equal.");

	for (uint32_t i = 0; i < ARRAY_SIZE(sample->leds); i++) {
		if (clear_slot) {
			led_buf[i] = (struct t_led_sync_seen_led){0};
		}

		if (!sample->leds[i].observed) {
			continue;
		}

		led_buf[i].accum_seen_count += 1;
		led_buf[i].accum_facing_dot += sample->leds[i].facing_dot;
	}

	// Wait until we have enough samples to actually make a selection
	if (refinement->leds_seen.num_samples < T_LED_SYNC_SEEN_LEDS_ACCUMULATION_BUFFER_SIZE) {
		return;
	}

	bool need_new_led_selection =
	    (sample->timestamp_ns - refinement->leds_seen.last_selection_ns) > TIME_BETWEEN_LED_SELECTIONS_NS &&
	    refinement->sample_for_driver.pssense_mode != T_LED_SYNC_SAMPLE_PSSENSE_MODE_PRESCAN;

	// If we don't have a sample outgoing, then compute a new one for the new "most-seen" LEDs.
	if (!refinement->has_sample_for_driver && need_new_led_selection) {
		struct t_led_sync_seen_led leds_seen[XRT_CONSTELLATION_MAX_LEDS_PER_DEVICE] = {0};

		for (uint32_t window_idx = 0; window_idx < T_LED_SYNC_SEEN_LEDS_ACCUMULATION_BUFFER_SIZE;
		     window_idx++) {
			struct t_led_sync_seen_led *window_led_buf =
			    refinement->leds_seen.buf + (window_idx * XRT_CONSTELLATION_MAX_LEDS_PER_DEVICE);

			for (uint32_t led_idx = 0; led_idx < XRT_CONSTELLATION_MAX_LEDS_PER_DEVICE; led_idx++) {
				leds_seen[led_idx].accum_seen_count += window_led_buf[led_idx].accum_seen_count;
				leds_seen[led_idx].accum_facing_dot += window_led_buf[led_idx].accum_facing_dot;
			}
		}

		size_t most_seen_index = 0;
		float most_seen_value = -INFINITY;
		size_t second_most_seen_index = 0;
		float second_most_seen_value = -INFINITY;
		for (uint32_t i = 0; i < ARRAY_SIZE(leds_seen); i++) {
			struct t_led_sync_seen_led seen = leds_seen[i];

			// Seen more times == good, facing more towards camera == good
			float value = seen.accum_seen_count + -(seen.accum_facing_dot / seen.accum_seen_count);

			if (value > most_seen_value) {
				second_most_seen_index = most_seen_index;
				second_most_seen_value = most_seen_value;

				most_seen_index = i;
				most_seen_value = value;
			} else if (value > second_most_seen_value) {
				second_most_seen_index = i;
				second_most_seen_value = value;
			}
		}

		uint8_t leds[4] = {0xFF, 0xFF, 0xFF, 0xFF};
		if (most_seen_value > -INFINITY) {
			leds[0] = most_seen_index;
		}
		if (second_most_seen_value > -INFINITY) {
			leds[1] = second_most_seen_index;
		}

		LOG_GRAY_SPEW(refinement, "Selected LEDs %d,%d", leds[0], leds[1]);

		// Only set the new one if there isn't an existing sample ready
		if (!refinement->has_sample_for_driver) {
			set_pssense_mode(refinement, refinement->sample_for_driver.pssense_mode, leds);

			refinement->leds_seen.last_selection_ns = sample->timestamp_ns;
		}
	}
}

static void
handle_decode_gray_constellation_sample(struct t_led_sync_refinement *refinement,
                                        const struct t_constellation_tracker_sample *sample)
{
	XRT_MAYBE_UNUSED const uint16_t decoding_mask = 0x7FF; // 0b11111111111
	const uint16_t header_mask = 0x7F8;                    // 0b11111111XXX
	const uint16_t gray_code_mask = 0x7;                   // 0bXXXXXXXX111
	const uint16_t expected_header = 0x38;                 // 0b00000111XXX

	// Update the ring buffer with the new values
	for (int i = 0; i < T_LED_SYNC_SELECTED_LEDS; i++) {
		uint8_t led = refinement->sample_for_driver.pssense.leds[i];
		if (led == 0xFF) {
			continue;
		}

		size_t led_buffer_index =
		    T_LED_SYNC_GRAY_CODE_BUFFER_INDEX(sample->camera_index, refinement->leds_seen.cur_sequence_id, i);

		// Total light, for the same contrast as BG.
		refinement->decode_gray.led_brightnesses[led_buffer_index] = sample->leds[led].greysum;
		refinement->decode_gray.led_facing_dots[led_buffer_index] = sample->leds[led].facing_dot;
	}

	// We have enough info in theory
	// @todo this should be an actual "num samples" check
	if (refinement->leds_seen.cur_sequence_id > T_LED_SYNC_GRAY_CODE_SIZE_BITS) {
		uint16_t decoded_values[T_LED_SYNC_SELECTED_LEDS] = {0};

		for (int led = 0; led < T_LED_SYNC_SELECTED_LEDS; led++) {
			float min_observed_brightness[XRT_TRACKING_MAX_CAMS];
			float max_observed_brightness[XRT_TRACKING_MAX_CAMS] = {0};
			for (uint32_t i = 0; i < ARRAY_SIZE(min_observed_brightness); i++) {
				min_observed_brightness[i] = INFINITY;
			}

			float brightnesses[T_LED_SYNC_GRAY_CODE_SIZE_BITS] = {0};
			int best_camera_indices[T_LED_SYNC_GRAY_CODE_SIZE_BITS];
			for (size_t i = 0; i < ARRAY_SIZE(best_camera_indices); i++) {
				best_camera_indices[i] = -1;
			}

			// Collect the brightnesses and the thresholds
			for (int age = 0; age < T_LED_SYNC_GRAY_CODE_SIZE_BITS; age++) {
				float best_facing_dot = 1.0f;
				float best_brightness = 0.0f;

				for (int camera_index = 0; camera_index < XRT_TRACKING_MAX_CAMS; camera_index++) {
					// Pick which camera's data to use
					size_t led_buffer_index = T_LED_SYNC_GRAY_CODE_BUFFER_INDEX(
					    camera_index, (refinement->leds_seen.cur_sequence_id - age), led);

					float brightness = refinement->decode_gray.led_brightnesses[led_buffer_index];
					float facing_dot = refinement->decode_gray.led_facing_dots[led_buffer_index];

					if (brightness > 0.0f && facing_dot < 1) {
						// Update the min/max observed brightnesses, but only for valid
						// observations.
						min_observed_brightness[camera_index] =
						    MIN(min_observed_brightness[camera_index], brightness);
						max_observed_brightness[camera_index] =
						    MAX(max_observed_brightness[camera_index], brightness);
					}

					if (brightness > 0 && facing_dot < best_facing_dot) {
						best_brightness = brightness;
						best_facing_dot = facing_dot;
						best_camera_indices[age] = camera_index;
					}
				}

				brightnesses[age] = best_brightness;
			}

			// Decode the brightnesses
			for (int age = 0; age < T_LED_SYNC_GRAY_CODE_SIZE_BITS; age++) {
				if (age != 0) {
					decoded_values[led] >>= 1;
				}

				float threshold = 0.5f;
				if (best_camera_indices[age] != -1) {
					threshold = (min_observed_brightness[best_camera_indices[age]] +
					             max_observed_brightness[best_camera_indices[age]]) /
					            2.0f;
				}

				bool bit = brightnesses[age] > threshold;

				LOG_GRAY_SPEW(refinement, "brightness[%d][%d]: %f", led, age, brightnesses[age]);

				decoded_values[led] |= (bit ? 1 : 0) << (T_LED_SYNC_GRAY_CODE_SIZE_BITS - 1);
			}
		}

		if (decoded_values[0] == decoded_values[1]) {
			uint16_t decoded_value = decoded_values[0];

			// No bits outside the decoded value
			assert((decoded_value & (~decoding_mask)) == 0);

			uint16_t decoded_header = decoded_value & header_mask;
			uint16_t decoded_gray_code = decoded_value & gray_code_mask;

			if (decoded_header == expected_header) {
				handle_decoded_gray_code(refinement, decoded_gray_code);
			} else {
				LOG_GRAY_SPEW(refinement, "Decoding gray code failed! Got header %x and data %x",
				              decoded_header, decoded_gray_code);
			}
		} else {
			LOG_GRAY_SPEW(refinement, "Decoding gray code failed, the two LEDs didn't match, %d\t%d",
			              decoded_values[0], decoded_values[1]);
		}
	}
}

/*
 *
 * BG brightness curve decoding and clock sync.
 *
 */

static struct t_led_sync_frame *
bg_frame(struct t_led_sync_refinement *refinement, uint32_t camera_index, uint64_t sequence_id)
{
	return &refinement->optimize_brightness_curve.frames[camera_index][sequence_id % BG_NUM_FRAMES];
}

//! Whether @p frame was recorded for @p sequence_id and saw all the selected LEDs.
static bool
bg_frame_usable(const struct t_led_sync_frame *frame, uint64_t sequence_id)
{
	if (frame->sequence_id != sequence_id) {
		return false;
	}

	for (int led = 0; led < T_LED_SYNC_SELECTED_LEDS; led++) {
		if (frame->brightness[led] <= 0.0f) {
			return false;
		}
	}

	return true;
}

static void
bg_store_frame(struct t_led_sync_refinement *refinement, const struct t_constellation_tracker_sample *sample)
{
	const uint8_t *leds = refinement->sample_for_driver.pssense.leds;

	// Frames recorded for other LEDs can't be used, start the window over.
	if (memcmp(refinement->optimize_brightness_curve.leds, leds, T_LED_SYNC_SELECTED_LEDS) != 0) {
		memcpy(refinement->optimize_brightness_curve.leds, leds, T_LED_SYNC_SELECTED_LEDS);
		refinement->optimize_brightness_curve.window_start_sequence_id = sample->sequence_id + 1;
	}

	assert(sample->camera_index < XRT_TRACKING_MAX_CAMS);
	struct t_led_sync_frame *frame = bg_frame(refinement, sample->camera_index, sample->sequence_id);

	frame->sequence_id = sample->sequence_id;
	for (int i = 0; i < T_LED_SYNC_SELECTED_LEDS; i++) {
		bool seen = leds[i] != 0xFF && sample->leds[leds[i]].observed;

		// The total light, a partly exposed LED's saturated core mostly shrinks, which dividing by area hides.
		frame->brightness[i] = seen ? sample->leds[leds[i]].greysum : 0.0f;
		frame->facing_dot[i] = seen ? sample->leds[leds[i]].facing_dot : 1.0f;
	}
}

/*!
 * Reads the window ending at @p last_sequence_id, oldest first, from the camera facing the LEDs best. Only cameras
 * that saw both LEDs in every frame are used, a missed LED would read as a dark frame.
 */
static bool
bg_read_window(struct t_led_sync_refinement *refinement,
               uint64_t last_sequence_id,
               float out_brightness[T_LED_SYNC_SELECTED_LEDS][BG_NUM_FRAMES])
{
	if (last_sequence_id + 1 < refinement->optimize_brightness_curve.window_start_sequence_id + BG_NUM_FRAMES) {
		return false;
	}

	uint64_t first_sequence_id = last_sequence_id + 1 - BG_NUM_FRAMES;

	int best_camera = -1;
	float best_facing_dot = INFINITY;

	for (uint32_t camera = 0; camera < XRT_TRACKING_MAX_CAMS; camera++) {
		bool complete = true;
		float facing_dot = 0.0f;

		for (uint64_t seq = first_sequence_id; seq <= last_sequence_id && complete; seq++) {
			const struct t_led_sync_frame *frame = bg_frame(refinement, camera, seq);

			complete = bg_frame_usable(frame, seq);
			facing_dot += frame->facing_dot[0] + frame->facing_dot[1];
		}

		if (complete && facing_dot < best_facing_dot) {
			best_camera = (int)camera;
			best_facing_dot = facing_dot;
		}
	}

	if (best_camera < 0) {
		return false;
	}

	for (int i = 0; i < BG_NUM_FRAMES; i++) {
		const struct t_led_sync_frame *frame = bg_frame(refinement, best_camera, first_sequence_id + i);

		for (int led = 0; led < T_LED_SYNC_SELECTED_LEDS; led++) {
			out_brightness[led][i] = frame->brightness[led];
		}
	}

	return true;
}

//! Whether the BG marker (1001 at the start of the cycle, 101 at the end) is at @p cycle_start.
static bool
bg_marker_ok(const bool bits[BG_NUM_FRAMES], int cycle_start)
{
	static const int positions[] = {0, 1, 2, 3, 17, 18, 19};
	static const bool expected[] = {true, false, false, true, true, false, true};

	for (size_t i = 0; i < ARRAY_SIZE(positions); i++) {
		if (bits[(cycle_start + positions[i]) % BG_NUM_FRAMES] != expected[i]) {
			return false;
		}
	}

	return true;
}

//! Finds where the cycle starts in the window, by both LEDs showing the marker there. -1 if they don't agree.
static int
bg_find_cycle_start(float brightness[T_LED_SYNC_SELECTED_LEDS][BG_NUM_FRAMES])
{
	bool bits[T_LED_SYNC_SELECTED_LEDS][BG_NUM_FRAMES];

	for (int led = 0; led < T_LED_SYNC_SELECTED_LEDS; led++) {
		float min = brightness[led][0];
		float max = brightness[led][0];
		for (int i = 1; i < BG_NUM_FRAMES; i++) {
			min = MIN(min, brightness[led][i]);
			max = MAX(max, brightness[led][i]);
		}

		// @todo Reject windows with too little contrast to threshold reliably.
		float threshold = (min + max) / 2.0f;
		for (int i = 0; i < BG_NUM_FRAMES; i++) {
			bits[led][i] = brightness[led][i] > threshold;
		}
	}

	for (int cycle_start = 0; cycle_start < BG_NUM_FRAMES; cycle_start++) {
		bool all_ok = true;
		for (int led = 0; led < T_LED_SYNC_SELECTED_LEDS; led++) {
			all_ok = all_ok && bg_marker_ok(bits[led], cycle_start);
		}

		if (all_ok) {
			return cycle_start;
		}
	}

	return -1;
}

//! How far the dip of the curve is from @ref BG_TARGET_STEP, from the centroid of the frames below the midpoint.
static bool
bg_measure_error_steps(const float brightness[BG_NUM_FRAMES], int cycle_start, float *out_error_steps)
{
	const int first = 4;
	const int last = 16;

	float min = INFINITY;
	float max = -INFINITY;
	for (int k = first; k <= last; k++) {
		float value = brightness[(cycle_start + k) % BG_NUM_FRAMES];
		min = MIN(min, value);
		max = MAX(max, value);
	}
	float mid = (min + max) / 2.0f;

	float weighted_sum = 0.0f;
	float total_weight = 0.0f;
	for (int k = first; k <= last; k++) {
		float weight = fmaxf(mid - brightness[(cycle_start + k) % BG_NUM_FRAMES], 0.0f);
		weighted_sum += (float)k * weight;
		total_weight += weight;
	}

	// A flat curve has no dip to measure.
	if (total_weight <= 0.0f) {
		return false;
	}

	*out_error_steps = BG_TARGET_STEP - weighted_sum / total_weight;
	return true;
}

//! Corrects the clock model's drift from the LED cycle since the anchor, the LEDs follow the camera so they're right.
static void
delta_sync_reanchor(struct t_led_sync_refinement *refinement, bool reanchor_period)
{
	struct t_led_sync_delta_sync *delta = &refinement->delta_sync;

	if (refinement->last_exposure_sequence_id > delta->anchor_sequence_id) {
		// The LEDs advanced a base period per frame, the model advanced by the camera's timestamps.
		uint64_t frames = refinement->last_exposure_sequence_id - delta->anchor_sequence_id;
		double elapsed_ns = (double)(refinement->last_exposure_ns - delta->anchor_ns);
		double drift_ns = elapsed_ns - (double)frames * delta->base_period_ns;

		refinement->current_clock_offset_ns -= (time_duration_ns)drift_ns;

		if (reanchor_period) {
			delta->base_period_ns += drift_ns / (double)frames;
		}

		LOG_DEBUG(refinement, "Clock model drifted %.0fns from the LEDs over %" PRIu64 " frames", drift_ns,
		          frames);
	}

	delta->anchor_sequence_id = refinement->last_exposure_sequence_id;
	delta->anchor_ns = refinement->last_exposure_ns;
}

//! Moves to a new skew, re-basing the clock model at @p now_ns so host->device conversion stays continuous.
static void
delta_sync_set_skew(struct t_led_sync_refinement *refinement, timepoint_ns now_ns, double skew)
{
	struct t_led_sync_delta_sync *delta = &refinement->delta_sync;

	delta_sync_reanchor(refinement, false);

	refinement->current_clock_offset_ns += (time_duration_ns)(delta->skew * (double)(now_ns - delta->epoch_ns));
	delta->epoch_ns = now_ns;
	delta->skew = skew;
	delta->trim_pending = true;
}

//! Moves the base onto the latest trusted period estimate, the skew takes up the difference so the cycle is unchanged.
static void
delta_sync_rebase(struct t_led_sync_refinement *refinement, timepoint_ns now_ns)
{
	struct t_led_sync_delta_sync *delta = &refinement->delta_sync;
	double period_ns = refinement->period_estimate.period_ns;

	if (period_ns <= 0.0 || period_ns == delta->base_period_ns) {
		return;
	}

	delta_sync_set_skew(refinement, now_ns, delta->base_period_ns * (1.0 + delta->skew) / period_ns - 1.0);
	delta->base_period_ns = period_ns;

	LOG_DEBUG(refinement, "BG rebased onto %.1fns, skew %f ppm", period_ns, delta->skew * 1e6);
}

//! Fits the phase deviation against time over a window of measurements and adds the slope to the skew.
static void
bg_update_skew(struct t_led_sync_refinement *refinement, timepoint_ns now_ns, time_duration_ns error_ns)
{
	struct t_led_sync_delta_sync *delta = &refinement->delta_sync;

	if (delta->num_samples == 0) {
		delta->window_start_ns = now_ns;
	}

	// The servo corrections plus the remaining error is the drift the cycle trim didn't cover.
	double t = (double)(now_ns - delta->window_start_ns);
	double d = (double)(delta->servo_integral_ns + error_ns);

	delta->num_samples++;
	delta->sum_t += t;
	delta->sum_tt += t * t;
	delta->sum_d += d;
	delta->sum_td += t * d;

	if (delta->num_samples < BG_SKEW_WINDOW_SAMPLES) {
		return;
	}

	double n = delta->num_samples;
	double denom = n * delta->sum_tt - delta->sum_t * delta->sum_t;
	bool converged = false;
	if (denom != 0.0) {
		double slope = (n * delta->sum_td - delta->sum_t * delta->sum_d) / denom;
		delta_sync_set_skew(refinement, now_ns, CLAMP(delta->skew + slope, -BG_MAX_SKEW, BG_MAX_SKEW));
		converged = fabs(slope) < BG_STABLE_MAX_RATE;
	}

	// STABLE has nothing to correct the LEDs with, so the skew has to have stopped moving first.
	delta->num_windows++;
	uint32_t windows_needed = delta->been_stable ? 1 : 2;
	if (!converged || delta->num_windows < windows_needed) {
		delta->converged_since_ns = 0;
	} else if (delta->converged_since_ns == 0) {
		delta->converged_since_ns = now_ns;
	}

	delta->num_samples = 0;
	delta->sum_t = delta->sum_tt = delta->sum_d = delta->sum_td = 0.0;
}

//! Smooths the LEDs' measured offset from the exposure, which the published clock model takes out.
static void
bg_update_led_error(struct t_led_sync_refinement *refinement, time_duration_ns error_ns)
{
	struct t_led_sync_delta_sync *delta = &refinement->delta_sync;

	if (!delta->has_led_error) {
		delta->led_error_ns = (double)error_ns;
		delta->has_led_error = true;
		return;
	}

	delta->led_error_ns += BG_LED_ERROR_SMOOTHING * ((double)error_ns - delta->led_error_ns);
}

//! Whether the clock model has moved far enough from what the driver has to be worth republishing.
static bool
bg_model_stale(struct t_led_sync_refinement *refinement)
{
	const struct t_led_sync_sample *published = &refinement->sample_for_driver;
	if (published->timestamp_mode != T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_HOST_DEVICE_CLOCK_OFFSET) {
		return false;
	}

	time_duration_ns diff_ns = model_clock_offset_ns(refinement, refinement->current_clock_offset_ns) -
	                           published->timestamp.host_device_clock_offset_ns;

	return diff_ns > BG_MODEL_PUBLISH_THRESHOLD_NS || diff_ns < -BG_MODEL_PUBLISH_THRESHOLD_NS;
}

//! Sends a servo correction along with the current cycle trim, unless the driver hasn't taken the last sample yet.
static void
bg_send_correction(struct t_led_sync_refinement *refinement, time_duration_ns servo_ns)
{
	if (refinement->has_sample_for_driver) {
		return;
	}

	// The LEDs move toward the exposure, the published model stays where it was.
	refinement->delta_sync.servo_integral_ns += servo_ns;
	refinement->delta_sync.led_error_ns -= (double)servo_ns;

	bool sent = advance_clock_offset_pssense_locked(refinement, servo_ns);
	assert(sent);
	(void)sent;
}

static void
handle_optimize_brightness_curve_sample(struct t_led_sync_refinement *refinement,
                                        const struct t_constellation_tracker_sample *sample)
{
	bg_store_frame(refinement, sample);

	float brightness[T_LED_SYNC_SELECTED_LEDS][BG_NUM_FRAMES];
	if (!bg_read_window(refinement, sample->sequence_id, brightness)) {
		return;
	}

	for (int i = 0; i < BG_NUM_FRAMES; i++) {
		LOG_GRAY_SPEW(refinement, "BG [%d] %f %f", i, brightness[0][i], brightness[1][i]);
	}

	int cycle_start = bg_find_cycle_start(brightness);
	if (cycle_start < 0) {
		return;
	}

	// These frames are used up, the next measurement needs a fresh window.
	refinement->optimize_brightness_curve.window_start_sequence_id = sample->sequence_id + 1;

	float error_steps;
	if (!bg_measure_error_steps(brightness[0], cycle_start, &error_steps)) {
		return;
	}

	timepoint_ns now_ns = os_monotonic_get_ns();
	refinement->delta_sync.last_measurement_ns = now_ns;

	time_duration_ns error_ns = (time_duration_ns)(BG_STEP_NS * error_steps);
	bg_update_skew(refinement, now_ns, error_ns);
	bg_update_led_error(refinement, error_ns);

	LOG_GRAY_SPEW(refinement, "BG error %f steps, skew %f ppm", error_steps, refinement->delta_sync.skew * 1e6);

	if (fabsf(error_steps) > BG_DEADBAND_STEPS) {
		bg_send_correction(refinement, (time_duration_ns)(error_ns * BG_OFFSET_GAIN));
	}
}

//! Keeps the clock model on the latest period estimate and republishes it, needs no measurements.
static void
delta_sync_maintain_clock(struct t_led_sync_refinement *refinement, timepoint_ns now_ns)
{
	// IMU timestamps need the skew against host time, which only a host-measured base gives.
	delta_sync_rebase(refinement, now_ns);

	// A new skew goes out as a cycle trim with no base-time change, a moved clock model the same way.
	if (refinement->delta_sync.trim_pending || bg_model_stale(refinement)) {
		bg_send_correction(refinement, 0);
	}
}

static void
handle_decode_gray_tick(struct t_led_sync_refinement *refinement)
{
	if ((os_monotonic_get_ns() - refinement->decode_gray.last_accepted_gray_code_ns) > GRAY_CODE_TIMEOUT_NS) {
		LOG_DEBUG(refinement, "Waited %lf seconds without valid gray decode, resetting.",
		          time_ns_to_s(GRAY_CODE_TIMEOUT_NS));
		set_phase_locked(refinement, T_LED_SYNC_PHASE_FIND_INITIAL_OFFSET);
		return;
	}

	// If the final one hasn't been marked as applied, but it has been applied, send the new broad mode
	if (!refinement->decode_gray.applied_gray_start && refinement->sample_applied) {
		set_pssense_mode(refinement, T_LED_SYNC_SAMPLE_PSSENSE_MODE_BROAD, NULL);

		refinement->decode_gray.applied_gray_start = true;
	}

	// Every 10 sequence events reanchor the period
	if (refinement->last_exposure_sequence_id > refinement->delta_sync.anchor_sequence_id &&
	    refinement->last_exposure_sequence_id - refinement->delta_sync.anchor_sequence_id > 10) {
		delta_sync_reanchor(refinement, true);
		bool applied = advance_clock_offset_pssense_locked(refinement, 0);
		(void)applied; // ignore errors, it's fine
	}
}

static void
handle_optimize_brightness_curve(struct t_led_sync_refinement *refinement)
{
	// Seen but never measurable means the lock is bad, waiting longer only lets the LEDs drift off.
	timepoint_ns now_ns = os_monotonic_get_ns();
	time_duration_ns since_measurement_ns = now_ns - refinement->delta_sync.last_measurement_ns;
	if (since_measurement_ns > BG_NO_MEASUREMENT_TIMEOUT_NS) {
		LOG_DEBUG(refinement, "No BG measurement for %fms, resyncing", time_ns_to_ms_f(since_measurement_ns));
		set_phase_locked(refinement, T_LED_SYNC_PHASE_FIND_INITIAL_OFFSET);
		return;
	}

	// Converged for long enough, a pending sample has to go out before the phase can change.
	time_duration_ns dwell_ns = refinement->delta_sync.been_stable ? BG_STABLE_DWELL_WARM_NS : BG_STABLE_DWELL_NS;
	timepoint_ns converged_since_ns = refinement->delta_sync.converged_since_ns;
	if (converged_since_ns != 0 && now_ns - converged_since_ns > dwell_ns && !refinement->has_sample_for_driver) {
		set_phase_locked(refinement, T_LED_SYNC_PHASE_STABLE);
		return;
	}

	delta_sync_maintain_clock(refinement, now_ns);
}

static void
handle_stable(struct t_led_sync_refinement *refinement)
{
	timepoint_ns now_ns = os_monotonic_get_ns();

	// Nothing corrects the LEDs here, so BG checks on them every so often, once they're in view to measure.
	bool seen = refinement->frames_since_last_visually_seen <= MAX_SAMPLE_LATENCY;
	if (now_ns - refinement->delta_sync.stable_entry_ns > STABLE_RECHECK_NS && seen &&
	    !refinement->has_sample_for_driver) {
		set_phase_locked(refinement, T_LED_SYNC_PHASE_OPTIMIZE_BRIGHTNESS_CURVE);
		return;
	}

	delta_sync_maintain_clock(refinement, now_ns);
}

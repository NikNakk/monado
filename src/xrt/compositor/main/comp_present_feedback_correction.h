// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Corrects presentation feedback for the adaptive display-timing pacer.
 *
 * On macOS, CoreAnimation shows a frame one refresh after the time requested
 * through VK_GOOGLE_display_timing, even when it was ready in time, and
 * MoltenVK estimates presentMargin as actual minus begin-presentation time.
 * Fed unchanged, the adaptive pacer sees every frame as missed. This removes
 * the constant presentation delay, measured as a low percentile of recent
 * actual minus desired presentation times, and derives the margin from
 * compositor GPU completion instead.
 *
 * @ingroup comp_main
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COMP_PRESENT_FEEDBACK_WINDOW 64
#define COMP_PRESENT_FEEDBACK_MIN_SAMPLES 8

struct comp_present_feedback_correction
{
	//! Display period, bounds the delay estimate.
	int64_t period_ns;

	//! How long after the desired present time the GPU may finish and still make it.
	int64_t latch_ns;

	//! Margin reported when a frame's GPU completion is unknown, so the pacer holds.
	int64_t neutral_margin_ns;

	//! Recent actual minus desired presentation times.
	int64_t samples[COMP_PRESENT_FEEDBACK_WINDOW];
	uint32_t next, count;

	//! Current estimate of the constant presentation delay.
	int64_t delay_ns;
};

static inline void
comp_present_feedback_correction_reset(struct comp_present_feedback_correction *c, int64_t period_ns)
{
	c->period_ns = period_ns;
	c->next = 0;
	c->count = 0;
	c->delay_ns = 0;
}

//! Record a presented frame and update the delay estimate.
static inline void
comp_present_feedback_correction_observe(struct comp_present_feedback_correction *c,
                                         int64_t desired_ns,
                                         int64_t actual_ns)
{
	if (desired_ns <= 0 || actual_ns <= 0 || c->period_ns <= 0) {
		return;
	}
	c->samples[c->next] = actual_ns - desired_ns;
	c->next = (c->next + 1) % COMP_PRESENT_FEEDBACK_WINDOW;
	if (c->count < COMP_PRESENT_FEEDBACK_WINDOW) {
		c->count++;
	}
	if (c->count < COMP_PRESENT_FEEDBACK_MIN_SAMPLES) {
		return;
	}

	/*
	 * Frames that genuinely missed are a whole refresh later, and while the
	 * pacer is still settling an occasional frame lands early. The 25th
	 * percentile ignores both: a single early frame cannot set the delay, and
	 * up to three quarters of the window may be misses.
	 */
	int64_t sorted[COMP_PRESENT_FEEDBACK_WINDOW];
	for (uint32_t i = 0; i < c->count; i++) {
		int64_t value = c->samples[i];
		uint32_t j = i;
		while (j > 0 && sorted[j - 1] > value) {
			sorted[j] = sorted[j - 1];
			j--;
		}
		sorted[j] = value;
	}
	int64_t delay_ns = sorted[c->count / 4];
	if (delay_ns < 0) {
		delay_ns = 0;
	}
	if (delay_ns > 2 * c->period_ns) {
		delay_ns = 2 * c->period_ns;
	}
	c->delay_ns = delay_ns;
}

/*!
 * Correct one feedback entry in place. Zero timestamps mean "unknown" in
 * VK_GOOGLE_display_timing and are left alone.
 */
static inline void
comp_present_feedback_correction_apply(const struct comp_present_feedback_correction *c,
                                       int64_t desired_ns,
                                       bool has_gpu_end,
                                       int64_t gpu_end_ns,
                                       uint64_t *actual_ns,
                                       uint64_t *earliest_ns,
                                       uint64_t *margin_ns)
{
	if (*actual_ns > (uint64_t)c->delay_ns) {
		*actual_ns -= (uint64_t)c->delay_ns;
	}
	if (*earliest_ns > (uint64_t)c->delay_ns) {
		*earliest_ns -= (uint64_t)c->delay_ns;
	}
	if (!has_gpu_end || desired_ns <= 0) {
		*margin_ns = (uint64_t)c->neutral_margin_ns;
		return;
	}
	int64_t margin = desired_ns + c->latch_ns - gpu_end_ns;
	*margin_ns = margin > 0 ? (uint64_t)margin : 0;
}

#ifdef __cplusplus
}
#endif

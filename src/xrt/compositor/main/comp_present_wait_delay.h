// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Estimates the present-to-display delay from present-wait results.
 *
 * Without presentation timestamps, a non-blocking present-wait still says
 * whether each recent frame has been shown yet. A shown frame bounds the
 * delay from above (desired + delay <= now), an unshown one from below
 * (desired + delay > now). Frames reach the display on refresh boundaries, a
 * whole number of refreshes after their vblank-aligned desired time, so when
 * exactly one whole-refresh delay fits between the bounds, that is the delay.
 *
 * @ingroup comp_main
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @param now_ns      When the frames were checked.
 * @param period_ns   Display refresh period.
 * @param desired_ns  Desired present times of recent frames.
 * @param shown       Whether each frame had been shown at @p now_ns.
 * @param count       Number of frames.
 * @return The delay in nanoseconds (a whole number of periods), or -1 when
 *         the frames do not determine a single delay or contradict each other.
 */
static inline int64_t
comp_present_wait_delay_estimate(
    int64_t now_ns, int64_t period_ns, const int64_t *desired_ns, const bool *shown, uint32_t count)
{
	if (period_ns <= 0) {
		return -1;
	}
	bool have_lower = false;
	bool have_upper = false;
	int64_t lower = 0;
	int64_t upper = 0;
	for (uint32_t i = 0; i < count; i++) {
		int64_t since = now_ns - desired_ns[i];
		if (shown[i]) {
			upper = !have_upper || since < upper ? since : upper;
			have_upper = true;
		} else {
			lower = !have_lower || since > lower ? since : lower;
			have_lower = true;
		}
	}
	if (!have_lower || !have_upper) {
		return -1;
	}
	// Presentation times jitter slightly around the refresh boundary.
	const int64_t slack_ns = 1000000;
	int64_t first = lower - slack_ns < 0 ? 0 : (lower - slack_ns) / period_ns + 1;
	int64_t last = upper + slack_ns < 0 ? -1 : (upper + slack_ns) / period_ns;
	if (first != last) {
		return -1;
	}
	return first * period_ns;
}

#ifdef __cplusplus
}
#endif

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for estimating the present-to-display delay from present-wait.
 */

#include "../src/xrt/compositor/main/comp_present_wait_delay.h"

#include "catch_amalgamated.hpp"

namespace {

constexpr int64_t kPeriod = 11122208; // PS VR2 at 90 Hz.
constexpr int64_t kCall = 2800000;    // Present call before the frame's desired time.

/*
 * Frames N-1 .. N-4 checked right after presenting frame N. Each reaches the
 * display @p refreshes after its desired time, plus @p jitter.
 */
int64_t
estimate(int refreshes, int64_t jitter = 0)
{
	const int64_t desired_n = 1000000000000;
	const int64_t now = desired_n - kCall;
	int64_t desired[4];
	bool shown[4];
	for (int k = 1; k <= 4; k++) {
		desired[k - 1] = desired_n - k * kPeriod;
		shown[k - 1] = desired[k - 1] + refreshes * kPeriod + jitter <= now;
	}
	return comp_present_wait_delay_estimate(now, kPeriod, desired, shown, 4);
}

} // namespace

TEST_CASE("present_wait_delay_finds_two_and_three_refreshes")
{
	CHECK(estimate(2) == 2 * kPeriod);
	CHECK(estimate(3) == 3 * kPeriod);
	CHECK(estimate(1) == 1 * kPeriod);
}

TEST_CASE("present_wait_delay_tolerates_small_jitter")
{
	CHECK(estimate(2, 400000) == 2 * kPeriod);
	CHECK(estimate(3, -400000) == 3 * kPeriod);
}

TEST_CASE("present_wait_delay_needs_an_unshown_frame")
{
	// Every checked frame shown: nothing bounds the delay from below.
	int64_t desired[2] = {100, 200};
	bool shown[2] = {true, true};
	CHECK(comp_present_wait_delay_estimate(1000, kPeriod, desired, shown, 2) == -1);
}

TEST_CASE("present_wait_delay_rejects_contradictions")
{
	// An older frame unshown while a newer one is shown cannot be one fixed delay.
	const int64_t now = 1000 * kPeriod;
	int64_t desired[2] = {now - 1 * kPeriod, now - 5 * kPeriod};
	bool shown[2] = {true, false};
	CHECK(comp_present_wait_delay_estimate(now, kPeriod, desired, shown, 2) == -1);
}

TEST_CASE("present_wait_delay_handles_skipped_frames")
{
	const int64_t desired_n = 1000000000000;
	const int64_t now = desired_n - kCall;
	// The compositor skipped slot N-2: one and two refreshes both fit, so no answer.
	int64_t gap[3] = {desired_n - kPeriod, desired_n - 3 * kPeriod, desired_n - 4 * kPeriod};
	bool gap_shown[3] = {false, true, true};
	CHECK(comp_present_wait_delay_estimate(now, kPeriod, gap, gap_shown, 3) == -1);
	// Skipping N-1 instead still pins two refreshes.
	int64_t skip[3] = {desired_n - 2 * kPeriod, desired_n - 3 * kPeriod, desired_n - 4 * kPeriod};
	bool skip_shown[3] = {false, true, true};
	CHECK(comp_present_wait_delay_estimate(now, kPeriod, skip, skip_shown, 3) == 2 * kPeriod);
}

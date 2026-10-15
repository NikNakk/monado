// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for correcting presentation feedback before adaptive pacing.
 */

#include "../src/xrt/compositor/main/comp_present_feedback_correction.h"
#include "util/u_pacing.h"

#include "catch_amalgamated.hpp"

#include <deque>

namespace {

constexpr int64_t kPeriod = 11111111;

comp_present_feedback_correction
make_correction(int64_t latch_ns = 0)
{
	comp_present_feedback_correction c = {};
	c.latch_ns = latch_ns;
	c.neutral_margin_ns = 1000000;
	comp_present_feedback_correction_reset(&c, kPeriod);
	return c;
}

} // namespace

TEST_CASE("The presentation delay is the recent minimum lateness", "[compositor][pacing]")
{
	auto c = make_correction();
	for (int i = 0; i < COMP_PRESENT_FEEDBACK_MIN_SAMPLES - 1; i++) {
		comp_present_feedback_correction_observe(&c, 1000000000 + i * kPeriod,
		                                         1000000000 + i * kPeriod + kPeriod);
	}
	CHECK(c.delay_ns == 0); // Not enough samples yet.
	comp_present_feedback_correction_observe(&c, 2000000000, 2000000000 + kPeriod + 40000);
	CHECK(c.delay_ns == kPeriod);

	// A genuine miss is a refresh later still and does not move the estimate.
	comp_present_feedback_correction_observe(&c, 2100000000, 2100000000 + 2 * kPeriod);
	CHECK(c.delay_ns == kPeriod);

	// Frames that are on time pull it back down.
	for (int i = 0; i < COMP_PRESENT_FEEDBACK_WINDOW; i++) {
		comp_present_feedback_correction_observe(&c, 3000000000 + i * kPeriod,
		                                         3000000000 + i * kPeriod + 20000);
	}
	CHECK(c.delay_ns == 20000);

	// Early or absent timestamps are bounded or ignored.
	comp_present_feedback_correction_reset(&c, kPeriod);
	for (int i = 0; i < COMP_PRESENT_FEEDBACK_MIN_SAMPLES; i++) {
		comp_present_feedback_correction_observe(&c, 1000000000, 999000000);
		comp_present_feedback_correction_observe(&c, 0, 999000000);
	}
	CHECK(c.delay_ns == 0);
	for (int i = 0; i < COMP_PRESENT_FEEDBACK_WINDOW; i++) {
		comp_present_feedback_correction_observe(&c, 1000000000, 1000000000 + 5 * kPeriod);
	}
	CHECK(c.delay_ns == 2 * kPeriod);
}

TEST_CASE("Corrected feedback removes the delay and measures margin from GPU completion", "[compositor][pacing]")
{
	auto c = make_correction(500000);
	c.delay_ns = kPeriod;
	const int64_t desired = 5000000000;
	uint64_t actual = desired + kPeriod, earliest = desired + kPeriod, margin = 14000000;
	comp_present_feedback_correction_apply(&c, desired, true, desired - 2000000, &actual, &earliest, &margin);
	CHECK(actual == (uint64_t)desired);
	CHECK(earliest == (uint64_t)desired);
	CHECK(margin == 2500000);

	// GPU finished after the latch allowance: no margin.
	actual = desired + kPeriod;
	comp_present_feedback_correction_apply(&c, desired, true, desired + 1000000, &actual, &earliest, &margin);
	CHECK(margin == 0);

	// Unknown GPU completion holds the pacer, unknown timestamps stay unknown.
	actual = 0;
	earliest = 0;
	comp_present_feedback_correction_apply(&c, desired, false, 0, &actual, &earliest, &margin);
	CHECK(actual == 0);
	CHECK(earliest == 0);
	CHECK(margin == 1000000);
}

namespace {

struct SimResult
{
	int frames = 0;
	int late = 0;
	double presents_per_refresh = 0;
};

/*
 * Drive the real display-timing pacer against a display with a fixed vblank
 * grid. A frame latches on the first vblank at or after its requested time,
 * after its GPU work allows and after the previous frame, then CoreAnimation
 * shows it one refresh later. Feedback arrives two frames later.
 */
SimResult
simulate(bool corrected)
{
	struct u_pc_display_timing_config config = u_pc_display_timing_get_default_config();
	config.margin_ns = 1000000;
	config.comp_time_fraction = 10;
	config.comp_time_max_fraction = 30;
	struct u_pacing_compositor *upc = nullptr;
	REQUIRE(u_pc_display_timing_create(kPeriod, &config, &upc) == XRT_SUCCESS);
	auto c = make_correction();
	c.neutral_margin_ns = config.margin_ns;

	const int64_t vblank_base = 1003000000;
	const int64_t gpu_time = 2500000;
	const int64_t latch_after_vblank = 2000000; // GPU may finish this late and still make that vblank.
	auto next_vblank = [&](int64_t t) {
		int64_t n = (t - vblank_base + kPeriod - 1) / kPeriod;
		return vblank_base + n * kPeriod;
	};
	struct Pending
	{
		int64_t id, desired, actual, gpu_end;
	};
	std::deque<Pending> pending;
	int64_t now = 1000000000;
	int64_t last_latch = 0;
	int64_t first_actual = 0, last_actual = 0;
	SimResult result;
	for (int i = 0; i < 400; i++) {
		int64_t id, wake, desired, slop, display, period, minimum;
		u_pc_predict(upc, now, &id, &wake, &desired, &slop, &display, &period, &minimum);
		int64_t start = wake > now ? wake : now;
		u_pc_mark_point(upc, U_TIMING_POINT_WAKE_UP, id, start);
		u_pc_mark_point(upc, U_TIMING_POINT_BEGIN, id, start);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_BEGIN, id, start + 100000);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_END, id, start + 200000);
		int64_t gpu_end = start + 200000 + gpu_time;
		u_pc_info_gpu(upc, id, start + 200000, gpu_end, gpu_end);
		int64_t earliest = desired - 500000;
		if (gpu_end - latch_after_vblank > earliest) {
			earliest = gpu_end - latch_after_vblank;
		}
		if (last_latch + kPeriod > earliest) {
			earliest = last_latch + kPeriod;
		}
		int64_t latch = next_vblank(earliest);
		int64_t actual = latch + kPeriod;
		last_latch = latch;
		pending.push_back({id, desired, actual, gpu_end});
		if (pending.size() > 2) {
			Pending p = pending.front();
			pending.pop_front();
			uint64_t a = (uint64_t)p.actual, e = (uint64_t)p.actual;
			uint64_t m = (uint64_t)(p.actual - p.desired); // Like MoltenVK: from presentation start.
			if (corrected) {
				comp_present_feedback_correction_observe(&c, p.desired, p.actual);
				comp_present_feedback_correction_apply(&c, p.desired, true, p.gpu_end, &a, &e, &m);
			}
			u_pc_info(upc, p.id, p.desired, (int64_t)a, (int64_t)e, (int64_t)m, now);
		}
		if (i >= 100) {
			if (result.frames == 0) {
				first_actual = actual;
			}
			last_actual = actual;
			result.frames++;
			result.late += actual > desired + kPeriod + kPeriod / 2;
		}
		// The next frame starts after this one is submitted.
		now = start + 300000;
	}
	result.presents_per_refresh = result.frames / ((double)(last_actual - first_actual) / kPeriod + 1.0);
	u_pc_destroy(&upc);
	return result;
}

} // namespace

TEST_CASE("Corrected feedback lets the adaptive pacer keep display-rate cadence", "[compositor][pacing]")
{
	SimResult raw = simulate(false);
	SimResult fixed = simulate(true);
	INFO("raw: " << raw.presents_per_refresh << " presents per refresh, " << raw.late << "/" << raw.frames
	             << " late");
	INFO("corrected: " << fixed.presents_per_refresh << " presents per refresh, " << fixed.late << "/"
	                   << fixed.frames << " late");
	CHECK(fixed.presents_per_refresh > 0.99);
	CHECK(fixed.late == 0);
	// With raw feedback every frame looks missed and the pacer skips refreshes.
	CHECK(raw.presents_per_refresh < 0.9);
}

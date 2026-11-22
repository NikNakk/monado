// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "../src/xrt/compositor/main/comp_window_macos_feedback.h"
#include "catch_amalgamated.hpp"

namespace {
constexpr int64_t period = 8341708;

struct Fixture
{
	macos_present_feedback feedback{};
	macos_present_calibration calibration{};
	uint64_t frame = 0;
	int64_t desired = 1000000000;

	Fixture()
	{
		macos_present_feedback_reset(&feedback);
	}

	macos_present_sample
	sample(int64_t latency = 2 * period)
	{
		desired += period;
		return {feedback.generation,        ++frame, frame + 100, desired, desired + latency,
		        desired + latency + 100000, period};
	}

	void
	feed(unsigned count, int64_t latency = 2 * period)
	{
		for (unsigned i = 0; i < count; i++) {
			auto s = sample(latency);
			REQUIRE(macos_present_feedback_observe(&feedback, &s));
		}
	}
};
} // namespace

TEST_CASE("Native presentation calibration works without a diagnostic sink")
{
	Fixture f;
	for (unsigned i = 0; i < 8; i++) {
		auto s = f.sample();
		REQUIRE(macos_present_feedback_observe(&f.feedback, &s));
		CHECK(macos_present_calibration_consume(&f.calibration, &f.feedback, s.observed_ns, period) ==
		      (i == 7));
		CHECK_FALSE(macos_present_calibration_consume(&f.calibration, &f.feedback, s.observed_ns, period));
	}
	CHECK(f.calibration.count == 8);
	CHECK(f.calibration.offset_ns == 2 * period);
	f.feed(1, 3 * period);
	CHECK(macos_present_calibration_consume(&f.calibration, &f.feedback, f.feedback.latest.observed_ns, period));
	CHECK(f.calibration.offset_ns == (2 * period * 7 + 3 * period) / 8);
	// Split observations never substitute for the legacy predictor's EMA.
	CHECK(f.feedback.baseline_ns == 2 * period);
}

TEST_CASE("Presentation feedback rejects reordered invalid and stale callbacks coherently")
{
	Fixture f;
	auto first = f.sample();
	auto second = f.sample();
	REQUIRE(macos_present_feedback_observe(&f.feedback, &second));
	uint64_t serial = f.feedback.serial;
	CHECK_FALSE(macos_present_feedback_observe(&f.feedback, &first));
	CHECK_FALSE(macos_present_feedback_observe(&f.feedback, &second));
	CHECK(f.feedback.serial == serial);
	CHECK(f.feedback.latest.timeline == second.timeline);
	for (int fault = 0; fault < 7; fault++) {
		auto s = f.sample();
		switch (fault) {
		case 0: s.presented_ns = 0; break;
		case 1: s.observed_ns = s.presented_ns - 1; break;
		case 2: s.observed_ns = s.presented_ns + 4 * period + 1; break;
		case 3: s.period_ns = 0; break;
		case 4: s.desired_ns = 0; break;
		case 5:
			s.presented_ns = s.desired_ns + 4 * period + 1;
			s.observed_ns = s.presented_ns;
			break;
		case 6: s.period_ns = 11111111; break;
		}
		CHECK_FALSE(macos_present_feedback_observe(&f.feedback, &s));
		CHECK(f.feedback.serial == serial);
	}
	CHECK_FALSE(macos_present_calibration_consume(&f.calibration, &f.feedback, second.presented_ns + 4 * period + 1,
	                                              period));
	CHECK(f.calibration.count == 0);
}

TEST_CASE("Calibration coalesces callbacks and requalifies after a presentation gap")
{
	Fixture f;
	f.feed(10);
	CHECK_FALSE(
	    macos_present_calibration_consume(&f.calibration, &f.feedback, f.feedback.latest.observed_ns, period));
	CHECK(f.calibration.count == 1); // Ten callbacks are only one consumed sample.
	for (unsigned i = 0; i < 7; i++) {
		f.feed(1);
		macos_present_calibration_consume(&f.calibration, &f.feedback, f.feedback.latest.observed_ns, period);
	}
	REQUIRE(f.calibration.count == 8);
	f.desired += 10 * period;
	f.feed(1, 3 * period);
	CHECK_FALSE(
	    macos_present_calibration_consume(&f.calibration, &f.feedback, f.feedback.latest.observed_ns, period));
	CHECK(f.calibration.count == 1);
	CHECK(f.calibration.offset_ns == 3 * period);
}

TEST_CASE("Display generation invalidates in-flight samples and predictor calibration")
{
	Fixture f;
	for (unsigned i = 0; i < 8; i++) {
		f.feed(1);
		macos_present_calibration_consume(&f.calibration, &f.feedback, f.feedback.latest.observed_ns, period);
	}
	auto old = f.sample();
	macos_present_feedback_reset(&f.feedback);
	CHECK_FALSE(macos_present_feedback_observe(&f.feedback, &old));
	CHECK_FALSE(macos_present_calibration_consume(&f.calibration, &f.feedback, old.observed_ns, period));
	CHECK(f.calibration.count == 0);
	CHECK_FALSE(f.feedback.baseline_ready);
	for (unsigned i = 0; i < 8; i++) {
		auto s = f.sample();
		s.period_ns = 11111111;
		s.presented_ns = s.desired_ns + 2 * s.period_ns;
		s.observed_ns = s.presented_ns + 100000;
		REQUIRE(macos_present_feedback_observe(&f.feedback, &s));
		CHECK(macos_present_calibration_consume(&f.calibration, &f.feedback, s.observed_ns, s.period_ns) ==
		      (i == 7));
	}
	CHECK(f.calibration.offset_ns == 22222222);
}

TEST_CASE("Split observations retain a provisional baseline through sustained buffering")
{
	Fixture f;
	f.feed(10);
	REQUIRE(f.feedback.baseline_ready);
	REQUIRE(f.feedback.extra_ready);
	CHECK(f.feedback.extra_refreshes == 0);
	f.feed(1, 3 * period);
	CHECK(f.feedback.extra_refreshes == 0); // A single miss cannot move it.
	f.desired += period;                    // Recovery repeats an output refresh, not a physical timestamp.
	f.feed(1);
	f.feed(1200, 3 * period);
	CHECK(f.feedback.baseline_ns == 2 * period);
	CHECK(f.feedback.extra_ready);
	CHECK(f.feedback.extra_refreshes == 1);
	f.desired += period;
	f.feed(3);
	CHECK(f.feedback.extra_refreshes == 0);
	// Non-integral delay is uncertain, not rounded confidently into a queue count.
	f.feed(1, 2 * period + period / 3);
	CHECK_FALSE(f.feedback.extra_ready);
}

TEST_CASE("Buffered startup only establishes an anchor until a lower stable band is observed")
{
	Fixture f;
	f.feed(10, 3 * period);
	CHECK(f.feedback.baseline_ns == 3 * period);
	CHECK(f.feedback.extra_refreshes == 0); // No claim that startup was unbuffered.
	f.desired += period;
	f.feed(1);
	CHECK(f.feedback.baseline_ns == 3 * period); // One early frame cannot lower it.
	f.feed(1, 3 * period);
	f.desired += period;
	f.feed(8);
	CHECK(f.feedback.baseline_ns == 2 * period);
	f.feed(3);
	CHECK(f.feedback.extra_ready);
	CHECK(f.feedback.extra_refreshes == 0);
	// A long visibility/presentation gap invalidates the former anchor.
	f.desired += 10 * period;
	f.feed(1, 3 * period);
	CHECK_FALSE(f.feedback.baseline_ready);
	CHECK_FALSE(f.feedback.extra_ready);
	CHECK_FALSE(
	    macos_present_calibration_consume(&f.calibration, &f.feedback, f.feedback.latest.observed_ns, period));
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "catch_amalgamated.hpp"
#include "pssense_clock.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace {

constexpr int64_t kMs = 1000000;
constexpr int64_t kS = 1000000000;
constexpr int64_t kReportNs = 5 * kMs;

//! A repeatable uniform draw in [0, 1).
struct Lcg
{
	uint64_t state = 0x2545F4914F6CDD1Dull;
	double
	next()
	{
		state = state * 6364136223846793005ull + 1442695040888963407ull;
		return (double)(state >> 11) / (double)(1ull << 53);
	}
};

/*!
 * The previous driver mapping, kept as a reference: max-tracked offset dragged down at 50 ppm, smoothed by at most
 * 2.5 us per sample, snapping on a gap above @p snap_ns.
 */
struct Reference
{
	bool have = false;
	double envelope = 0.0, offset = 0.0;
	int64_t last = 0;
	double snap_ns;

	void
	push(int64_t local, int64_t remote)
	{
		double sample = (double)(remote - local);
		if (!have) {
			envelope = offset = sample;
			have = true;
		} else {
			envelope -= (double)(local - last) * 5.0e-5;
			envelope = std::max(envelope, sample);
			double delta = envelope - offset;
			if (!(snap_ns > 0.0 && std::fabs(delta) > snap_ns)) {
				delta = std::clamp(delta, -2500.0, 2500.0);
			}
			offset += delta;
		}
		last = local;
	}
};

/*!
 * A controller whose clock runs @p drift_ppm fast against the host, reporting every 5 ms over a link whose
 * latency is its floor plus 0-10 ms; @p floor_ns gives the floor at each host time.
 */
struct Link
{
	double drift_ppm = 20.0;
	double true_offset_ns = 3.0e9;
	std::function<double(int64_t)> floor_ns = [](int64_t) { return 4.0 * kMs; };
	Lcg rng{};

	double
	truth(int64_t local) const
	{
		return true_offset_ns + drift_ppm * 1e-6 * (double)local;
	}

	//! Remote clock of a report that arrives at @p local.
	int64_t
	remote(int64_t local)
	{
		double latency = floor_ns(local) + 10.0 * kMs * rng.next();
		return (int64_t)(truth(local) - latency) + local;
	}
};

} // namespace

TEST_CASE("Sense clock reproduces the previous mapping by default")
{
	for (double snap_ns : {0.0, 250000.0}) {
		CAPTURE(snap_ns);
		pssense_clock_options options;
		pssense_clock_default_options(&options);
		options.snap_ns = snap_ns;
		pssense_clock clock;
		pssense_clock_init(&clock, &options);
		Reference reference{.snap_ns = snap_ns};

		Link link;
		// A latency floor that steps, as the snaps in the 4-5 Oct sessions suggest.
		link.floor_ns = [](int64_t t) { return t > 30 * kS && t < 60 * kS ? 3.0 * kMs : 4.0 * kMs; };
		int snaps = 0;
		for (int64_t t = 0; t < 90 * kS; t += kReportNs) {
			int64_t remote = link.remote(t);
			reference.push(t, remote);
			pssense_clock_push(&clock, t, remote);
			snaps += clock.snapped ? 1 : 0;
			REQUIRE(clock.offset_ns == reference.offset);
		}
		CHECK(clock.holding == false);
		if (snap_ns > 0.0) {
			CHECK(snaps > 0);
		}
	}
}

TEST_CASE("Sense clock steady mode holds through latency steps at the fitted drift rate")
{
	pssense_clock_options options;
	pssense_clock_default_options(&options);
	options.snap_ns = 250000.0;
	options.steady = true;
	pssense_clock clock;
	pssense_clock_init(&clock, &options);
	Reference reference{.snap_ns = options.snap_ns};

	Link link;
	// The floor drops by 1 ms for 30 s, then rises 1 ms above its start: the default mapping snaps up, then sags.
	link.floor_ns = [](int64_t t) { return t < 40 * kS ? 4.0 * kMs : (t < 70 * kS ? 3.0 * kMs : 5.0 * kMs); };

	double held_bias = 0.0;
	double max_held_error = 0.0;
	double max_reference_error = 0.0;
	for (int64_t t = 0; t < 120 * kS; t += kReportNs) {
		// The driver asks for the hold once the LED schedule has locked.
		pssense_clock_set_hold(&clock, t >= 10 * kS);
		int64_t remote = link.remote(t);
		reference.push(t, remote);
		bool was_holding = clock.holding;
		pssense_clock_push(&clock, t, remote);
		if (clock.holding && !was_holding) {
			// The lock absorbs whatever constant bias the mapping had when it was measured.
			held_bias = clock.offset_ns - link.truth(t);
			CHECK(t >= 10 * kS);
		}
		if (clock.holding) {
			REQUIRE_FALSE(clock.snapped);
			max_held_error =
			    std::max(max_held_error, std::fabs(clock.offset_ns - link.truth(t) - held_bias));
			max_reference_error =
			    std::max(max_reference_error, std::fabs(reference.offset - link.truth(t) - held_bias));
		}
	}
	REQUIRE(clock.holding);
	CHECK(clock.have_rate);
	CHECK(std::fabs(clock.rate * 1e6 - link.drift_ppm) < 3.0);
	CAPTURE(max_held_error, max_reference_error);
	// Within the LED lock's ~350 us tolerance with a wide margin; the default mapping moves by the full steps.
	CHECK(max_held_error < 100000.0);
	CHECK(max_reference_error > 500000.0);
}

TEST_CASE("Sense clock recovers the measured offset when a drifted hold is released")
{
	pssense_clock_options options;
	pssense_clock_default_options(&options);
	options.snap_ns = 250000.0;
	options.steady = true;
	pssense_clock clock;
	pssense_clock_init(&clock, &options);

	// The controller's drift changes while the mapping is held, as it did on 7 Oct when a held mapping ended
	// about 10 ms off and the hinted LED scans, placed by it, saw nothing.
	Link link;
	double base = link.true_offset_ns;
	link.drift_ppm = 20.0;
	int64_t t = 0;
	auto truth = [&](int64_t at) {
		return at < 60 * kS ? base + 20e-6 * (double)at : base + 20e-6 * 60e9 + 200e-6 * (double)(at - 60 * kS);
	};
	auto push = [&](int64_t at) {
		double latency = 4.0 * kMs + 10.0 * kMs * link.rng.next();
		pssense_clock_push(&clock, at, (int64_t)(truth(at) - latency) + at);
	};
	for (; t < 120 * kS; t += kReportNs) {
		pssense_clock_set_hold(&clock, t >= 10 * kS);
		push(t);
	}
	REQUIRE(clock.holding);
	double held_error = std::fabs(clock.offset_ns - truth(t));
	CAPTURE(held_error);
	CHECK(held_error > 2.0 * kMs);

	// Lock lost: the hold is released, and the default mapping converges on (snaps to) the measured offset.
	pssense_clock_set_hold(&clock, false);
	for (int64_t end = t + 2 * kS; t < end; t += kReportNs) {
		push(t);
	}
	CHECK_FALSE(clock.holding);
	double released_error = std::fabs(clock.offset_ns - truth(t));
	CAPTURE(released_error);
	// Within the 4-14 ms latency band of the samples, as the default mapping always is.
	CHECK(released_error < 15.0 * kMs);
	CHECK(released_error < held_error);
}

TEST_CASE("Sense clock steady mode waits for a rate and clamps it")
{
	pssense_clock_options options;
	pssense_clock_default_options(&options);
	options.steady = true;
	options.max_rate_ppm = 50.0;
	pssense_clock clock;
	pssense_clock_init(&clock, &options);
	pssense_clock_set_hold(&clock, true);

	Link link;
	link.drift_ppm = 400.0;
	int64_t t = 0;
	for (; t < 5 * kS; t += kReportNs) {
		pssense_clock_push(&clock, t, link.remote(t));
	}
	// A rate needs 8 s of 1 s buckets (four differences 4 s apart): none yet, so no hold.
	CHECK_FALSE(clock.have_rate);
	CHECK_FALSE(clock.holding);
	for (; t < 20 * kS; t += kReportNs) {
		pssense_clock_push(&clock, t, link.remote(t));
	}
	REQUIRE(clock.holding);
	CHECK(clock.rate * 1e6 == Catch::Approx(50.0));

	// Releasing the hold resumes the default mapping.
	pssense_clock_set_hold(&clock, false);
	pssense_clock_push(&clock, t, link.remote(t));
	CHECK_FALSE(clock.holding);
}

TEST_CASE("Sense clock never holds without steady mode")
{
	pssense_clock_options options;
	pssense_clock_default_options(&options);
	pssense_clock clock;
	pssense_clock_init(&clock, &options);
	pssense_clock_set_hold(&clock, true);
	Link link;
	for (int64_t t = 0; t < 30 * kS; t += kReportNs) {
		pssense_clock_push(&clock, t, link.remote(t));
	}
	CHECK(clock.have_rate);
	CHECK_FALSE(clock.holding);
}

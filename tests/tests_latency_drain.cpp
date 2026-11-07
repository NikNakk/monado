// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for deciding when to drain a backed-up presentation queue.
 */

#include "../src/xrt/compositor/main/comp_latency_drain.h"

#include "catch_amalgamated.hpp"

namespace {

constexpr int64_t kMs = 1000000;
constexpr int64_t kPeriod = 11111111;

int
count_drains(comp_latency_drain &d, int64_t start_ns, int frames, bool backlogged)
{
	int drains = 0;
	for (int i = 0; i < frames; i++) {
		drains += comp_latency_drain_observe(&d, start_ns + i * kPeriod, backlogged) ? 1 : 0;
	}
	return drains;
}

} // namespace

TEST_CASE("latency_drain_waits_for_a_sustained_backlog")
{
	comp_latency_drain d;
	comp_latency_drain_init(&d, 500 * kMs, 1000 * kMs);
	const int64_t t0 = 1000 * kMs;

	// 46 observations span 499.99 ms: no drain yet.
	CHECK(count_drains(d, t0, 46, true) == 0);
	// The 47th is 511 ms after the first.
	CHECK(comp_latency_drain_observe(&d, t0 + 46 * kPeriod, true));
	CHECK(d.drains == 1);
}

TEST_CASE("latency_drain_resets_when_the_backlog_clears")
{
	comp_latency_drain d;
	comp_latency_drain_init(&d, 500 * kMs, 1000 * kMs);
	const int64_t t0 = 1000 * kMs;

	CHECK(count_drains(d, t0, 40, true) == 0);
	CHECK_FALSE(comp_latency_drain_observe(&d, t0 + 40 * kPeriod, false));
	// The run restarts, so another 40 backlogged frames still do not drain.
	CHECK(count_drains(d, t0 + 41 * kPeriod, 40, true) == 0);
	CHECK(d.drains == 0);
}

TEST_CASE("latency_drain_respects_the_cooldown")
{
	comp_latency_drain d;
	comp_latency_drain_init(&d, 100 * kMs, 1000 * kMs);
	const int64_t t0 = 1000 * kMs;

	// A backlog that never clears: one drain per cooldown, not per sustain period.
	int drains = count_drains(d, t0, 270, true); // 3 s
	CHECK(drains == 3);
	CHECK(d.drains == 3);
}

TEST_CASE("latency_drain_first_drain_is_not_blocked_by_cooldown")
{
	comp_latency_drain d;
	comp_latency_drain_init(&d, 0, 1000 * kMs);
	CHECK(comp_latency_drain_observe(&d, 5 * kMs, true));
}

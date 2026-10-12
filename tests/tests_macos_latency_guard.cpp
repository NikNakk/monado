// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "main/comp_window_macos_latency_guard.h"

static constexpr uint64_t period = 8333333;

static void
feed_deep(macos_latency_guard &guard, uint64_t first_frame, uint64_t first_ns, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		macos_latency_guard_observe(&guard, first_frame + i, first_ns + i * period, 2, period);
	}
}

TEST_CASE("Latency guard drains sustained depth, suppresses bursts and ignores old feedback")
{
	macos_latency_guard guard{};
	feed_deep(guard, 1, 1000000000, 120);
	REQUIRE_FALSE(macos_latency_guard_should_skip(&guard, 121, 1991000000, period));
	feed_deep(guard, 121, 2000000000, 2);
	const uint64_t drain_ns = 2009000000;
	REQUIRE(macos_latency_guard_should_skip(&guard, 124, drain_ns, period));
	REQUIRE(guard.drains == 1);
	REQUIRE(macos_latency_guard_should_skip(&guard, 125, drain_ns + 1, period));
	REQUIRE_FALSE(macos_latency_guard_should_skip(&guard, 126, drain_ns + period, period));
	macos_latency_guard_observe(&guard, 123, 2010000000, 2, period);
	REQUIRE(guard.deep_since_ns == 0);
	feed_deep(guard, 127, 2020000000, 1080);
	REQUIRE_FALSE(macos_latency_guard_should_skip(&guard, 1207, 11012000000, period));
	feed_deep(guard, 1207, 11020000000, 120);
	REQUIRE(macos_latency_guard_should_skip(&guard, 1327, 12012000000, period));
	REQUIRE(guard.drains == 2);
}

TEST_CASE("Latency guard requires valid recent sustained feedback")
{
	macos_latency_guard guard{};
	feed_deep(guard, 1, 1000000000, 120);
	macos_latency_guard_observe(&guard, 121, 2000000000, 1, period);
	macos_latency_guard_observe(&guard, 119, 1990000000, 3, period);
	REQUIRE(guard.deep_since_ns == 0);
	REQUIRE_FALSE(macos_latency_guard_should_skip(&guard, 122, 2001000000, period));
	macos_latency_guard_observe(&guard, 123, 2010000000, 2, period);
	macos_latency_guard_observe(&guard, 124, 0, 2, period);
	macos_latency_guard_observe(&guard, 125, 2020000000, -1, period);
	REQUIRE(guard.latest_frame == 123);
	REQUIRE_FALSE(macos_latency_guard_should_skip(&guard, 240, 3100000000, period));
	macos_latency_guard_observe(&guard, 241, 3100000000, 2, period);
	REQUIRE(guard.deep_since_ns == 3100000000);
	feed_deep(guard, 242, 3100000000 + period, 120);
	REQUIRE_FALSE(macos_latency_guard_should_skip(&guard, 362, 4101000000, 0));
	REQUIRE(macos_latency_guard_should_skip(&guard, 362, 4101000000, period));
}

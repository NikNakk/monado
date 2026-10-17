// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "../src/xrt/compositor/main/comp_target_swapchain_timing.h"
#include "catch_amalgamated.hpp"

TEST_CASE("Presentation clock conversion preserves immediate sentinel", "[compositor][timing]")
{
	REQUIRE(comp_target_timing_apply_offset(0, 3600000000000LL) == 0);
	REQUIRE(comp_target_timing_apply_offset(0, -3600000000000LL) == 0);
}

TEST_CASE("Presentation deadlines and feedback round trip after system sleep", "[compositor][timing]")
{
	// Metal host time and runtime monotonic time can differ by accumulated sleep.
	for (int64_t offset : {INT64_C(37000000000000), INT64_C(-37000000000000), INT64_C(0)}) {
		uint64_t desired = UINT64_C(1000000000000000);
		uint64_t actual = desired + 11122222;
		uint64_t host_desired = comp_target_timing_apply_offset(desired, -offset);
		uint64_t host_actual = comp_target_timing_apply_offset(actual, -offset);
		REQUIRE(comp_target_timing_apply_offset(host_desired, offset) == desired);
		REQUIRE(comp_target_timing_apply_offset(host_actual, offset) == actual);
		REQUIRE(host_actual - host_desired == actual - desired);
		// A present slop is a duration, applied once before conversion.
		REQUIRE(comp_target_timing_apply_offset(desired - 500000, -offset) == host_desired - 500000);
	}
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for projecting display-link output times onto vblank boundaries.
 */

#include "../src/xrt/compositor/main/comp_macos_display_timing.h"

#include "catch_amalgamated.hpp"

#include <limits>

TEST_CASE("display phase projects over missed callbacks and exact boundaries", "[macos][timing]")
{
	CHECK(comp_macos_last_vblank_ns(110, 105, 10) == 100);
	CHECK(comp_macos_last_vblank_ns(140, 105, 10) == 100);
	CHECK(comp_macos_last_vblank_ns(110, 110, 10) == 110);
	CHECK(comp_macos_last_vblank_ns(80, 105, 10) == 100);
	CHECK(comp_macos_last_vblank_ns(5, 1, 10) == 0);
	CHECK(comp_macos_last_vblank_ns(0, 105, 10) == 105);
	CHECK(comp_macos_last_vblank_ns(110, 105, 0) == 105);
	const auto max = std::numeric_limits<uint64_t>::max();
	CHECK(comp_macos_last_vblank_ns(max, max - 5, 10) == max - 10);
}

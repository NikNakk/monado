// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "foveation/u_foveation.h"

#include "catch_amalgamated.hpp"

#include <array>
#include <cstring>

TEST_CASE("generic foveation profiles are stable")
{
	const auto *aggressive = u_foveation_profile_get(U_FOVEATION_PROFILE_AGGRESSIVE);
	REQUIRE(aggressive != nullptr);
	CHECK(std::strcmp(aggressive->name, "aggressive") == 0);
	CHECK(aggressive->center_rate == Catch::Approx(1.0f));
	CHECK(aggressive->middle_rate == Catch::Approx(0.50f));
	CHECK(aggressive->peripheral_rate == Catch::Approx(0.25f));
	CHECK(aggressive->center_half_extent == Catch::Approx(3.0f / 32.0f));
	CHECK(aggressive->middle_half_extent == Catch::Approx(7.0f / 32.0f));

	CHECK(u_foveation_profile_find("aggressive") == U_FOVEATION_PROFILE_AGGRESSIVE);
	CHECK(u_foveation_profile_find("does-not-exist") == -1);
	CHECK(u_foveation_profile_get(-1) == nullptr);
	CHECK(u_foveation_profile_get(U_FOVEATION_PROFILE_COUNT) == nullptr);
}

TEST_CASE("generic foveation rate regions preserve the validated 16-zone layout")
{
	const auto *profile = u_foveation_profile_get(U_FOVEATION_PROFILE_AGGRESSIVE);
	REQUIRE(profile != nullptr);

	CHECK(u_foveation_profile_rate_for_offset(profile, 0.0f) == Catch::Approx(1.0f));
	CHECK(u_foveation_profile_rate_for_offset(profile, 1.0f / 16.0f) == Catch::Approx(1.0f));
	CHECK(u_foveation_profile_rate_for_offset(profile, 2.0f / 16.0f) == Catch::Approx(0.50f));
	CHECK(u_foveation_profile_rate_for_offset(profile, 3.0f / 16.0f) == Catch::Approx(0.50f));
	CHECK(u_foveation_profile_rate_for_offset(profile, 4.0f / 16.0f) == Catch::Approx(0.25f));

	std::array<float, 16> rates{};
	REQUIRE(u_foveation_build_axis_rates(profile, rates.size(), 8, rates.data()));

	for (size_t i = 0; i < rates.size(); ++i) {
		const size_t delta = i > 8 ? i - 8 : 8 - i;
		const float expected = delta <= 1 ? 1.0f : (delta <= 3 ? 0.50f : 0.25f);
		CHECK(rates[i] == Catch::Approx(expected));
	}
}

TEST_CASE("generic foveation axis builder validates arguments")
{
	const auto *profile = u_foveation_profile_get(U_FOVEATION_PROFILE_REFERENCE);
	std::array<float, 4> rates{};
	CHECK_FALSE(u_foveation_build_axis_rates(nullptr, rates.size(), 0, rates.data()));
	CHECK_FALSE(u_foveation_build_axis_rates(profile, 0, 0, rates.data()));
	CHECK_FALSE(u_foveation_build_axis_rates(profile, rates.size(), rates.size(), rates.data()));
	CHECK_FALSE(u_foveation_build_axis_rates(profile, rates.size(), 0, nullptr));
}

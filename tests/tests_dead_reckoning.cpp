// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Dead-reckoning FIFO boundary regression test.
 */

#include "catch_amalgamated.hpp"

#include "math/m_filter_fifo.h"
#include "tracking/t_dead_reckoning.h"

#include <cmath>

TEST_CASE("dead reckoning selects the oldest applicable IMU sample", "[tracking]")
{
	constexpr timepoint_ns base_time = U_TIME_1S_IN_NS;
	constexpr timepoint_ns step = 10 * U_TIME_1MS_IN_NS;
	FilterFifo3F gyro(3);
	gyro.push({0.0f, 0.0f, 1.0f}, base_time + step);
	gyro.push({0.0f, 0.0f, 2.0f}, base_time + 2 * step);
	gyro.push({0.0f, 0.0f, 3.0f}, base_time + 3 * step);
	xrt_space_relation base = XRT_SPACE_RELATION_ZERO;
	base.relation_flags = XRT_SPACE_RELATION_BITMASK_ALL;
	timepoint_ns timestamp = base_time;
	timepoint_ns when = base_time + 3 * step;
	double expected_angle = 0;
	float expected_velocity = 3;
	SECTION("base precedes every sample")
	{
		// Integrate 1, 2 and 3 rad/s for 10 ms each.
		expected_angle = 0.01 + 0.02 + 0.03;
	}
	SECTION("base equals the oldest sample")
	{
		timestamp += step;
		expected_angle = 0.02 + 0.03;
	}
	SECTION("base lies between retained samples")
	{
		timestamp += 15 * U_TIME_1MS_IN_NS;
		expected_angle = 0.01 + 0.03;
	}
	SECTION("all samples precede the base")
	{
		timestamp += 35 * U_TIME_1MS_IN_NS;
		base.angular_velocity.z = 0.5f;
		expected_velocity = 0.5f;
		when = base_time + 4 * step;
		expected_angle = 0.005 * 0.5;
	}
	xrt_space_relation result = XRT_SPACE_RELATION_ZERO;
	REQUIRE(t_apply_dead_reckoning(gyro.unsafeGetFilterFifo(), nullptr, nullptr, when, &base, timestamp, &result));
	CHECK(result.pose.orientation.x == Catch::Approx(0.0f).margin(1e-6));
	CHECK(result.pose.orientation.y == Catch::Approx(0.0f).margin(1e-6));
	CHECK(result.pose.orientation.z == Catch::Approx(std::sin(expected_angle / 2)).margin(1e-6));
	CHECK(result.pose.orientation.w == Catch::Approx(std::cos(expected_angle / 2)).margin(1e-6));
	CHECK(result.angular_velocity.z == Catch::Approx(expected_velocity).margin(1e-6));
	CHECK(result.relation_flags == base.relation_flags);
}

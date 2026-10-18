// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Directions and limits of the desktop-steered simulated head pose.
 */

#include "util/u_desktop_pose.h"

#include "catch_amalgamated.hpp"

#include <cmath>

using Catch::Approx;

namespace {

constexpr float pi = 3.14159265358979f;

//! Rotate v by unit quaternion q.
xrt_vec3
rotate(const xrt_quat &q, const xrt_vec3 &v)
{
	// t = 2 * cross(q.xyz, v); v' = v + w * t + cross(q.xyz, t)
	xrt_vec3 t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
	return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
	        v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}

xrt_vec3
forward()
{
	xrt_pose pose{};
	u_desktop_pose_get(&pose);
	return rotate(pose.orientation, {0, 0, -1});
}

} // namespace

TEST_CASE("Desktop pose starts at identity and resets", "[desktop_pose]")
{
	u_desktop_pose_reset();
	xrt_pose pose{};
	u_desktop_pose_get(&pose);
	CHECK(pose.orientation.w == Approx(1));
	CHECK(pose.position.x == Approx(0));
	CHECK(pose.position.z == Approx(0));

	u_desktop_pose_rotate(0.5f, 0.3f);
	u_desktop_pose_move(1, 2);
	u_desktop_pose_reset();
	u_desktop_pose_get(&pose);
	CHECK(pose.orientation.w == Approx(1));
	CHECK(pose.position.x == Approx(0).margin(1e-6));
	CHECK(pose.position.z == Approx(0).margin(1e-6));
}

TEST_CASE("Desktop pose turns and looks in the expected directions", "[desktop_pose]")
{
	u_desktop_pose_reset();
	// Positive yaw turns left: forward (-Z) swings towards -X.
	u_desktop_pose_rotate(pi / 2, 0);
	xrt_vec3 f = forward();
	CHECK(f.x == Approx(-1).margin(1e-5));
	CHECK(f.z == Approx(0).margin(1e-5));

	// Positive pitch looks up, and stays level-relative after the turn.
	u_desktop_pose_rotate(0, pi / 4);
	f = forward();
	CHECK(f.y == Approx(std::sqrt(0.5f)).margin(1e-5));
	CHECK(f.x == Approx(-std::sqrt(0.5f)).margin(1e-5));
	CHECK(f.z == Approx(0).margin(1e-5));
}

TEST_CASE("Desktop pose pitch stops short of vertical", "[desktop_pose]")
{
	u_desktop_pose_reset();
	u_desktop_pose_rotate(0, 10);
	CHECK(forward().y == Approx(std::sin(89 * pi / 180)).margin(1e-4));
	u_desktop_pose_rotate(0, -20);
	CHECK(forward().y == Approx(-std::sin(89 * pi / 180)).margin(1e-4));
}

TEST_CASE("Desktop pose moves on the floor relative to the heading", "[desktop_pose]")
{
	u_desktop_pose_reset();
	xrt_pose pose{};

	// Facing -Z: forward is -Z, right is +X.
	u_desktop_pose_move(0, 1);
	u_desktop_pose_get(&pose);
	CHECK(pose.position.z == Approx(-1));
	u_desktop_pose_move(1, 0);
	u_desktop_pose_get(&pose);
	CHECK(pose.position.x == Approx(1));

	// After turning left by 90 degrees, forward is -X; looking up does not lift.
	u_desktop_pose_reset();
	u_desktop_pose_rotate(pi / 2, 0.5f);
	u_desktop_pose_move(0, 1);
	u_desktop_pose_get(&pose);
	CHECK(pose.position.x == Approx(-1).margin(1e-5));
	CHECK(pose.position.y == Approx(0));
	CHECK(pose.position.z == Approx(0).margin(1e-5));
	u_desktop_pose_reset();
}

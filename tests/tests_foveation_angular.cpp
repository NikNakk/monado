// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "foveation/u_foveation_angular.h"
#include "catch_amalgamated.hpp"
#include <cmath>
#include <cstring>
TEST_CASE("Angular gaze preserves quantization, hysteresis, hold and fallback", "[foveation][angular]")
{
	u_foveation_angular_config c = {.3f, 12.f, 27.f, 2.f, true};
	u_foveation_angular_history h = {};
	REQUIRE(u_foveation_angular_config_valid(&c));
	u_foveation_angular_update(&c, true, 10.1f, -4.1f, &h);
	REQUIRE(h.have_gaze);
	REQUIRE(h.yaw == 10);
	REQUIRE(h.pitch == -4);
	u_foveation_angular_update(&c, true, 11.f, -4.f, &h);
	REQUIRE(h.yaw == 10);
	u_foveation_angular_update(&c, true, 11.3f, -4.f, &h);
	REQUIRE(h.yaw == 12);
	for (unsigned i = 0; i < 30; ++i) {
		u_foveation_angular_update(&c, false, 0, 0, &h);
		REQUIRE(h.have_gaze);
	}
	u_foveation_angular_update(&c, false, 0, 0, &h);
	REQUIRE_FALSE(h.have_gaze);
	u_foveation_angular_update(&c, true, 60, -60, &h);
	REQUIRE(h.yaw == 45);
	REQUIRE(h.pitch == -45);
	u_foveation_angular_update(&c, true, NAN, NAN, &h);
	REQUIRE(h.invalid_calls == 1);
	c.eye_tracked = false;
	h = {};
	u_foveation_angular_update(&c, true, 10, 10, &h);
	REQUIRE_FALSE(h.have_gaze);
}
TEST_CASE("Angular targets reject invalid geometry and FOVs", "[foveation][angular]")
{
	u_foveation_angular_target t = {
	    1024, 512, {{0, 0, 512, 512, -.9f, .8f, .85f, -.8f}, {512, 0, 512, 512, -.8f, .9f, .85f, -.8f}}};
	REQUIRE(u_foveation_angular_target_valid(&t));
	t.views[1].x = INT32_MAX;
	REQUIRE_FALSE(u_foveation_angular_target_valid(&t));
	t.views[1].x = 512;
	t.views[0].up = NAN;
	REQUIRE_FALSE(u_foveation_angular_target_valid(&t));
	t.views[0].up = 1.58f;
	REQUIRE_FALSE(u_foveation_angular_target_valid(&t));
	u_foveation_angular_config c = {.3f, 12, 27, 2, true};
	c.outer = NAN;
	REQUIRE_FALSE(u_foveation_angular_config_valid(&c));
	c.outer = .3f;
	c.step = 0;
	REQUIRE_FALSE(u_foveation_angular_config_valid(&c));
}

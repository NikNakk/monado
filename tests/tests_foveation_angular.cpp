// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#include "foveation/u_foveation_angular.h"
#include "catch_amalgamated.hpp"
#include <cmath>
#include <cstring>
TEST_CASE("Angular gaze preserves quantization, hysteresis, hold and fallback", "[foveation][angular]")
{
	u_foveation_angular_config c = {.3f, 12.f, 27.f, 2.f, true, U_FOVEATION_ANGULAR_SMOOTHSTEP, 0};
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
	u_foveation_angular_config c = {.3f, 12, 27, 2, true, U_FOVEATION_ANGULAR_SMOOTHSTEP, 0};
	c.outer = NAN;
	REQUIRE_FALSE(u_foveation_angular_config_valid(&c));
	c.outer = .3f;
	c.step = 0;
	REQUIRE_FALSE(u_foveation_angular_config_valid(&c));
}

TEST_CASE("Gaussian alternatives preserve linear-rate bounds and separable identity", "[foveation][angular]")
{
	struct u_foveation_angular_config c = {.3f, 12, 27, 2, true, U_FOVEATION_ANGULAR_GAUSSIAN_CLAMPED, 15};
	REQUIRE(u_foveation_angular_config_valid(&c));
	for (int shape = 1; shape <= 3; ++shape) {
		c.shape = (enum u_foveation_angular_shape)shape;
		float last = 1;
		for (unsigned angle = 0; angle <= 90; ++angle) {
			float a = u_foveation_angular_axis_rate(&c, float(angle), 20, 35);
			REQUIRE(a >= .3f);
			REQUIRE(a <= 1);
			REQUIRE(a <= last);
			REQUIRE(a == u_foveation_angular_axis_rate(&c, -float(angle), 20, 35));
			last = a;
		}
		REQUIRE(u_foveation_angular_axis_rate(&c, 0, 20, 35) == 1);
	}
	c.shape = U_FOVEATION_ANGULAR_GAUSSIAN_CLAMPED;
	float x = u_foveation_angular_axis_rate(&c, 8, 20, 35), y = u_foveation_angular_axis_rate(&c, 12, 20, 35);
	REQUIRE(x * y == Catch::Approx(std::exp(-.5f * (8 * 8 + 12 * 12) / (15 * 15))));
	c.shape = U_FOVEATION_ANGULAR_GAUSSIAN_CORE;
	REQUIRE(u_foveation_angular_axis_rate(&c, 20, 20, 35) == 1);
	REQUIRE(u_foveation_angular_axis_rate(&c, 21, 20, 35) < 1);
	c.sigma_degrees = 0;
	REQUIRE_FALSE(u_foveation_angular_config_valid(&c));
	c.sigma_degrees = NAN;
	REQUIRE_FALSE(u_foveation_angular_config_valid(&c));
	c.sigma_degrees = 181;
	REQUIRE_FALSE(u_foveation_angular_config_valid(&c));
	c.shape = (enum u_foveation_angular_shape)99;
	REQUIRE_FALSE(u_foveation_angular_config_valid(&c));
	c.shape = U_FOVEATION_ANGULAR_SMOOTHSTEP;
	c.sigma_degrees = 0;
	REQUIRE(u_foveation_angular_config_valid(&c));
}

TEST_CASE("Tracked alternatives preserve the broad legacy loss fallback", "[foveation][angular]")
{
	struct u_foveation_angular_target t = {
	    1024, 512, {{0, 0, 512, 512, -.9f, .8f, .85f, -.8f}, {512, 0, 512, 512, -.8f, .9f, .85f, -.8f}}};
	struct u_foveation_angular_config c = {.3f, 12, 27, 2, true, U_FOVEATION_ANGULAR_SMOOTHSTEP, 20};
	struct u_foveation_angular_history h = {};
	struct u_foveation_angular_recipe legacy = {}, alternative = {};
	u_foveation_angular_build_recipe(&c, &t, &h, &legacy);
	for (int shape = 1; shape <= 3; ++shape) {
		c.shape = (enum u_foveation_angular_shape)shape;
		u_foveation_angular_build_recipe(&c, &t, &h, &alternative);
		REQUIRE(std::memcmp(&legacy, &alternative, sizeof(legacy)) == 0);
		c.eye_tracked = false;
		u_foveation_angular_build_recipe(&c, &t, &h, &alternative);
		REQUIRE(std::memcmp(&legacy, &alternative, sizeof(legacy)) != 0);
		c.eye_tracked = true;
	}
}

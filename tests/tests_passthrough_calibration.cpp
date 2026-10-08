// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "util/u_passthrough_calibration.h"
#include "util/u_json.h"
#include "math/m_api.h"
#include "catch_amalgamated.hpp"
#include <cmath>
#include <cstring>
#include <string>

static std::string
candidate()
{
	std::string camera = R"({"view":0,"width":1024,"height":1016,"model":"fisheye_equidistant4",
	"intrinsics":{"fx":400,"fy":400,"cx":511.5,"cy":507.5},
	"distortion":{"k1":0,"k2":0,"k3":0,"k4":0},
	"head_from_camera_xrt":{"orientation":{"x":0,"y":0,"z":0,"w":1},"position":{"x":0,"y":0,"z":0}}})";
	std::string other = camera;
	other.replace(other.find("\"view\":0"), 8, "\"view\":1");
	return R"({"format":"psvr2-passthrough-calibration-v1","headset_serial":"test","projection":"rotation-only","cameras":[)" +
	       camera + "," + other + "]}";
}

TEST_CASE("Passthrough schema rejects mismatches and malformed geometry atomically")
{
	struct u_passthrough_calibration cal = {};
	const auto json = candidate();
	REQUIRE(u_passthrough_calibration_parse(json.c_str(), "test", &cal));
	REQUIRE_FALSE(u_passthrough_calibration_parse(json.c_str(), "other", &cal));
	REQUIRE_FALSE(u_passthrough_calibration_parse(nullptr, "test", &cal));
	for (const char *key : {"format", "projection", "headset_serial", "view", "width", "height", "model", "fx",
	                        "k4", "orientation"}) {
		auto bad = json;
		bad.replace(bad.find(key), strlen(key), "missing");
		REQUIRE_FALSE(u_passthrough_calibration_parse(bad.c_str(), "test", &cal));
		REQUIRE(cal.cameras[0].fx == 400);
	}
	auto unbound = json;
	unbound.replace(unbound.find("\"test\""), 6, "null");
	REQUIRE(u_passthrough_calibration_parse(unbound.c_str(), "other", &cal));
	auto duplicate = json;
	duplicate.replace(duplicate.find("\"view\":1"), 8, "\"view\":0");
	REQUIRE_FALSE(u_passthrough_calibration_parse(duplicate.c_str(), "test", &cal));
	auto zero_quat = json;
	zero_quat.replace(zero_quat.find("\"w\":1"), 5, "\"w\":0");
	REQUIRE_FALSE(u_passthrough_calibration_parse(zero_quat.c_str(), "test", &cal));
	REQUIRE_FALSE(u_passthrough_calibration_parse((json + " garbage").c_str(), "test", &cal));
}

TEST_CASE("Passthrough projects XRT rays with pixel centres, fisheye distortion and inverse head rotation")
{
	struct u_passthrough_calibration cal = {};
	REQUIRE(u_passthrough_calibration_parse(candidate().c_str(), "test", &cal));
	auto &cam = cal.cameras[0];
	struct xrt_vec2 uv;
	struct xrt_vec3 ray = {0, 0, -1};
	REQUIRE(u_passthrough_calibration_project(&cam, &ray, &uv));
	REQUIRE(uv.x == Catch::Approx(0.5));
	REQUIRE(uv.y == Catch::Approx(0.5));
	ray = {1, 1, -1};
	cam.k[0] = 0.1;
	const double theta = atan(sqrt(2.0));
	const double radius = theta * (1 + 0.1 * theta * theta);
	REQUIRE(u_passthrough_calibration_project(&cam, &ray, &uv));
	REQUIRE(uv.x == Catch::Approx(0.5 + 400 * radius / sqrt(2.0) / 1024));
	REQUIRE(uv.y == Catch::Approx(0.5 - 400 * radius / sqrt(2.0) / 1016));
	struct xrt_vec3 head_ray;
	struct xrt_vec3 axis = {0, 1, 0};
	math_quat_from_angle_vector(0.3f, &axis, &cam.head_from_camera.orientation);
	math_quat_rotate_vec3(&cam.head_from_camera.orientation, &ray, &head_ray);
	struct xrt_vec2 rotated;
	REQUIRE(u_passthrough_calibration_project(&cam, &head_ray, &rotated));
	REQUIRE(rotated.x == Catch::Approx(uv.x));
	REQUIRE(rotated.y == Catch::Approx(uv.y));
	ray = {0, 0, 1};
	REQUIRE_FALSE(u_passthrough_calibration_project(&cam, &ray, &uv));
	cam.fx = 10000;
	ray = {1, 0, -1};
	REQUIRE_FALSE(u_passthrough_calibration_project(&cam, &ray, &uv));
}

TEST_CASE("Passthrough projection agrees with OpenCV fisheye projectPoints")
{
	struct u_passthrough_camera cam = {};
	cam.fx = 380;
	cam.fy = 379;
	cam.cx = 503.4;
	cam.cy = 502.8;
	cam.k[0] = .03;
	cam.k[1] = -.014;
	cam.k[2] = .006;
	cam.k[3] = -.0017;
	cam.head_from_camera.orientation.w = 1;
	struct xrt_vec3 ray = {.3f, .2f, -1};
	struct xrt_vec2 uv;
	REQUIRE(u_passthrough_calibration_project(&cam, &ray, &uv));
	// OpenCV 5 fisheye.projectPoints([.3, -.2, 1], K, D), then Metal centre conversion.
	REQUIRE(uv.x == Catch::Approx((613.18499557 + .5) / 1024).margin(1e-7));
	REQUIRE(uv.y == Catch::Approx((429.80260821 + .5) / 1016).margin(1e-7));
}

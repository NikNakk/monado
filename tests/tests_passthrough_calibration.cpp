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
	REQUIRE(u_passthrough_calibration_parse_default(json.c_str(), "test", &cal));
	REQUIRE_FALSE(u_passthrough_calibration_parse_default(json.c_str(), "other", &cal));
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
	REQUIRE_FALSE(u_passthrough_calibration_parse_default(unbound.c_str(), "other", &cal));
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

TEST_CASE("Camera rotational reprojection keeps a world direction fixed across head motion")
{
	struct u_passthrough_camera camera = {};
	struct xrt_vec3 camera_axis = {0, 1, 0};
	math_quat_from_angle_vector(.25f, &camera_axis, &camera.head_from_camera.orientation);
	const struct xrt_vec3 camera_ray = {.1f, -.2f, -1};
	for (const struct xrt_vec3 axis : {xrt_vec3{1, 0, 0}, xrt_vec3{0, 1, 0}, xrt_vec3{0, 0, 1}}) {
		for (float movement : {-.4f, 0.0f, .4f}) {
			struct xrt_quat capture, display, inverse_display, camera_from_display;
			math_quat_from_angle_vector(-.15f, &camera_axis, &capture);
			math_quat_from_angle_vector(movement, &axis, &display);
			// A fixed world direction, observed by the camera at capture.
			struct xrt_vec3 capture_ray, world_ray, display_ray, recovered;
			math_quat_rotate_vec3(&camera.head_from_camera.orientation, &camera_ray, &capture_ray);
			math_quat_rotate_vec3(&capture, &capture_ray, &world_ray);
			math_quat_invert(&display, &inverse_display);
			math_quat_rotate_vec3(&inverse_display, &world_ray, &display_ray);
			REQUIRE(u_passthrough_calibration_rotation(&camera, &capture, &display, &camera_from_display));
			math_quat_rotate_vec3(&camera_from_display, &display_ray, &recovered);
			CHECK(recovered.x == Catch::Approx(camera_ray.x).margin(1e-6));
			CHECK(recovered.y == Catch::Approx(camera_ray.y).margin(1e-6));
			CHECK(recovered.z == Catch::Approx(camera_ray.z).margin(1e-6));
		}
	}
}

TEST_CASE("Stationary rotational reprojection matches the static camera transform and rejects invalid poses")
{
	struct u_passthrough_camera camera = {};
	struct xrt_vec3 axis = {1, 0, 0};
	math_quat_from_angle_vector(.5f, &axis, &camera.head_from_camera.orientation);
	struct xrt_quat head, result, inverse_camera;
	math_quat_from_angle_vector(-.7f, &axis, &head);
	REQUIRE(u_passthrough_calibration_rotation(&camera, &head, &head, &result));
	math_quat_invert(&camera.head_from_camera.orientation, &inverse_camera);
	struct xrt_vec3 ray = {.2f, .3f, -1}, expected, actual;
	math_quat_rotate_vec3(&inverse_camera, &ray, &expected);
	math_quat_rotate_vec3(&result, &ray, &actual);
	CHECK(actual.x == Catch::Approx(expected.x));
	CHECK(actual.y == Catch::Approx(expected.y));
	CHECK(actual.z == Catch::Approx(expected.z));
	struct xrt_quat invalid = {};
	CHECK_FALSE(u_passthrough_calibration_rotation(&camera, &invalid, &head, &result));
	CHECK_FALSE(u_passthrough_calibration_rotation(&camera, &head, &invalid, &result));
}

TEST_CASE("Camera reprojection rejects arrival-only, future and stale timestamps")
{
	constexpr int64_t now = 1000000000;
	CHECK(u_passthrough_calibration_frame_is_fresh(now - 10000000, 900000000, now));
	CHECK_FALSE(u_passthrough_calibration_frame_is_fresh(now - 10000000, 0, now));
	CHECK_FALSE(u_passthrough_calibration_frame_is_fresh(0, 900000000, now));
	CHECK_FALSE(u_passthrough_calibration_frame_is_fresh(now + 1, 900000000, now));
	CHECK_FALSE(u_passthrough_calibration_frame_is_fresh(now - 250000000, 900000000, now));
}

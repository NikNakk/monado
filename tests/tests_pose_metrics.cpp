// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "catch_amalgamated.hpp"
#include "pose_metrics.h"
#include "t_constellation_tracker_internal.hpp"

#include <cstdlib>

#include <cmath>


TEST_CASE("Pose metrics assign blobs to unique LEDs")
{
	struct camera_model camera = {};
	camera.width = camera.height = 200;
	camera.calib.fx = camera.calib.fy = 100.0f;
	camera.calib.cx = camera.calib.cy = 100.0f;
	camera.calib.model = T_DISTORTION_OPENCV_RADTAN_8;
	struct t_constellation_tracker_led leds[] = {
	    {
	        .position = {0.00f, 0.0f, 1.0f},
	        .normal = {0.0f, 0.0f, -1.0f},
	        .radius_m = 0.10f,
	        .visibility_angle = (float)M_PI_2,
	        .id = 0,
	    },
	    {
	        .position = {0.04f, 0.0f, 1.0f},
	        .normal = {0.0f, 0.0f, -1.0f},
	        .radius_m = 0.10f,
	        .visibility_angle = (float)M_PI_2,
	        .id = 1,
	    },
	};
	struct t_constellation_tracker_led_model model = {};
	model.leds = leds;
	model.led_count = 2;
	struct t_blob blobs[3] = {};
	const float centers[] = {100.0f, 101.0f, 100.5f};
	for (int i = 0; i < 3; i++) {
		blobs[i].blob_id = i + 1;
		blobs[i].matched_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID;
		blobs[i].matched_device_led_id = XRT_CONSTELLATION_INVALID_LED_ID;
		blobs[i].center = {centers[i], 100.0f};
		blobs[i].size = {2.0f, 2.0f};
	}

	CHECK_FALSE(model.unique_blob_matches);
	model.unique_blob_matches = true;
	struct pose_metrics_blob_match_info matches = {};
	struct xrt_pose pose = XRT_POSE_IDENTITY;
	pose_metrics_match_pose_to_blobs(&pose, blobs, 3, &model, 0, &camera, &matches);

	CHECK(matches.num_visible_leds == 2);
	CHECK(matches.matched_blobs == 2);
	CHECK(matches.unmatched_blobs == 1);
	REQUIRE(matches.visible_leds[0].matched_blob != nullptr);
	REQUIRE(matches.visible_leds[1].matched_blob != nullptr);
	CHECK(matches.visible_leds[0].matched_blob != matches.visible_leds[1].matched_blob);

	// Preserve the upstream default assignment policy for other devices.
	model.unique_blob_matches = false;
	matches = {};
	pose_metrics_match_pose_to_blobs(&pose, blobs, 3, &model, 0, &camera, &matches);
	CHECK(matches.matched_blobs == 3);
	CHECK(matches.degenerate_solution);
}

TEST_CASE("Constellation preserves the Rift-style legacy sample callback")
{
	struct LegacyDevice : t_constellation_tracker_device
	{
		int calls{0};
		t_constellation_tracker_sample received{};
	};
	LegacyDevice device{};
	device.push_constellation_tracker_sample = [](t_constellation_tracker_device *base,
	                                              t_constellation_tracker_sample *sample) {
		auto *legacy = static_cast<LegacyDevice *>(base);
		legacy->calls++;
		legacy->received = *sample;
	};
	t_constellation_tracker_sample sample{};
	sample.timestamp_ns = 123456;
	sample.pose = XRT_POSE_IDENTITY;
	sample.pose.position = {0.1f, 0.2f, 0.3f};
	CHECK(device.push_optical_sample == nullptr);
	CHECK(device.push_camera_blob_count == nullptr);
	CHECK(device.push_camera_led_blob_count == nullptr);
	CHECK(t_constellation_tracker_device_push_sample(&device, &sample));
	CHECK(device.calls == 1);
	CHECK(device.received.timestamp_ns == sample.timestamp_ns);
	CHECK(device.received.pose.position.x == sample.pose.position.x);
	CHECK(device.received.pose.position.y == sample.pose.position.y);
	CHECK(device.received.pose.position.z == sample.pose.position.z);

	// Only the new opt-in acceptance callback may reject a sample.
	device.push_optical_sample = [](t_constellation_tracker_device *, t_constellation_tracker_sample *) {
		return false;
	};
	CHECK_FALSE(t_constellation_tracker_device_push_sample(&device, &sample));
	CHECK(device.calls == 1);
}

TEST_CASE("Legacy trackers ignore inherited Sense joint solver settings")
{
	// The cached option is deliberately enabled: the caller opt-in must still be required.
	const char *previous = std::getenv("CONSTELLATION_TRACKER_JOINT");
	const bool had_previous = previous != nullptr;
	const std::string saved = previous != nullptr ? previous : "";
	auto set_joint_option = [](const char *value) {
#if defined(_WIN32)
		_putenv_s("CONSTELLATION_TRACKER_JOINT", value != nullptr ? value : "");
#else
		if (value != nullptr) {
			setenv("CONSTELLATION_TRACKER_JOINT", value, 1);
		} else {
			unsetenv("CONSTELLATION_TRACKER_JOINT");
		}
#endif
	};
	set_joint_option("1");
	t_constellation_tracker_params params{};
	params.flags = T_CONSTELLATION_TRACKER_FLAGS_DETERMINISTIC;
	params.num_mosaics = 1;
	params.mosaics[0].num_cameras = 1;
	auto &camera = params.mosaics[0].cameras[0];
	camera.calibration.image_size_pixels = {200, 200};
	camera.calibration.intrinsics[0][0] = camera.calibration.intrinsics[1][1] = 100.0;
	camera.calibration.intrinsics[0][2] = camera.calibration.intrinsics[1][2] = 100.0;
	camera.calibration.intrinsics[2][2] = 1.0;
	camera.calibration.distortion_model = T_DISTORTION_OPENCV_RADTAN_8;
	camera.pose_in_origin = XRT_POSE_IDENTITY;
	camera.has_concrete_pose = true;
	{
		xrt::tracking::constellation::ConstellationTracker legacy(&params);
		CHECK(legacy.joint == nullptr);
	}
	params.flags = (t_constellation_tracker_flags)(params.flags | T_CONSTELLATION_TRACKER_FLAGS_ALLOW_JOINT);
	{
		xrt::tracking::constellation::ConstellationTracker sense(&params);
		CHECK(sense.joint != nullptr);
	}
	if (had_previous) {
		set_joint_option(saved.c_str());
	} else {
		set_joint_option(nullptr);
	}
}

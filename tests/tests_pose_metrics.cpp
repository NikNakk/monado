// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "catch_amalgamated.hpp"
#include "pose_metrics.h"

#include <cmath>


TEST_CASE("Pose metrics assign blobs to unique LEDs")
{
	struct camera_model camera = {
	    .width = 200,
	    .height = 200,
	    .calib =
	        {
	            .fx = 100.0f,
	            .fy = 100.0f,
	            .cx = 100.0f,
	            .cy = 100.0f,
	            .model = T_DISTORTION_OPENCV_RADTAN_8,
	        },
	};
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
	struct t_constellation_tracker_led_model model = {
	    .leds = leds,
	    .led_count = 2,
	};
	struct t_blob blobs[] = {
	    {
	        .blob_id = 1,
	        .matched_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID,
	        .matched_device_led_id = XRT_CONSTELLATION_INVALID_LED_ID,
	        .center = {100.0f, 100.0f},
	        .size = {2.0f, 2.0f},
	    },
	    {
	        .blob_id = 2,
	        .matched_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID,
	        .matched_device_led_id = XRT_CONSTELLATION_INVALID_LED_ID,
	        .center = {101.0f, 100.0f},
	        .size = {2.0f, 2.0f},
	    },
	    {
	        .blob_id = 3,
	        .matched_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID,
	        .matched_device_led_id = XRT_CONSTELLATION_INVALID_LED_ID,
	        .center = {100.5f, 100.0f},
	        .size = {2.0f, 2.0f},
	    },
	};

	struct pose_metrics_blob_match_info matches = {};
	struct xrt_pose pose = XRT_POSE_IDENTITY;
	pose_metrics_match_pose_to_blobs(&pose, blobs, 3, &model, 0, &camera, &matches);

	CHECK(matches.num_visible_leds == 2);
	CHECK(matches.matched_blobs == 2);
	CHECK(matches.unmatched_blobs == 1);
	REQUIRE(matches.visible_leds[0].matched_blob != nullptr);
	REQUIRE(matches.visible_leds[1].matched_blob != nullptr);
	CHECK(matches.visible_leds[0].matched_blob != matches.visible_leds[1].matched_blob);
}

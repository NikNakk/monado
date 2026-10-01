// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Round trip of the constellation dataset format, including extension records (packet 5).
 * @author Nick Kennedy
 */

#include "t_constellation_tracker_dataset.hpp"

#include "catch_amalgamated.hpp"

#include <cstdio>
#include <string>

#include <unistd.h>

using namespace xrt::tracking::constellation;

namespace {

std::string
temp_path(const char *name)
{
	const char *dir = std::getenv("TMPDIR");
	return std::string(dir != nullptr ? dir : "/tmp") + "/" + name;
}

t_camera_calibration
make_calibration(double fx)
{
	t_camera_calibration cal{};
	cal.image_size_pixels.w = 640;
	cal.image_size_pixels.h = 480;
	cal.intrinsics[0][0] = fx;
	cal.intrinsics[1][1] = fx;
	cal.intrinsics[0][2] = 320.0;
	cal.intrinsics[1][2] = 240.0;
	cal.intrinsics[2][2] = 1.0;
	cal.distortion_model = T_DISTORTION_FISHEYE_KB4;
	cal.kb4 = t_camera_calibration_kb4_params{0.1, -0.02, 0.003, -0.0004};
	return cal;
}

CameraSample
make_sample(uint64_t id, int64_t timestamp_ns, uint32_t camera_index)
{
	CameraSample sample{};
	sample.id = id;
	sample.timestamp_ns = timestamp_ns;
	sample.mosaic_index = 0;
	sample.camera_index = camera_index;
	sample.blob_count = 2;
	for (uint32_t i = 0; i < sample.blob_count; i++) {
		t_blob &b = sample.blobs[i];
		b.blob_id = 100 + i;
		b.matched_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID;
		b.matched_device_led_id = XRT_CONSTELLATION_INVALID_LED_ID;
		b.center = xrt_vec2{10.5f + (float)i, 20.25f};
		b.size = xrt_vec2{3.0f, 3.0f};
		b.brightness = 0.5f;
	}
	sample.Txr_world_cam = xrt_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.1f, 1.6f, 0.0f}};
	return sample;
}

} // namespace

TEST_CASE("constellation dataset round trip with extension records")
{
	const std::string path = temp_path("tests_constellation_dataset.ctd");

	std::vector<t_constellation_tracker_led> leds(3);
	for (size_t i = 0; i < leds.size(); i++) {
		leds[i].position = xrt_vec3{0.01f * (float)i, 0.02f, 0.03f};
		leds[i].normal = xrt_vec3{0.0f, 0.0f, 1.0f};
		leds[i].radius_m = 0.002f;
		leds[i].visibility_angle = 1.2f;
		leds[i].id = (t_constellation_led_id_it)i;
	}
	t_constellation_tracker_led_model model{};
	model.leds = leds.data();
	model.led_count = leds.size();

	{
		DataRecorder recorder(path, {{make_calibration(300.0), make_calibration(310.0)}});
		recorder.recordSessionInfo("{\"tool\":\"test\"}");
		recorder.recordDeviceInfo(0, model);
		recorder.recordSample(make_sample(1, 1'000'000, 0));

		// An extension kind no reader knows must be skipped, and reading must continue after it.
		recorder.recordExtension(9999, {1, 2, 3, 4, 5});

		recorder.recordSyncEvent(DatasetSyncEvent{0, 2'000'000, 3, {12.5, 400.0, 0.0}});
		recorder.recordImuTiming(DatasetImuTiming{0, 2'500'000, 7'000'000'000, 6'997'500'000.0, {0.01, -0.02, 0.03}});
		recorder.recordHeadPose(DatasetHeadPose{3'000'000,
		                                        XRT_SPACE_RELATION_ORIENTATION_VALID_BIT,
		                                        xrt_pose{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.7f, 0.2f}},
		                                        2'990'000,
		                                        1});
		recorder.recordGroundTruth(DatasetGroundTruth{0, 3'000'000,
		                                              xrt_pose{{0.0f, 0.70710677f, 0.0f, 0.70710677f}, {0.2f, 1.1f, -0.4f}},
		                                              0.001f, 0.002f, 1});
		recorder.recordAnnotation(DatasetAnnotation{0, 4'000'000, "static_begin"});
		recorder.recordSample(make_sample(2, 5'000'000, 1));
	}

	DatasetReader reader(path);
	std::remove(path.c_str());

	CHECK(reader.stop_reason.empty());
	REQUIRE(reader.mosaics.size() == 1);
	REQUIRE(reader.mosaics[0].camera_calibrations.size() == 2);
	CHECK(reader.mosaics[0].camera_calibrations[1].intrinsics[0][0] == 310.0);
	REQUIRE(reader.devices.size() == 1);
	CHECK(reader.devices[0].leds.size() == 3);

	REQUIRE(reader.samples.size() == 2);
	CHECK(reader.samples[1].id == 2);
	CHECK(reader.samples[1].camera_index == 1);
	CHECK(reader.samples[1].blobs[1].center.x == 11.5f);

	CHECK(reader.unknown_extensions == 1);
	REQUIRE(reader.session_info.size() == 1);
	CHECK(reader.session_info[0] == "{\"tool\":\"test\"}");

	REQUIRE(reader.sync_events.size() == 1);
	CHECK(reader.sync_events[0].kind == 3);
	CHECK(reader.sync_events[0].value[1] == 400.0);

	REQUIRE(reader.imu_timing.size() == 1);
	CHECK(reader.imu_timing[0].device_ns == 7'000'000'000);
	CHECK(reader.imu_timing[0].clock_offset_ns == 6'997'500'000.0);
	CHECK(reader.imu_timing[0].applied_gyro_bias[2] == 0.03);

	REQUIRE(reader.head_poses.size() == 1);
	CHECK(reader.head_poses[0].source_ns == 2'990'000);
	CHECK(reader.head_poses[0].Txr_world_head.position.y == 1.7f);

	REQUIRE(reader.ground_truth.size() == 1);
	CHECK(reader.ground_truth[0].Txr_world_device.orientation.y == 0.70710677f);
	CHECK(reader.ground_truth[0].orientation_sigma_rad == 0.002f);

	REQUIRE(reader.annotations.size() == 1);
	CHECK(reader.annotations[0].text == "static_begin");
	CHECK(reader.annotations[0].host_ns == 4'000'000);
}

TEST_CASE("constellation dataset reader stops cleanly on a truncated extension")
{
	const std::string path = temp_path("tests_constellation_dataset_truncated.ctd");
	{
		DataRecorder recorder(path, {{make_calibration(300.0)}});
		recorder.recordSample(make_sample(1, 1'000'000, 0));
		recorder.recordAnnotation(DatasetAnnotation{XRT_CONSTELLATION_INVALID_DEVICE_ID, 2'000'000, "a marker"});
	}
	// Cut the last record short.
	FILE *f = std::fopen(path.c_str(), "rb+");
	REQUIRE(f != nullptr);
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fclose(f);
	REQUIRE(truncate(path.c_str(), size - 3) == 0);

	DatasetReader reader(path);
	std::remove(path.c_str());

	CHECK(reader.samples.size() == 1);
	CHECK(reader.annotations.empty());
	CHECK_FALSE(reader.stop_reason.empty());
}

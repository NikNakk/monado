// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "catch_amalgamated.hpp"
#include "target_psvr2_sense_tracking.h"
#include "xrt/xrt_device.h"
#include "math/m_api.h"
#include "psvr2/psvr2.h"
#include "os/os_hid.h"

#include <future>
#include <thread>
#include <chrono>

#include <filesystem>
#include <fstream>
#include <string>

using Catch::Approx;

namespace {
std::string
calibration(bool duplicate = false, bool extrinsic = true, const char *fx = "300")
{
	std::string json = R"({"format":"psvr2-mode4-constellation-calibration-v1","runtime_usable":false,"cameras":[)";
	for (int i = 0; i < 4; ++i) {
		if (i)
			json += ",";
		json +=
		    "{\"camera\":" + std::to_string(duplicate ? 0 : i) +
		    R"(,"calibration":{"model":"fisheye_equidistant4","resolution":{"width":512,"height":508},"intrinsics":{"fx":)" +
		    fx +
		    R"(,"fy":300,"cx":256,"cy":254},"distortion":{"k1":0,"k2":0,"k3":0,"k4":0}},"pose_in_tracking_origin_xrt":{"position":{"x":0,"y":0,"z":0},"orientation":{"x":0,"y":0,"z":0,"w":1}}})";
	}
	json += "]";
	if (extrinsic)
		json +=
		    R"(,"head_from_camera0_xrt":{"position":{"x":0.04,"y":0,"z":-0.1},"orientation":{"x":0,"y":0,"z":0,"w":1}})";
	return json + "}";
}

struct CalibrationFile
{
	std::filesystem::path path = std::filesystem::temp_directory_path() / "monado-sense-calibration-test.json";
	explicit CalibrationFile(const std::string &json)
	{
		std::ofstream(path) << json;
	}
	~CalibrationFile()
	{
		std::filesystem::remove(path);
	}
};

bool
load(const std::string &json, t_constellation_tracker_params &params, xrt_pose &pose, bool &have)
{
	CalibrationFile file(json);
	return psvr2_constellation_load_calibration(file.path.c_str(), &params, &pose, &have);
}
} // namespace

TEST_CASE("Sense calibration validates the four-camera experimental rig")
{
	t_constellation_tracker_params params{};
	xrt_pose pose{};
	bool have = false;
	REQUIRE(load(calibration(), params, pose, have));
	REQUIRE(params.num_mosaics == 1);
	REQUIRE(params.mosaics[0].num_cameras == 4);
	REQUIRE(have);
	REQUIRE(pose.position.x == Approx(0.04));
	REQUIRE(params.mosaics[0].cameras[2].calibration.distortion_model == T_DISTORTION_FISHEYE_KB4);
	REQUIRE_FALSE(load(calibration(true), params, pose, have));
	REQUIRE_FALSE(load(calibration(false, true, "-1"), params, pose, have));
	REQUIRE_FALSE(load(calibration(false, true, "1e999"), params, pose, have));
	REQUIRE_FALSE(load("{}", params, pose, have));
	REQUIRE_FALSE(load("bad JSON", params, pose, have));
	REQUIRE(load(calibration(false, false), params, pose, have));
	REQUIRE_FALSE(have);
	REQUIRE(pose.orientation.w == 1.0f);
}

TEST_CASE("Sense camera origin composes the head pose and preserves validity")
{
	xrt_device head{};
	head.get_tracked_pose = [](xrt_device *, xrt_input_name, int64_t, xrt_space_relation *out) {
		*out = XRT_SPACE_RELATION_ZERO;
		out->pose = XRT_POSE_IDENTITY;
		out->pose.position = {1.0f, 2.0f, 3.0f};
		out->relation_flags = static_cast<xrt_space_relation_flags>(XRT_SPACE_RELATION_POSITION_VALID_BIT |
		                                                            XRT_SPACE_RELATION_ORIENTATION_VALID_BIT);
		return XRT_SUCCESS;
	};
	psvr2_head_tracking_origin origin{};
	psvr2_head_tracking_origin_init(&origin, &head);
	origin.head_from_camera0.position = {0.04f, 0.0f, -0.1f};
	xrt_space_relation relation{};
	origin.base.get_tracked_pose(&origin.base, 123, &relation);
	REQUIRE(relation.pose.position.x == Approx(1.04));
	REQUIRE(relation.pose.position.z == Approx(2.9));
	REQUIRE((relation.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
	head.get_tracked_pose = [](xrt_device *, xrt_input_name, int64_t, xrt_space_relation *) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	};
	origin.base.get_tracked_pose(&origin.base, 123, &relation);
	REQUIRE(relation.relation_flags == 0);
}

TEST_CASE("Sense camera sinks reject replacement and drain before detaching")
{
	struct psvr2_hmd head{};
	std::snprintf(head.base.str, sizeof(head.base.str), "PS VR2 test");
	os_mutex_init(&head.data_lock);
	xrt_frame_sink sink{};
	xrt_frame_sink *sinks[4] = {&sink, &sink, &sink, &sink};
	REQUIRE(psvr2_set_camera_frame_sinks(&head.base, sinks));
	CHECK_FALSE(psvr2_set_camera_frame_sinks(&head.base, sinks));
	head.camera_frame_pushes_in_flight = 1;
	auto detached =
	    std::async(std::launch::async, [&] { return psvr2_set_camera_frame_sinks(&head.base, nullptr); });
	bool cleared = false;
	for (int i = 0; i < 500 && !cleared; ++i) {
		os_mutex_lock(&head.data_lock);
		cleared = head.camera_frame_sinks[0] == nullptr;
		os_mutex_unlock(&head.data_lock);
		if (!cleared)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(cleared);
	CHECK(detached.wait_for(std::chrono::milliseconds(10)) == std::future_status::timeout);
	os_mutex_lock(&head.data_lock);
	head.camera_frame_pushes_in_flight = 0;
	os_mutex_unlock(&head.data_lock);
	CHECK(detached.get());
	CHECK(psvr2_set_camera_frame_sinks(&head.base, sinks));
	CHECK(psvr2_set_camera_frame_sinks(&head.base, nullptr));
	auto hook = [](void *) {};
	CHECK(psvr2_set_teardown_hook(&head.base, hook, nullptr));
	CHECK_FALSE(psvr2_set_teardown_hook(&head.base, hook, nullptr));
	CHECK(psvr2_set_teardown_hook(&head.base, nullptr, nullptr));
	CHECK(psvr2_set_teardown_hook(&head.base, hook, nullptr));
	os_mutex_destroy(&head.data_lock);
}

TEST_CASE("Sense optical acceptance preserves the legacy device callback")
{
	t_constellation_tracker_device device{};
	t_constellation_tracker_sample sample{};
	device.push_constellation_tracker_sample = [](t_constellation_tracker_device *,
	                                              t_constellation_tracker_sample *s) { s->timestamp_ns = 12; };
	CHECK(t_constellation_tracker_device_push_sample(&device, &sample));
	CHECK(sample.timestamp_ns == 12);
	device.push_optical_sample = [](t_constellation_tracker_device *, t_constellation_tracker_sample *s) {
		s->timestamp_ns = 24;
		return false;
	};
	CHECK_FALSE(t_constellation_tracker_device_push_sample(&device, &sample));
	CHECK(sample.timestamp_ns == 24);
}

TEST_CASE("HID receipt timestamps are optional on existing backends")
{
	os_hid_device hid{};
	hid.read = [](os_hid_device *, uint8_t *, size_t, int) { return 7; };
	int64_t timestamp = 123;
	CHECK(os_hid_read_with_timestamp(&hid, nullptr, 0, 0, &timestamp) == 7);
	CHECK(timestamp == 0);
	hid.read_with_timestamp = [](os_hid_device *, uint8_t *, size_t, int, int64_t *out) {
		if (out)
			*out = 456;
		return 8;
	};
	CHECK(os_hid_read_with_timestamp(&hid, nullptr, 0, 0, &timestamp) == 8);
	CHECK(timestamp == 456);
	CHECK(os_hid_read_with_timestamp(&hid, nullptr, 0, 0, nullptr) == 8);
}

TEST_CASE("Load a supplied PS VR2 calibration without hardware", "[.sense-calibration]")
{
	const char *path = std::getenv("MONADO_SENSE_TEST_CALIBRATION");
	REQUIRE(path != nullptr);
	t_constellation_tracker_params params{};
	xrt_pose pose{};
	bool have = false;
	REQUIRE(psvr2_constellation_load_calibration(path, &params, &pose, &have));
	REQUIRE(params.num_mosaics == 1);
	REQUIRE(params.mosaics[0].num_cameras == 4);
	REQUIRE(have);
}

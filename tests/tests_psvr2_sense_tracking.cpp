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

#ifdef XRT_OS_OSX
#include "pssense/pssense_interface.h"
#include "pssense/pssense_protocol.h"
#include "xrt/xrt_prober.h"
#include <atomic>
#include <cstring>

namespace {
struct FaultHid : os_hid_device
{
	std::atomic<int> writes{0};
	std::atomic<int> reads{0};
	std::atomic<bool> disconnected{false};
	int feature_part = 0;
	FaultHid() : os_hid_device{}
	{
		read = [](os_hid_device *base, uint8_t *out, size_t, int) {
			auto &self = *static_cast<FaultHid *>(base);
			if (self.disconnected)
				return -1;
			const int n = ++self.reads;
			pssense_usb_input_report report{};
			report.report_id = INPUT_REPORT_ID_USB;
			report.common.thumbstick_x = report.common.thumbstick_y = 128;
			report.common.trigger_value = self.writes >= 1 ? 255 : 0;
			report.common.imu_ticks = __cpu_to_le32(n * 3000);
			report.common.device_timestamp_ticks = __cpu_to_le32(n * 3000);
			std::memcpy(out, &report, sizeof(report));
			return (int)sizeof(report);
		};
		write = [](os_hid_device *base, const uint8_t *, size_t size) {
			auto &self = *static_cast<FaultHid *>(base);
			return ++self.writes <= 3 ? -1 : (int)size;
		};
		get_feature = [](os_hid_device *base, uint8_t id, uint8_t *out, size_t) {
			auto &self = *static_cast<FaultHid *>(base);
			pssense_calibration_data calibration{};
			calibration.accel_plus_x = calibration.accel_plus_y = calibration.accel_plus_z = 1000;
			calibration.accel_minus_x = calibration.accel_minus_y = calibration.accel_minus_z = -1000;
			calibration.gyro_plus_x = calibration.gyro_plus_y = calibration.gyro_plus_z = 1000;
			calibration.gyro_minus_x = calibration.gyro_minus_y = calibration.gyro_minus_z = -1000;
			pssense_feature_report report{};
			report.report_id = id;
			const int part = self.feature_part++ % 2;
			report.part_id = part == 0 ? CALIBRATION_DATA_PART_ID_1 : CALIBRATION_DATA_PART_ID_2;
			std::memcpy(report.data, (const uint8_t *)&calibration + part * sizeof(report.data), sizeof(report.data));
			std::memcpy(out, &report, sizeof(report));
			return (int)sizeof(report);
		};
		set_feature = [](os_hid_device *, const uint8_t *, size_t size) { return (int)size; };
		destroy = [](os_hid_device *) {};
	}
};
struct FaultProber : xrt_prober
{
	FaultHid hid;
	FaultProber() : xrt_prober{}
	{
		open_hid_interface = [](xrt_prober *base, xrt_prober_device *, int, os_hid_device **out) {
			*out = &static_cast<FaultProber *>(base)->hid;
			return 0;
		};
		get_string_descriptor = [](xrt_prober *, xrt_prober_device *, xrt_prober_string, unsigned char *out, size_t) {
			std::memcpy(out, "Mock Sense", 11);
			return 11;
		};
	}
};
}

TEST_CASE("macOS Sense keeps input alive across output failures and retries")
{
	FaultProber prober;
	xrt_prober_device device{};
	device.product_id = 0x0e45;
	device.bus = XRT_BUS_TYPE_USB;
	xrt_frame_context frames{};
	t_timing_event_sink *sink = nullptr;
	xrt_device *sense = pssense_create(&prober, &device, &frames, &sink);
	REQUIRE(sense != nullptr);
	// Confirm changed input reaches the app while output is still failing.
	bool input_during_failure = false;
	for (int n = 0; n < 100 && !input_during_failure; ++n) {
		sense->update_inputs(sense);
		for (uint32_t i = 0; i < sense->input_count; ++i)
			if (sense->inputs[i].name == XRT_INPUT_PSSENSE_TRIGGER_VALUE)
				input_during_failure = sense->inputs[i].value.vec1.x > 0.9f;
		if (!input_during_failure)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(input_during_failure);
	CHECK(prober.hid.writes < 4);
	// The real driver loop sees three failed writes, then a successful retry.
	for (int i = 0; i < 1000 && prober.hid.writes < 4; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	CHECK(prober.hid.writes >= 4);
	CHECK(prober.hid.reads > 10);
	sense->update_inputs(sense);
	bool pressed = false;
	for (uint32_t i = 0; i < sense->input_count; ++i)
		if (sense->inputs[i].name == XRT_INPUT_PSSENSE_TRIGGER_VALUE)
			pressed = sense->inputs[i].value.vec1.x > 0.9f;
	CHECK(pressed);
	// An actual input disconnection must still terminate polling.
	prober.hid.disconnected = true;
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const int writes = prober.hid.writes;
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	CHECK(prober.hid.writes == writes);
	sense->destroy(sense);
	xrt_frame_context_destroy_nodes(&frames);
}
#endif

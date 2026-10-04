// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the data recorder logic for the constellation tracker.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup tracking
 */

#pragma once

#include "t_constellation_tracker_internal.hpp"

#include <mutex>
#include <string>
#include <vector>


namespace xrt::tracking::constellation {

struct DataSerializer
{
private: // Fields
	std::fstream file;

public: // Methods
	bool upstream_distortion_codes{false};

	DataSerializer(std::string file, bool write);

	~DataSerializer();

	void
	write(uint8_t value);
	void
	read(uint8_t &value);

	void
	write(uint32_t value);
	void
	read(uint32_t &value);

	void
	write(float value);
	void
	read(float &value);

	void
	write(uint64_t value);
	void
	read(uint64_t &value);

	void
	write(double value);
	void
	read(double &value);

	void
	write(const xrt_pose &value);
	void
	read(xrt_pose &value);

	void
	write(const FoundDevicePose &value);
	void
	read(FoundDevicePose &value);

	void
	write(const DeviceState &value);
	void
	read(DeviceState &value);

	void
	write(const t_blob &value);
	void
	read(t_blob &value);

	void
	write(const CameraSample &value);
	void
	read(CameraSample &value);

	void
	write(const t_camera_calibration &value);
	void
	read(t_camera_calibration &value);

	void
	write(t_constellation_tracker_led &value);
	void
	read(t_constellation_tracker_led &value);

	void
	write(const t_constellation_tracker_led_model &value);
	void
	read(std::vector<t_constellation_tracker_led> &led_storage, t_constellation_tracker_led_model &value);

	template <typename T>
	void
	write(const std::optional<T> &opt_value)
	{
		this->write(static_cast<uint8_t>(opt_value.has_value() ? 1 : 0));
		if (opt_value.has_value()) {
			this->write(opt_value.value());
		}
	}

	template <typename T>
	void
	read(std::optional<T> &opt_value)
	{
		uint8_t has_value;
		this->read(has_value);
		if (has_value) {
			T value;
			this->read(value);
			opt_value = value;
		} else {
			opt_value = std::nullopt;
		}
	}

	void
	flush();

	//! True when reading has consumed the whole file (checked between packets).
	bool
	atEnd();
};

struct DataRecorder
{
private: // Fields
	DataSerializer serializer;

	/*!
	 * Serializes writes to @ref serializer. Every camera has a fast processing thread of its own and
	 * records from it, so without this two cameras' packets interleave and the file is unreadable
	 * from the first race onwards.
	 */
	std::mutex lock;

public: // Methods
	DataRecorder(ConstellationTracker *tracker, std::string out_file);

	//! A recorder without a tracker, for writing datasets offline (synthetic recordings, conversions).
	DataRecorder(std::string out_file, const std::vector<std::vector<t_camera_calibration>> &mosaics);

	~DataRecorder() = default;

	void
	recordSample(const CameraSample &sample);

	void
	recordDeviceInfo(const Device &device);

	void
	recordDeviceInfo(t_constellation_device_id_t device_id, const t_constellation_tracker_led_model &led_model);

	void
	recordImuSample(t_constellation_device_id_t device_id, const xrt_imu_sample &sample);

	void
	recordDeviceTracking(const CameraSample &sample,
	                     t_constellation_device_id_t device_id,
	                     const xrt_space_relation &relation);

	//! Write one extension record (packet 5): its kind, then the payload with its length, so readers can skip it.
	void
	recordExtension(uint32_t kind, const std::vector<uint8_t> &payload);

	void
	recordSessionInfo(const std::string &json);
	void
	recordSyncEvent(const struct DatasetSyncEvent &event);
	void
	recordImuTiming(const struct DatasetImuTiming &timing);
	void
	recordHeadPose(const struct DatasetHeadPose &pose);
	void
	recordGroundTruth(const struct DatasetGroundTruth &truth);
	void
	recordAnnotation(const struct DatasetAnnotation &annotation);
};

/*!
 * Kinds of extension record (packet 5). Each is length-prefixed, so a reader skips kinds it does not know and the
 * format can grow without breaking older readers of newer files. Readers from before packet 5 stop at the first one.
 */
enum DatasetExtensionKind : uint32_t
{
	//! UTF-8 JSON describing the session: tool, versions, settings, notes.
	DATASET_EXTENSION_SESSION_INFO = 1,
	//! @ref DatasetSyncEvent
	DATASET_EXTENSION_SYNC_EVENT = 2,
	//! @ref DatasetImuTiming, one per IMU sample (packet 4), in the same order.
	DATASET_EXTENSION_IMU_TIMING = 3,
	//! @ref DatasetHeadPose, one per tracking-origin query.
	DATASET_EXTENSION_HEAD_POSE = 4,
	//! @ref DatasetGroundTruth
	DATASET_EXTENSION_GROUND_TRUTH = 5,
	//! @ref DatasetAnnotation
	DATASET_EXTENSION_ANNOTATION = 6,
};

//! A timing or synchronisation event from a device driver, see @ref t_constellation_sync_event_kind.
struct DatasetSyncEvent
{
	t_constellation_device_id_t device_id;
	int64_t host_ns;
	uint32_t kind;
	double value[3];
};

//! How an IMU sample's host time was derived, and what the driver had already subtracted from it.
struct DatasetImuTiming
{
	t_constellation_device_id_t device_id;
	//! The host time of the matching packet 4 sample.
	int64_t host_ns;
	//! The sample's own device time, unwrapped.
	int64_t device_ns;
	//! The device-to-host clock offset applied, host = device - offset.
	double clock_offset_ns;
	//! Online gyro bias already subtracted from the packet 4 gyro (rad/s, IMU frame).
	double applied_gyro_bias[3];
};

//! The head pose a tracking-origin query returned, and where it came from.
struct DatasetHeadPose
{
	//! The time the pose was queried for (a camera exposure).
	int64_t timestamp_ns;
	xrt_space_relation_flags relation_flags;
	xrt_pose Txr_world_head;
	//! Host time of the newest SLAM pose the head pose was derived from.
	int64_t source_ns;
	//! @ref t_constellation_head_pose_source_flags
	uint32_t source_flags;
};

//! An externally known device pose, in the tracking world the camera poses use.
struct DatasetGroundTruth
{
	t_constellation_device_id_t device_id;
	int64_t timestamp_ns;
	xrt_pose Txr_world_device;
	float position_sigma_m;
	float orientation_sigma_rad;
	//! @ref t_constellation_ground_truth_flags
	uint32_t flags;
};

//! A free-text marker, such as the start of a scripted motion or a controller set down on a fixture.
struct DatasetAnnotation
{
	//! @ref XRT_CONSTELLATION_INVALID_DEVICE_ID when not about one device.
	t_constellation_device_id_t device_id;
	int64_t host_ns;
	std::string text;
};

struct DatasetMosaic
{
	std::vector<t_camera_calibration> camera_calibrations;
};

struct DatasetDevice
{
	t_constellation_device_id_t id;

	std::vector<t_constellation_tracker_led> leds;
	t_constellation_tracker_led_model led_model;
};

struct DatasetDeviceTracking
{
	uint64_t sample_id;
	int64_t timestamp_ns;
	uint32_t mosaic_index;
	uint32_t camera_index;
	t_constellation_device_id_t device_id;
	xrt_space_relation_flags relation_flags;
	xrt_pose pose;
};

struct DatasetImuSample
{
	t_constellation_device_id_t device_id;
	xrt_imu_sample sample;
};

struct DatasetReader
{
private: // Fields
	DataSerializer serializer;

public: // Fields
	std::vector<DatasetMosaic> mosaics;
	std::vector<DatasetDevice> devices;

	std::vector<CameraSample> samples;
	std::vector<DatasetDeviceTracking> device_tracking;
	std::vector<DatasetImuSample> imu_samples;

	// Extension records (packet 5).
	std::vector<std::string> session_info;
	std::vector<DatasetSyncEvent> sync_events;
	std::vector<DatasetImuTiming> imu_timing;
	std::vector<DatasetHeadPose> head_poses;
	std::vector<DatasetGroundTruth> ground_truth;
	std::vector<DatasetAnnotation> annotations;
	//! Extension records of kinds this reader does not know, skipped.
	size_t unknown_extensions{0};

	//! Why reading stopped: empty at a clean end of file; anything else means the file is truncated or corrupt.
	std::string stop_reason;

public: // Methods
	//! Legacy untagged upstream recordings require upstream_distortion_codes=true.
	DatasetReader(std::string filename, bool upstream_distortion_codes = false);
};

}; // namespace xrt::tracking::constellation

// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the data recorder logic for the constellation tracker.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup tracking
 */

#include "xrt/xrt_byte_order.h"

#include "t_constellation_tracker_dataset.hpp"

#include <cstring>
#include <string>


namespace xrt::tracking::constellation {

#define CT_DATA_MAGIC_LEGACY_VALUE 0x066A4C50
#define CT_DATA_MAGIC_VALUE 0x066A4C51 // Tagged fixed distortion codes, independent of upstream enum order.

enum PacketType
{
	PACKET_TYPE_CAMERA_SAMPLE = 1,
	PACKET_TYPE_DEVICE_INFO = 2,
	/*!
	 * A device's tracking-source relation at a camera sample's exposure, including orientation-only relations
	 * that @ref DeviceState::Txr_world_device_prior leaves out. Needed to replay IMU gravity priors.
	 */
	PACKET_TYPE_DEVICE_TRACKING = 3,
	//! One IMU sample as the device pushed it to the tracker's IMU sink (host time, device IMU frame).
	PACKET_TYPE_IMU_SAMPLE = 4,
	//! An extension record: u32 kind, u32 payload length, payload. See @ref DatasetExtensionKind.
	PACKET_TYPE_EXTENSION = 5,
};

//! Extension payloads larger than this are treated as corruption.
constexpr uint32_t kMaxExtensionPayload = 1u << 20;

namespace {

	void
	throw_if_stream_failed(std::fstream &file, const char *operation)
	{
		if (!file) {
			throw std::runtime_error(operation);
		}
	}

	//! Little-endian encoding of extension payloads, matching DataSerializer.
	struct PayloadWriter
	{
		std::vector<uint8_t> bytes;

		void
		u8(uint8_t v)
		{
			bytes.push_back(v);
		}
		void
		u32(uint32_t v)
		{
			for (int i = 0; i < 4; i++) {
				bytes.push_back((uint8_t)(v >> (8 * i)));
			}
		}
		void
		u64(uint64_t v)
		{
			for (int i = 0; i < 8; i++) {
				bytes.push_back((uint8_t)(v >> (8 * i)));
			}
		}
		void
		i64(int64_t v)
		{
			u64((uint64_t)v);
		}
		void
		f32(float v)
		{
			uint32_t bits;
			std::memcpy(&bits, &v, sizeof(bits));
			u32(bits);
		}
		void
		f64(double v)
		{
			uint64_t bits;
			std::memcpy(&bits, &v, sizeof(bits));
			u64(bits);
		}
		void
		pose(const xrt_pose &p)
		{
			f32(p.position.x);
			f32(p.position.y);
			f32(p.position.z);
			f32(p.orientation.x);
			f32(p.orientation.y);
			f32(p.orientation.z);
			f32(p.orientation.w);
		}
		void
		string(const std::string &text)
		{
			u32((uint32_t)text.size());
			bytes.insert(bytes.end(), text.begin(), text.end());
		}
	};

	struct PayloadReader
	{
		const std::vector<uint8_t> &bytes;
		size_t at{0};

		void
		need(size_t n)
		{
			if (at + n > bytes.size()) {
				throw std::runtime_error("Truncated extension record in dataset file.");
			}
		}
		uint8_t
		u8()
		{
			need(1);
			return bytes[at++];
		}
		uint32_t
		u32()
		{
			need(4);
			uint32_t v = 0;
			for (int i = 0; i < 4; i++) {
				v |= (uint32_t)bytes[at++] << (8 * i);
			}
			return v;
		}
		uint64_t
		u64()
		{
			need(8);
			uint64_t v = 0;
			for (int i = 0; i < 8; i++) {
				v |= (uint64_t)bytes[at++] << (8 * i);
			}
			return v;
		}
		int64_t
		i64()
		{
			return (int64_t)u64();
		}
		float
		f32()
		{
			uint32_t bits = u32();
			float v;
			std::memcpy(&v, &bits, sizeof(v));
			return v;
		}
		double
		f64()
		{
			uint64_t bits = u64();
			double v;
			std::memcpy(&v, &bits, sizeof(v));
			return v;
		}
		xrt_pose
		pose()
		{
			xrt_pose p;
			p.position.x = f32();
			p.position.y = f32();
			p.position.z = f32();
			p.orientation.x = f32();
			p.orientation.y = f32();
			p.orientation.z = f32();
			p.orientation.w = f32();
			return p;
		}
		std::string
		string()
		{
			uint32_t n = u32();
			need(n);
			std::string text(bytes.begin() + (ptrdiff_t)at, bytes.begin() + (ptrdiff_t)(at + n));
			at += n;
			return text;
		}
	};

} // namespace

/*
 *
 * DataSerializer implementations
 *
 */

DataSerializer::DataSerializer(std::string file, bool write)
{
	this->file = std::fstream(file, std::ios::binary | (write ? std::ios::out : std::ios::in));
	if (!this->file.is_open()) {
		throw std::runtime_error("Failed to open file for data serialization.");
	}
}

DataSerializer::~DataSerializer()
{
	if (this->file.is_open()) {
		this->file.close();
	}
}

// uint8_t

void
DataSerializer::write(uint8_t value)
{
	this->file.write(reinterpret_cast<const char *>(&value), sizeof(value));
	throw_if_stream_failed(this->file, "Failed to write dataset file.");
}

bool
DataSerializer::atEnd()
{
	return this->file.peek() == std::char_traits<char>::eof();
}

void
DataSerializer::read(uint8_t &value)
{
	this->file.read(reinterpret_cast<char *>(&value), sizeof(value));
	throw_if_stream_failed(this->file, "Failed to read dataset file.");
}

// uint32_t

void
DataSerializer::write(uint32_t value)
{
	__le32 le_value = __cpu_to_le32(value);
	this->file.write(reinterpret_cast<const char *>(&le_value), sizeof(le_value));
	throw_if_stream_failed(this->file, "Failed to write dataset file.");
}

void
DataSerializer::read(uint32_t &value)
{
	__le32 le_value;
	this->file.read(reinterpret_cast<char *>(&le_value), sizeof(le_value));
	throw_if_stream_failed(this->file, "Failed to read dataset file.");
	value = __le32_to_cpu(le_value);
}

// float

void
DataSerializer::write(float value)
{
	__lef32 le_value = __cpu_to_lef32(value);
	this->file.write(reinterpret_cast<const char *>(&le_value), sizeof(le_value));
	throw_if_stream_failed(this->file, "Failed to write dataset file.");
}

void
DataSerializer::read(float &value)
{
	__lef32 le_value;
	this->file.read(reinterpret_cast<char *>(&le_value), sizeof(le_value));
	throw_if_stream_failed(this->file, "Failed to read dataset file.");
	value = __lef32_to_cpu(le_value);
}

// uint64_t

void
DataSerializer::write(uint64_t value)
{
	__le64 le_value = __cpu_to_le64(value);
	this->file.write(reinterpret_cast<const char *>(&le_value), sizeof(le_value));
	throw_if_stream_failed(this->file, "Failed to write dataset file.");
}

void
DataSerializer::read(uint64_t &value)
{
	__le64 le_value;
	this->file.read(reinterpret_cast<char *>(&le_value), sizeof(le_value));
	throw_if_stream_failed(this->file, "Failed to read dataset file.");
	value = __le64_to_cpu(le_value);
}

// double

void
DataSerializer::write(double value)
{
	__lef64 le_value = __cpu_to_lef64(value);
	this->file.write(reinterpret_cast<const char *>(&le_value), sizeof(le_value));
	throw_if_stream_failed(this->file, "Failed to write dataset file.");
}

void
DataSerializer::read(double &value)
{
	__lef64 le_value;
	this->file.read(reinterpret_cast<char *>(&le_value), sizeof(le_value));
	throw_if_stream_failed(this->file, "Failed to read dataset file.");
	value = __lef64_to_cpu(le_value);
}

// xrt_pose

void
DataSerializer::write(const xrt_pose &value)
{
	this->write(value.position.x);
	this->write(value.position.y);
	this->write(value.position.z);
	this->write(value.orientation.x);
	this->write(value.orientation.y);
	this->write(value.orientation.z);
	this->write(value.orientation.w);
}

void
DataSerializer::read(xrt_pose &value)
{
	this->read(value.position.x);
	this->read(value.position.y);
	this->read(value.position.z);
	this->read(value.orientation.x);
	this->read(value.orientation.y);
	this->read(value.orientation.z);
	this->read(value.orientation.w);
}

// FoundDevicePose

void
DataSerializer::write(const FoundDevicePose &value)
{
	this->write(value.Tcv_cam_device);
}

void
DataSerializer::read(FoundDevicePose &value)
{
	this->read(value.Tcv_cam_device);
}

// DeviceState

void
DataSerializer::write(const DeviceState &value)
{
	this->write(static_cast<uint8_t>(value.device_id));

	this->write(value.Txr_world_device_prior);
	this->write(value.found_pose);
}

void
DataSerializer::read(DeviceState &value)
{
	uint8_t device_id;
	this->read(device_id);
	value.device_id = static_cast<t_constellation_device_id_t>(device_id);

	this->read(value.Txr_world_device_prior);
	this->read(value.found_pose);
}

// t_blob

void
DataSerializer::write(const t_blob &value)
{
	this->write(value.blob_id);
	this->write(static_cast<uint8_t>(value.matched_device_id));
	this->write(static_cast<uint8_t>(value.matched_device_led_id));
	this->write(value.center.x);
	this->write(value.center.y);
	this->write(value.motion_vector.x);
	this->write(value.motion_vector.y);
	this->write(static_cast<uint32_t>(value.bounding_box.offset.w));
	this->write(static_cast<uint32_t>(value.bounding_box.offset.h));
	this->write(static_cast<uint32_t>(value.bounding_box.extent.w));
	this->write(static_cast<uint32_t>(value.bounding_box.extent.h));
	this->write(value.size.x);
	this->write(value.size.y);
	this->write(value.brightness);
}

void
DataSerializer::read(t_blob &value)
{
	this->read(value.blob_id);

	uint8_t matched_device_id;
	this->read(matched_device_id);
	value.matched_device_id = static_cast<t_constellation_device_id_t>(matched_device_id);

	uint8_t matched_device_led_id;
	this->read(matched_device_led_id);
	value.matched_device_led_id = static_cast<t_constellation_device_id_t>(matched_device_led_id);

	this->read(value.center.x);
	this->read(value.center.y);
	this->read(value.motion_vector.x);
	this->read(value.motion_vector.y);

	uint32_t bounding_box_offset_w;
	this->read(bounding_box_offset_w);
	value.bounding_box.offset.w = static_cast<int>(bounding_box_offset_w);

	uint32_t bounding_box_offset_h;
	this->read(bounding_box_offset_h);
	value.bounding_box.offset.h = static_cast<int>(bounding_box_offset_h);

	uint32_t bounding_box_extent_w;
	this->read(bounding_box_extent_w);
	value.bounding_box.extent.w = static_cast<int>(bounding_box_extent_w);

	uint32_t bounding_box_extent_h;
	this->read(bounding_box_extent_h);
	value.bounding_box.extent.h = static_cast<int>(bounding_box_extent_h);

	this->read(value.size.x);
	this->read(value.size.y);
	this->read(value.brightness);
}

// CameraSample

void
DataSerializer::write(const CameraSample &value)
{
	this->write(static_cast<uint64_t>(value.id));
	this->write(static_cast<uint64_t>(value.timestamp_ns));

	this->write(static_cast<uint32_t>(value.blob_count));
	for (size_t i = 0; i < value.blob_count; i++) {
		this->write(value.blobs[i]);
	}

	this->write(value.Txr_world_cam);

	this->write(value.device_count);
	for (size_t i = 0; i < value.device_count; i++) {
		this->write(value.device_states[i]);
	}

	this->write(value.mosaic_index);
	this->write(value.camera_index);
}

void
DataSerializer::read(CameraSample &value)
{
	this->read(value.id);

	uint64_t timestamp_ns;
	this->read(timestamp_ns);
	value.timestamp_ns = timestamp_ns;

	this->read(value.blob_count);
	if (value.blob_count > ARRAY_SIZE(value.blobs)) {
		throw std::runtime_error("Dataset sample blob_count exceeds CameraSample storage.");
	}

	for (size_t i = 0; i < value.blob_count; i++) {
		this->read(value.blobs[i]);
	}

	this->read(value.Txr_world_cam);

	uint32_t device_count;
	this->read(device_count);
	value.device_count = device_count;

	if (device_count > value.device_states.max_size()) {
		throw std::runtime_error("Dataset sample device_count exceeds CameraSample storage.");
	}

	for (size_t i = 0; i < value.device_count; i++) {
		this->read(value.device_states[i]);
	}

	this->read(value.mosaic_index);
	this->read(value.camera_index);
}

// t_camera_calibration

/*!
 * Distortion models are stored as fixed codes rather than as the enum's value, which differs between trees
 * (upstream inserted T_DISTORTION_PINHOLE at the front). The codes follow the enum's original order, so files
 * written before this mapping read unchanged. New models take new codes at the end.
 */
static uint8_t
distortion_model_to_file(t_camera_distortion_model model)
{
	switch (model) {
	case T_DISTORTION_OPENCV_RADTAN_5: return 0;
	case T_DISTORTION_OPENCV_RADTAN_8: return 1;
	case T_DISTORTION_OPENCV_RADTAN_14: return 2;
	case T_DISTORTION_FISHEYE_KB4: return 3;
	case T_DISTORTION_WMR: return 4;
	case T_DISTORTION_PINHOLE: return 5;
	case T_DISTORTION_RIFT_CV1: return 6;
	default: throw std::runtime_error("Distortion model has no dataset code: " + std::to_string((int)model));
	}
}

static t_camera_distortion_model
distortion_model_from_file(uint8_t code)
{
	switch (code) {
	case 0: return T_DISTORTION_OPENCV_RADTAN_5;
	case 1: return T_DISTORTION_OPENCV_RADTAN_8;
	case 2: return T_DISTORTION_OPENCV_RADTAN_14;
	case 3: return T_DISTORTION_FISHEYE_KB4;
	case 4: return T_DISTORTION_WMR;
	case 5: return T_DISTORTION_PINHOLE;
	case 6: return T_DISTORTION_RIFT_CV1;
	default: throw std::runtime_error("Unknown distortion model code in dataset: " + std::to_string(code));
	}
}

void
DataSerializer::write(const t_camera_calibration &value)
{
	this->write(static_cast<uint32_t>(value.image_size_pixels.w));
	this->write(static_cast<uint32_t>(value.image_size_pixels.h));

	// @todo remove when clang-format is updated in CI
	// clang-format off
	double *intrinsics_as_array = reinterpret_cast<double *>(const_cast<double (*)[3][3]>(&value.intrinsics));
	// clang-format on

	for (size_t i = 0; i < 9; i++) {
		this->write(intrinsics_as_array[i]);
	}

	for (size_t i = 0; i < ARRAY_SIZE(value.distortion_parameters_as_array); i++) {
		this->write(value.distortion_parameters_as_array[i]);
	}

	this->write(distortion_model_to_file(value.distortion_model));
}

void
DataSerializer::read(t_camera_calibration &value)
{
	uint32_t image_size_w;
	this->read(image_size_w);
	value.image_size_pixels.w = static_cast<int>(image_size_w);

	uint32_t image_size_h;
	this->read(image_size_h);
	value.image_size_pixels.h = static_cast<int>(image_size_h);

	// @todo remove when clang-format is updated in CI
	// clang-format off
	double *intrinsics_as_array = reinterpret_cast<double *>(const_cast<double (*)[3][3]>(&value.intrinsics));
	// clang-format on

	for (size_t i = 0; i < 9; i++) {
		this->read(intrinsics_as_array[i]);
	}

	for (size_t i = 0; i < ARRAY_SIZE(value.distortion_parameters_as_array); i++) {
		this->read(value.distortion_parameters_as_array[i]);
	}

	uint8_t distortion_model;
	this->read(distortion_model);
	if (this->upstream_distortion_codes) {
		value.distortion_model = static_cast<t_camera_distortion_model>(distortion_model);
		// Validate raw upstream codes using the same exhaustive model mapping.
		(void)distortion_model_to_file(value.distortion_model);
	} else {
		value.distortion_model = distortion_model_from_file(distortion_model);
	}
}

// t_constellation_tracker_led

void
DataSerializer::write(t_constellation_tracker_led &value)
{
	this->write(value.position.x);
	this->write(value.position.y);
	this->write(value.position.z);

	this->write(value.normal.x);
	this->write(value.normal.y);
	this->write(value.normal.z);

	this->write(value.radius_m);
	this->write(value.visibility_angle);

	this->write(static_cast<uint8_t>(value.id));
}

void
DataSerializer::read(t_constellation_tracker_led &value)
{
	this->read(value.position.x);
	this->read(value.position.y);
	this->read(value.position.z);

	this->read(value.normal.x);
	this->read(value.normal.y);
	this->read(value.normal.z);

	this->read(value.radius_m);
	this->read(value.visibility_angle);

	uint8_t id;
	this->read(id);
	value.id = static_cast<t_constellation_led_id_it>(id);
}

// t_constellation_tracker_led_model

void
DataSerializer::write(const t_constellation_tracker_led_model &value)
{
	this->write(static_cast<uint32_t>(value.led_count));
	for (size_t i = 0; i < value.led_count; i++) {
		this->write(value.leds[i]);
	}
}

void
DataSerializer::read(std::vector<t_constellation_tracker_led> &led_storage, t_constellation_tracker_led_model &value)
{
	uint32_t led_count;
	this->read(led_count);
	value.led_count = led_count;

	led_storage.resize(led_count);
	value.leds = led_storage.data();

	for (size_t i = 0; i < value.led_count; i++) {
		this->read(value.leds[i]);
	}
}

// Flush

void
DataSerializer::flush()
{
	this->file.flush();
}

/*
 *
 * DataRecorder implementations
 *
 */

DataRecorder::DataRecorder(ConstellationTracker *tracker, std::string out_file) : serializer(out_file, true)
{
	// Magic value to identify the file so we can prevent loading invalid files. Written in BE to preserve the
	// match with the source code.
	this->serializer.write(static_cast<uint32_t>(__be32_to_cpu(CT_DATA_MAGIC_VALUE)));

	this->serializer.write(static_cast<uint32_t>(tracker->mosaics.size()));
	for (auto &mosaic : tracker->mosaics) {
		this->serializer.write(static_cast<uint32_t>(mosaic->cameras.size()));
		for (auto &camera : mosaic->cameras) {
			this->serializer.write(camera->calibration);
		}
	}

	this->serializer.flush();
}

DataRecorder::DataRecorder(std::string out_file, const std::vector<std::vector<t_camera_calibration>> &mosaics)
    : serializer(out_file, true)
{
	this->serializer.write(static_cast<uint32_t>(__be32_to_cpu(CT_DATA_MAGIC_VALUE)));

	this->serializer.write(static_cast<uint32_t>(mosaics.size()));
	for (const std::vector<t_camera_calibration> &cameras : mosaics) {
		this->serializer.write(static_cast<uint32_t>(cameras.size()));
		for (const t_camera_calibration &calibration : cameras) {
			this->serializer.write(calibration);
		}
	}

	this->serializer.flush();
}

void
DataRecorder::recordSample(const CameraSample &sample)
{
	std::lock_guard<std::mutex> guard(this->lock);

	this->serializer.write(static_cast<uint8_t>(PACKET_TYPE_CAMERA_SAMPLE));
	this->serializer.write(sample);
	this->serializer.flush();
}

void
DataRecorder::recordDeviceTracking(const CameraSample &sample,
                                   t_constellation_device_id_t device_id,
                                   const xrt_space_relation &relation)
{
	std::lock_guard<std::mutex> guard(this->lock);

	this->serializer.write(static_cast<uint8_t>(PACKET_TYPE_DEVICE_TRACKING));
	this->serializer.write(static_cast<uint64_t>(sample.id));
	this->serializer.write(static_cast<uint64_t>(sample.timestamp_ns));
	this->serializer.write(static_cast<uint32_t>(sample.mosaic_index));
	this->serializer.write(static_cast<uint32_t>(sample.camera_index));
	this->serializer.write(static_cast<uint8_t>(device_id));
	this->serializer.write(static_cast<uint32_t>(relation.relation_flags));
	this->serializer.write(relation.pose);
	this->serializer.flush();
}

void
DataRecorder::recordImuSample(t_constellation_device_id_t device_id, const xrt_imu_sample &sample)
{
	std::lock_guard<std::mutex> guard(this->lock);

	this->serializer.write(static_cast<uint8_t>(PACKET_TYPE_IMU_SAMPLE));
	this->serializer.write(static_cast<uint8_t>(device_id));
	this->serializer.write(static_cast<uint64_t>(sample.timestamp_ns));
	this->serializer.write(sample.accel_m_s2.x);
	this->serializer.write(sample.accel_m_s2.y);
	this->serializer.write(sample.accel_m_s2.z);
	this->serializer.write(sample.gyro_rad_secs.x);
	this->serializer.write(sample.gyro_rad_secs.y);
	this->serializer.write(sample.gyro_rad_secs.z);
	this->serializer.flush();
}

void
DataRecorder::recordDeviceInfo(const Device &device)
{
	this->recordDeviceInfo(device.id, device.params.led_model);
}

void
DataRecorder::recordDeviceInfo(t_constellation_device_id_t device_id,
                               const t_constellation_tracker_led_model &led_model)
{
	std::lock_guard<std::mutex> guard(this->lock);

	this->serializer.write(static_cast<uint8_t>(PACKET_TYPE_DEVICE_INFO));
	this->serializer.write(static_cast<uint8_t>(device_id));
	this->serializer.write(led_model);
	this->serializer.flush();
}

void
DataRecorder::recordExtension(uint32_t kind, const std::vector<uint8_t> &payload)
{
	std::lock_guard<std::mutex> guard(this->lock);

	this->serializer.write(static_cast<uint8_t>(PACKET_TYPE_EXTENSION));
	this->serializer.write(kind);
	this->serializer.write(static_cast<uint32_t>(payload.size()));
	for (uint8_t byte : payload) {
		this->serializer.write(byte);
	}
	this->serializer.flush();
}

void
DataRecorder::recordSessionInfo(const std::string &json)
{
	PayloadWriter w;
	w.string(json);
	this->recordExtension(DATASET_EXTENSION_SESSION_INFO, w.bytes);
}

void
DataRecorder::recordSyncEvent(const DatasetSyncEvent &event)
{
	PayloadWriter w;
	w.u8(static_cast<uint8_t>(event.device_id));
	w.i64(event.host_ns);
	w.u32(event.kind);
	for (double v : event.value) {
		w.f64(v);
	}
	this->recordExtension(DATASET_EXTENSION_SYNC_EVENT, w.bytes);
}

void
DataRecorder::recordImuTiming(const DatasetImuTiming &timing)
{
	PayloadWriter w;
	w.u8(static_cast<uint8_t>(timing.device_id));
	w.i64(timing.host_ns);
	w.i64(timing.device_ns);
	w.f64(timing.clock_offset_ns);
	for (double v : timing.applied_gyro_bias) {
		w.f64(v);
	}
	this->recordExtension(DATASET_EXTENSION_IMU_TIMING, w.bytes);
}

void
DataRecorder::recordHeadPose(const DatasetHeadPose &pose)
{
	PayloadWriter w;
	w.i64(pose.timestamp_ns);
	w.u32(static_cast<uint32_t>(pose.relation_flags));
	w.pose(pose.Txr_world_head);
	w.i64(pose.source_ns);
	w.u32(pose.source_flags);
	this->recordExtension(DATASET_EXTENSION_HEAD_POSE, w.bytes);
}

void
DataRecorder::recordGroundTruth(const DatasetGroundTruth &truth)
{
	PayloadWriter w;
	w.u8(static_cast<uint8_t>(truth.device_id));
	w.i64(truth.timestamp_ns);
	w.pose(truth.Txr_world_device);
	w.f32(truth.position_sigma_m);
	w.f32(truth.orientation_sigma_rad);
	w.u32(truth.flags);
	this->recordExtension(DATASET_EXTENSION_GROUND_TRUTH, w.bytes);
}

void
DataRecorder::recordAnnotation(const DatasetAnnotation &annotation)
{
	PayloadWriter w;
	w.u8(static_cast<uint8_t>(annotation.device_id));
	w.i64(annotation.host_ns);
	w.string(annotation.text);
	this->recordExtension(DATASET_EXTENSION_ANNOTATION, w.bytes);
}

/*
 *
 * DatasetReader implementations
 *
 */

DatasetReader::DatasetReader(std::string filename, bool upstream_distortion_codes) : serializer(filename, false)
{
	uint32_t magic_value;
	this->serializer.read(magic_value);
	bool legacy = magic_value == static_cast<uint32_t>(__be32_to_cpu(CT_DATA_MAGIC_LEGACY_VALUE));
	if (legacy) {
		this->serializer.upstream_distortion_codes = upstream_distortion_codes;
	} else if (upstream_distortion_codes) {
		throw std::runtime_error("--upstream-distortion-codes is only valid for legacy untagged datasets");
	}
	if (!legacy && magic_value != static_cast<uint32_t>(__be32_to_cpu(CT_DATA_MAGIC_VALUE))) {
		throw std::runtime_error(
		    "Invalid magic value in dataset file, not a valid constellation tracker dataset.");
	}

	uint32_t mosaic_count;
	this->serializer.read(mosaic_count);

	for (size_t i = 0; i < mosaic_count; i++) {
		DatasetMosaic mosaic;

		uint32_t camera_count;
		this->serializer.read(camera_count);

		for (size_t j = 0; j < camera_count; j++) {
			t_camera_calibration calibration;
			this->serializer.read(calibration);
			mosaic.camera_calibrations.push_back(calibration);
		}

		this->mosaics.push_back(mosaic);
	}

	while (!this->serializer.atEnd()) {
		try {
			uint8_t packet_type;
			this->serializer.read(packet_type);
			switch (packet_type) {
			case PACKET_TYPE_CAMERA_SAMPLE: {
				CameraSample sample;
				this->serializer.read(sample);
				this->samples.push_back(sample);
				break;
			}
			case PACKET_TYPE_DEVICE_INFO: {
				DatasetDevice &device = this->devices.emplace_back();

				uint8_t device_id;
				this->serializer.read(device_id);
				device.id = static_cast<t_constellation_device_id_t>(device_id);

				this->serializer.read(device.leds, device.led_model);

				break;
			}
			case PACKET_TYPE_DEVICE_TRACKING: {
				DatasetDeviceTracking &tracking = this->device_tracking.emplace_back();
				uint64_t timestamp_ns;
				uint8_t device_id;
				uint32_t flags;
				this->serializer.read(tracking.sample_id);
				this->serializer.read(timestamp_ns);
				this->serializer.read(tracking.mosaic_index);
				this->serializer.read(tracking.camera_index);
				this->serializer.read(device_id);
				this->serializer.read(flags);
				this->serializer.read(tracking.pose);
				tracking.timestamp_ns = static_cast<int64_t>(timestamp_ns);
				tracking.device_id = static_cast<t_constellation_device_id_t>(device_id);
				tracking.relation_flags = static_cast<xrt_space_relation_flags>(flags);
				break;
			}
			case PACKET_TYPE_IMU_SAMPLE: {
				DatasetImuSample &imu = this->imu_samples.emplace_back();
				uint8_t device_id;
				uint64_t timestamp_ns;
				this->serializer.read(device_id);
				this->serializer.read(timestamp_ns);
				this->serializer.read(imu.sample.accel_m_s2.x);
				this->serializer.read(imu.sample.accel_m_s2.y);
				this->serializer.read(imu.sample.accel_m_s2.z);
				this->serializer.read(imu.sample.gyro_rad_secs.x);
				this->serializer.read(imu.sample.gyro_rad_secs.y);
				this->serializer.read(imu.sample.gyro_rad_secs.z);
				imu.device_id = static_cast<t_constellation_device_id_t>(device_id);
				imu.sample.timestamp_ns = static_cast<timepoint_ns>(timestamp_ns);
				break;
			}
			case PACKET_TYPE_EXTENSION: {
				uint32_t kind;
				uint32_t length;
				this->serializer.read(kind);
				this->serializer.read(length);
				if (length > kMaxExtensionPayload) {
					throw std::runtime_error("Oversized extension record in dataset file.");
				}
				std::vector<uint8_t> payload(length);
				for (uint8_t &byte : payload) {
					this->serializer.read(byte);
				}
				PayloadReader r{payload};
				switch (kind) {
				case DATASET_EXTENSION_SESSION_INFO: this->session_info.push_back(r.string()); break;
				case DATASET_EXTENSION_SYNC_EVENT: {
					DatasetSyncEvent &e = this->sync_events.emplace_back();
					e.device_id = static_cast<t_constellation_device_id_t>(r.u8());
					e.host_ns = r.i64();
					e.kind = r.u32();
					for (double &v : e.value) {
						v = r.f64();
					}
					break;
				}
				case DATASET_EXTENSION_IMU_TIMING: {
					DatasetImuTiming &t = this->imu_timing.emplace_back();
					t.device_id = static_cast<t_constellation_device_id_t>(r.u8());
					t.host_ns = r.i64();
					t.device_ns = r.i64();
					t.clock_offset_ns = r.f64();
					for (double &v : t.applied_gyro_bias) {
						v = r.f64();
					}
					break;
				}
				case DATASET_EXTENSION_HEAD_POSE: {
					DatasetHeadPose &h = this->head_poses.emplace_back();
					h.timestamp_ns = r.i64();
					h.relation_flags = static_cast<xrt_space_relation_flags>(r.u32());
					h.Txr_world_head = r.pose();
					h.source_ns = r.i64();
					h.source_flags = r.u32();
					break;
				}
				case DATASET_EXTENSION_GROUND_TRUTH: {
					DatasetGroundTruth &g = this->ground_truth.emplace_back();
					g.device_id = static_cast<t_constellation_device_id_t>(r.u8());
					g.timestamp_ns = r.i64();
					g.Txr_world_device = r.pose();
					g.position_sigma_m = r.f32();
					g.orientation_sigma_rad = r.f32();
					g.flags = r.u32();
					break;
				}
				case DATASET_EXTENSION_ANNOTATION: {
					DatasetAnnotation &a = this->annotations.emplace_back();
					a.device_id = static_cast<t_constellation_device_id_t>(r.u8());
					a.host_ns = r.i64();
					a.text = r.string();
					break;
				}
				default: this->unknown_extensions++; break;
				}
				break;
			}
			default: {
				throw std::runtime_error("Invalid packet type in dataset file. Got ID " +
				                         std::to_string(packet_type));
			}
			}
		} catch (const std::exception &e) {
			this->stop_reason = e.what();
			break;
		}
	}
}

}; // namespace xrt::tracking::constellation

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Joint multi-camera path of the constellation tracker (CONSTELLATION_TRACKER_JOINT=1).
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "t_constellation_tracker_internal.hpp"
#include "t_constellation_tracker_dataset.hpp"
#include "joint_pose_solver.hpp"
#include "stereo_bootstrap.hpp"

#include "util/u_time.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>

using namespace xrt::tracking::constellation;

namespace {

//! Samples from different cameras of one exposure are this close in time.
constexpr int64_t kExposureToleranceNs = 2'000'000;
//! A device not solved for this long is re-acquired by bootstrap rather than tracked from its last pose.
constexpr int64_t kTrackingTimeoutNs = 250'000'000;
//! Consecutive failed tracking solves before a device counts as lost.
constexpr uint32_t kMaxTrackingFailures = 3;
//! Orientation prior from the device's predicted pose (IMU-propagated), in degrees.
constexpr float kOrientationPriorSigmaDeg = 3.0f;
//! Status log interval.
constexpr int64_t kStatusIntervalNs = 5'000'000'000;

bool
relation_has(const xrt_space_relation &relation, uint32_t flags)
{
	return (relation.relation_flags & flags) == flags;
}

} // namespace

extern "C" void *
constellation_tracker_joint_thread(void *ptr)
{
	JointProcessor *joint = (JointProcessor *)ptr;

	os_thread_helper_lock(&joint->thread);
	while (os_thread_helper_is_running_locked(&joint->thread)) {
		if (!joint->ready.has_value()) {
			os_thread_helper_wait_locked(&joint->thread);
			continue;
		}
		JointExposure exposure = std::move(*joint->ready);
		joint->ready.reset();
		os_thread_helper_unlock(&joint->thread);

		joint->process(exposure);

		os_thread_helper_lock(&joint->thread);
	}
	os_thread_helper_unlock(&joint->thread);
	return nullptr;
}

JointProcessor::JointProcessor(ConstellationTracker *tracker, size_t camera_count)
    : tracker(tracker), camera_count(camera_count)
{
	if (os_thread_helper_init(&this->thread) < 0) {
		throw std::runtime_error("Joint processing thread failed to init");
	}
	if (!tracker->single_threaded &&
	    os_thread_helper_start(&this->thread, constellation_tracker_joint_thread, this) < 0) {
		throw std::runtime_error("Starting joint processing thread failed");
	}
}

JointProcessor::~JointProcessor()
{
	if (this->thread.initialized) {
		os_thread_helper_destroy(&this->thread);
	}
}

void
JointProcessor::finishBuildingLocked()
{
	if (!this->building.has_value()) {
		return;
	}
	if (this->ready.has_value()) {
		this->exposures_skipped++;
	}
	this->ready = std::move(this->building);
	this->building.reset();
	this->exposures_assembled++;
	os_thread_helper_signal_locked(&this->thread);
}

void
JointProcessor::push(CameraSample &&sample)
{
	// Deterministic (single-threaded) trackers solve closed exposures on the calling thread.
	std::vector<JointExposure> inline_exposures;
	auto close_building = [&]() {
		if (!this->building.has_value()) {
			return;
		}
		if (this->tracker->single_threaded) {
			inline_exposures.push_back(std::move(*this->building));
			this->building.reset();
			this->exposures_assembled++;
		} else {
			this->finishBuildingLocked();
		}
	};

	os_thread_helper_lock(&this->thread);
	if (this->building.has_value() &&
	    std::llabs(sample.timestamp_ns - this->building->timestamp_ns) <= kExposureToleranceNs) {
		// Another camera of the exposure being assembled.
	} else if (this->building.has_value() && sample.timestamp_ns < this->building->timestamp_ns) {
		// Belongs to an exposure that has already been closed.
		this->late_samples++;
		os_thread_helper_unlock(&this->thread);
		return;
	} else {
		// A new exposure: whatever was being assembled is as complete as it will get.
		close_building();
		JointExposure exposure;
		exposure.timestamp_ns = sample.timestamp_ns;
		exposure.samples.resize(this->camera_count);
		this->building = std::move(exposure);
	}

	uint32_t index = sample.camera_index;
	if (index < this->building->samples.size() && !this->building->samples[index].has_value()) {
		this->building->samples[index] = std::move(sample);
		this->building->received++;
	}
	if (this->building->received >= this->camera_count) {
		close_building();
	}
	os_thread_helper_unlock(&this->thread);

	for (JointExposure &exposure : inline_exposures) {
		this->process(exposure);
	}
}

void
JointProcessor::process(JointExposure &exposure)
{
	ConstellationTracker *ct = this->tracker;
	this->processed++;

	// Cameras of this exposure, with blob ownership shared between devices.
	std::vector<CameraSample *> samples;
	std::vector<JointSolveCamera> cameras;
	std::vector<std::vector<t_constellation_device_id_t>> owners;
	std::vector<Camera *> camera_of;
	std::shared_ptr<CameraMosaic> mosaic = ct->mosaics.empty() ? nullptr : ct->mosaics[0];
	for (std::optional<CameraSample> &maybe : exposure.samples) {
		if (!maybe.has_value() || !maybe->Txr_world_cam.has_value() || mosaic == nullptr ||
		    maybe->camera_index >= mosaic->cameras.size()) {
			continue;
		}
		Camera *camera = mosaic->cameras[maybe->camera_index].get();
		xrt_pose Tcv_world_cam;
		math_pose_convert_from_opencv(&maybe->Txr_world_cam.value(), &Tcv_world_cam);
		samples.push_back(&maybe.value());
		camera_of.push_back(camera);
		owners.emplace_back(maybe->blob_count, XRT_CONSTELLATION_INVALID_DEVICE_ID);
		cameras.push_back(JointSolveCamera{Tcv_world_cam, &camera->model.calib, camera->model.width,
		                                   camera->model.height, maybe->blobs, maybe->blob_count, nullptr});
	}
	for (size_t i = 0; i < cameras.size(); i++) {
		cameras[i].blob_owner = owners[i].data();
	}

	std::shared_lock device_lock(ct->device_lock);

	// Devices that are being tracked go first, so a lost device cannot claim a tracked ring's blobs.
	std::vector<Device *> order;
	for (std::unique_ptr<Device> &device : ct->devices) {
		order.push_back(device.get());
	}
	std::stable_sort(order.begin(), order.end(), [this](Device *a, Device *b) {
		return this->devices[a->id].tracking && !this->devices[b->id].tracking;
	});

	for (Device *device : order) {
		JointDeviceState &state = this->devices[device->id];

		xrt_space_relation predicted = XRT_SPACE_RELATION_ZERO;
		if (device->params.tracking_source != nullptr) {
			t_constellation_tracker_tracking_source_get_tracked_pose(device->params.tracking_source,
			                                                         exposure.timestamp_ns, &predicted);
		}
		if (ct->data_recorder && !samples.empty()) {
			ct->data_recorder->recordDeviceTracking(*samples[0], device->id, predicted);
		}
		if (cameras.empty()) {
			continue;
		}

		xrt_pose Tcv_predicted;
		math_pose_convert_from_opencv(&predicted.pose, &Tcv_predicted);
		const bool have_orientation =
		    relation_has(predicted, XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
		const bool have_pose = have_orientation && relation_has(predicted, XRT_SPACE_RELATION_POSITION_VALID_BIT);

		auto start = std::chrono::steady_clock::now();
		JointSolveResult result;
		bool ok = false;
		bool bootstrapped = false;

		if (state.tracking && exposure.timestamp_ns - state.last_solved_ns < kTrackingTimeoutNs) {
			xrt_pose prior = state.Tcv_world_device;
			if (have_pose) {
				prior = Tcv_predicted;
			} else if (have_orientation) {
				prior.orientation = Tcv_predicted.orientation;
			}
			JointSolveParams params;
			if (have_orientation) {
				params.orientation_prior_sigma_deg = kOrientationPriorSigmaDeg;
				ok = joint_solve_refine(cameras, device->params.led_model, prior, prior.orientation, params,
				                        result);
			} else {
				ok = joint_solve_refine(cameras, device->params.led_model, prior, params, result);
			}
		}
		if (!ok) {
			StereoBootstrapResult bootstrap;
			ok = stereo_bootstrap(cameras, device->params.led_model, StereoBootstrapParams{}, bootstrap);
			if (ok) {
				result = bootstrap.refined;
				bootstrapped = true;
			}
		}

		double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
		this->solve_us_total += us;
		this->solve_us_max = std::max(this->solve_us_max, us);

		if (!ok) {
			this->device_failed++;
			if (state.tracking && ++state.consecutive_failures > kMaxTrackingFailures) {
				state.tracking = false;
			}
			continue;
		}

		state.tracking = true;
		state.consecutive_failures = 0;
		state.Tcv_world_device = result.Tcv_world_device;
		state.last_solved_ns = exposure.timestamp_ns;
		if (bootstrapped) {
			this->device_bootstrapped++;
		} else {
			this->device_tracked++;
		}

		float brightness = 0.0f;
		size_t first_camera = SIZE_MAX;
		for (const JointSolveMatch &m : result.correspondences) {
			owners[m.camera][m.blob] = device->id;
			brightness += cameras[m.camera].blobs[m.blob].brightness;
			first_camera = std::min(first_camera, (size_t)camera_of[m.camera]->index);
		}
		brightness = result.correspondences.empty() ? 1.0f : brightness / (float)result.correspondences.size();

		xrt_pose Txr_world_device;
		math_pose_convert_from_opencv(&result.Tcv_world_device, &Txr_world_device);

		t_constellation_tracker_sample sample = {
		    .timestamp_ns = exposure.timestamp_ns,
		    .pose = Txr_world_device,
		    .mosaic_index = mosaic->index,
		    .camera_index = first_camera == SIZE_MAX ? 0 : first_camera,
		    .average_brightness = brightness,
		    .metrics =
		        {
		            .matched_blob_count = result.matches,
		            .visible_led_count = result.visible_leds,
		            .reprojection_error = result.rms_px,
		        },
		    .joint_camera_count = std::max<uint32_t>(result.cameras_used, 1),
		};
		if (t_constellation_tracker_device_push_sample(device->device, &sample)) {
			std::unique_lock<os::Mutex> lock(device->data_lock);
			device->locked_data.last_known_pose = DeviceLastPose(sample.pose, sample.timestamp_ns);
		}
	}

	if (ct->data_recorder) {
		for (CameraSample *sample : samples) {
			ct->data_recorder->recordSample(*sample);
		}
	}

	if (exposure.timestamp_ns - this->last_status_ns >= kStatusIntervalNs) {
		uint64_t assembled, skipped, late;
		os_thread_helper_lock(&this->thread);
		assembled = this->exposures_assembled;
		skipped = this->exposures_skipped;
		late = this->late_samples;
		os_thread_helper_unlock(&this->thread);
		uint64_t solves = this->device_tracked + this->device_bootstrapped + this->device_failed;
		CT_INFO(ct,
		        "JOINT_STATUS exposures=%" PRIu64 " processed=%" PRIu64 " skipped=%" PRIu64 " late_samples=%" PRIu64
		        " tracked=%" PRIu64 " bootstrapped=%" PRIu64 " failed=%" PRIu64 " mean_solve_us=%.0f max_solve_us=%.0f",
		        assembled, this->processed, skipped, late, this->device_tracked, this->device_bootstrapped,
		        this->device_failed, solves ? this->solve_us_total / (double)solves : 0.0, this->solve_us_max);
		this->last_status_ns = exposure.timestamp_ns;
		this->solve_us_max = 0.0;
	}
}

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
#include "oriented_bootstrap.hpp"

#include "util/u_time.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <string>

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
/*!
 * Solves a freshly bootstrapped track needs, in a row, before its poses reach the device. A mirror-image ring can
 * bootstrap just under the limits (in replay the right model fitted the left ring for two frames at 0.80 and 0.93 px)
 * but rarely persists; a real re-acquisition does. Costs two exposures (33 ms) of latency on re-acquisition.
 */
constexpr uint32_t kConfirmSolves = 3;
/*!
 * Coverage a solve anchored to the IMU orientation must reach when oriented bootstrap is enabled. Coverage catches a
 * pose slipped round the ring, which the orientation prior already rules out (the LEDs are ~20 deg apart, the prior
 * 3-4 deg). At 0.8 a partly occluded ring was rejected with 12-21 matches over three or four cameras at 0.2-0.3 px.
 */
constexpr float kOrientedMinCoverage = 0.5f;
//! Status log interval.
constexpr int64_t kStatusIntervalNs = 5'000'000'000;
//! An exposure slower than this is logged (JOINT_SLOW), at most every kSlowLogIntervalNs.
constexpr double kSlowProcessMs = 8.0;
constexpr int64_t kSlowLogIntervalNs = 250'000'000;

//! A loss is logged (JOINT_LOSS) once it lasts this long, the point where the device is re-acquired by bootstrap.
constexpr int64_t kLossLogMinNs = kTrackingTimeoutNs;
//! A camera counts as seeing a lost device's ring with this many free LED-shaped blobs above its background.
constexpr float kLitExcessBlobs = 3.0f;
//! Weight of each all-solved exposure in the free-blob background.
constexpr float kBackgroundAlpha = 0.02f;

bool
relation_has(const xrt_space_relation &relation, uint32_t flags)
{
	return (relation.relation_flags & flags) == flags;
}

/*!
 * Cameras into whose image @p p_world projects. @p out_margin_px gets the largest distance to an image edge among
 * them, or -1 if there are none.
 */
uint32_t
cameras_in_view(const std::vector<JointSolveCamera> &cameras, const xrt_vec3 &p_world, float *out_margin_px)
{
	uint32_t count = 0;
	float best = -1.0f;
	for (const JointSolveCamera &camera : cameras) {
		xrt_pose Tcv_cam_world;
		math_pose_invert(&camera.Tcv_world_cam, &Tcv_cam_world);
		xrt_vec3 p_cam;
		math_pose_transform_point(&Tcv_cam_world, &p_world, &p_cam);
		float u, v;
		if (p_cam.z <= 0.02f || !t_camera_models_project(camera.model, p_cam.x, p_cam.y, p_cam.z, &u, &v)) {
			continue;
		}
		float margin = std::min(std::min(u, v), std::min((float)camera.width - u, (float)camera.height - v));
		if (margin < 0.0f) {
			continue;
		}
		count++;
		best = std::max(best, margin);
	}
	*out_margin_px = best;
	return count;
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

JointProcessor::JointProcessor(ConstellationTracker *tracker, size_t camera_count, bool oriented_bootstrap_enabled)
    : tracker(tracker), camera_count(camera_count), oriented_bootstrap_enabled(oriented_bootstrap_enabled)
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

	// Where the time goes, for JOINT_SLOW: the worker skips exposures whenever one takes longer than a frame.
	using Clock = std::chrono::steady_clock;
	const Clock::time_point process_start = Clock::now();
	double lock_wait_ms = 0.0, predict_ms = 0.0, push_ms = 0.0, led_count_ms = 0.0, record_ms = 0.0;
	auto ms_since = [](Clock::time_point start) {
		return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
	};

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

	Clock::time_point lock_start = Clock::now();
	std::shared_lock device_lock(ct->device_lock);
	lock_wait_ms = ms_since(lock_start);

	// The device's prediction for this exposure, by device (filled in phase 1).
	std::map<t_constellation_device_id_t, xrt_space_relation> predictions;
	// Devices solved in this exposure: true once the track is confirmed, false while still tentative.
	std::map<t_constellation_device_id_t, bool> solved;

	// Accept a solve: claim its blobs, update the device state and push the pose.
	auto commit = [&](Device *device, const JointSolveResult &result, bool bootstrapped) {
		JointDeviceState &state = this->devices[device->id];
		const xrt_space_relation &predicted = predictions[device->id];
		if (relation_has(predicted, XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)) {
			xrt_pose Tcv_predicted;
			math_pose_convert_from_opencv(&predicted.pose, &Tcv_predicted);
			xrt_quat inverse;
			math_quat_invert(&Tcv_predicted.orientation, &inverse);
			math_quat_rotate(&result.Tcv_world_device.orientation, &inverse, &state.align);
			math_quat_normalize(&state.align);
			state.have_align = true;
		}
		state.tracking = true;
		state.consecutive_failures = 0;
		state.confirmations = bootstrapped ? 1 : state.confirmations + 1;
		state.Tcv_world_device = result.Tcv_world_device;
		state.last_solved_ns = exposure.timestamp_ns;
		state.last_cameras_used = result.cameras_used;
		solved[device->id] = state.confirmations >= kConfirmSolves;
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

		// Tentative tracks claim their blobs and keep tracking, but stay private until confirmed.
		if (state.confirmations < kConfirmSolves) {
			this->unconfirmed_dropped++;
			return;
		}

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
		Clock::time_point push_start = Clock::now();
		bool pushed = t_constellation_tracker_device_push_sample(device->device, &sample);
		push_ms += ms_since(push_start);
		if (pushed) {
			std::unique_lock<os::Mutex> lock(device->data_lock);
			device->locked_data.last_known_pose =
			    DeviceLastPose(sample.pose, result.Tcv_world_device, sample.timestamp_ns);
		}
	};

	auto failed = [&](Device *device) {
		JointDeviceState &state = this->devices[device->id];
		this->device_failed++;
		if (state.tracking && ++state.consecutive_failures > kMaxTrackingFailures) {
			state.tracking = false;
		}
		// A tentative track that fails is dropped at once, so its blobs are free for the other models.
		if (state.confirmations < kConfirmSolves) {
			state.tracking = false;
		}
	};

	auto timed = [&](auto &&fn) {
		auto start = std::chrono::steady_clock::now();
		auto value = fn();
		double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
		this->solve_us_total += us;
		this->solve_us_max = std::max(this->solve_us_max, us);
		return value;
	};

	// The device's predicted (IMU) orientation in this world, if it has one and an alignment from earlier solves.
	auto aligned_orientation = [&](t_constellation_device_id_t id, xrt_quat &out) {
		const JointDeviceState &state = this->devices[id];
		const xrt_space_relation &predicted = predictions[id];
		if (!state.have_align || !relation_has(predicted, XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		                                                      XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT)) {
			return false;
		}
		xrt_pose Tcv_predicted;
		math_pose_convert_from_opencv(&predicted.pose, &Tcv_predicted);
		math_quat_rotate(&state.align, &Tcv_predicted.orientation, &out);
		math_quat_normalize(&out);
		return true;
	};

	// Phase 1: devices being tracked refine from their predicted pose and claim their blobs first.
	std::vector<Device *> need_bootstrap;
	for (std::unique_ptr<Device> &owned : ct->devices) {
		Device *device = owned.get();
		JointDeviceState &state = this->devices[device->id];

		xrt_space_relation predicted = XRT_SPACE_RELATION_ZERO;
		if (device->params.tracking_source != nullptr) {
			Clock::time_point predict_start = Clock::now();
			t_constellation_tracker_tracking_source_get_tracked_pose(device->params.tracking_source,
			                                                         exposure.timestamp_ns, &predicted);
			predict_ms += ms_since(predict_start);
		}
		predictions[device->id] = predicted;
		if (ct->data_recorder && !samples.empty()) {
			ct->data_recorder->recordDeviceTracking(*samples[0], device->id, predicted);
		}
		if (cameras.empty()) {
			continue;
		}
		if (!state.tracking || exposure.timestamp_ns - state.last_solved_ns >= kTrackingTimeoutNs) {
			need_bootstrap.push_back(device);
			continue;
		}

		/*
		 * Position comes from the prediction when it has one (it is built on the optical poses pushed so far),
		 * otherwise from the last solve. Orientation is the predicted (IMU) orientation carried into this world
		 * by the alignment from earlier solves: the Sense driver's predicted orientation is its IMU's own
		 * world, 50-110 deg from the optical one, and anchoring to it directly (3-deg prior) stopped live
		 * tracking after every bootstrap.
		 */
		xrt_pose Tcv_predicted;
		math_pose_convert_from_opencv(&predicted.pose, &Tcv_predicted);
		const bool have_position = relation_has(predicted, XRT_SPACE_RELATION_POSITION_VALID_BIT);
		const bool have_orientation =
		    state.have_align && relation_has(predicted, XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		                                                    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);

		xrt_pose prior = state.Tcv_world_device;
		if (have_position) {
			prior.position = Tcv_predicted.position;
		}
		if (have_orientation) {
			math_quat_rotate(&state.align, &Tcv_predicted.orientation, &prior.orientation);
			math_quat_normalize(&prior.orientation);
		}
		JointSolveResult result;
		bool ok = timed([&] {
			JointSolveParams params;
			if (have_orientation) {
				params.orientation_prior_sigma_deg = kOrientationPriorSigmaDeg;
				if (this->oriented_bootstrap_enabled) {
					params.min_coverage = kOrientedMinCoverage;
				}
				return joint_solve_refine(cameras, device->params.led_model, prior, prior.orientation,
				                          params, result);
			}
			return joint_solve_refine(cameras, device->params.led_model, prior, params, result);
		});
		if (ok) {
			commit(device, result, false);
		} else {
			need_bootstrap.push_back(device);
		}
	}

	/*
	 * Phase 2: a contest between everything left. Every candidate bootstraps against the same free blobs and the
	 * best fit wins, claims its blobs, and the rest try again on what remains. Mirror-image rings (left and right
	 * Sense) can each fit the other's blobs just under the acceptance limits; in replay the right model took the
	 * left ring at 0.80 px while the left's tracking had lapsed. The true model always fits its own ring better.
	 */
	while (!need_bootstrap.empty() && !cameras.empty()) {
		int best = -1;
		StereoBootstrapResult best_result;
		bool best_oriented = false;
		for (size_t i = 0; i < need_bootstrap.size(); i++) {
			StereoBootstrapResult result;
			bool oriented = false;
			bool ok = timed([&] {
				return stereo_bootstrap(cameras, need_bootstrap[i]->params.led_model,
				                        StereoBootstrapParams{}, result);
			});
			// A ring in one camera, or partly occluded: re-acquire from the IMU orientation, carried into
			// this world by the alignment from earlier solves.
			xrt_quat orientation;
			if (!ok && this->oriented_bootstrap_enabled &&
			    aligned_orientation(need_bootstrap[i]->id, orientation)) {
				OrientedBootstrapResult oriented_result;
				ok = timed([&] {
					return oriented_bootstrap(cameras, need_bootstrap[i]->params.led_model,
					                          orientation, OrientedBootstrapParams{},
					                          oriented_result);
				});
				if (ok) {
					result.refined = oriented_result.refined;
					oriented = true;
				}
			}
			if (!ok) {
				continue;
			}
			const JointSolveResult &r = result.refined;
			const JointSolveResult &b = best_result.refined;
			// Prefer clearly more matches, then the lower RMS.
			bool better = best < 0 || r.matches > b.matches + 2 ||
			              (r.matches + 2 >= b.matches && r.rms_px < b.rms_px);
			if (better) {
				best = (int)i;
				best_result = result;
				best_oriented = oriented;
			}
		}
		if (best < 0) {
			break;
		}
		if (best_oriented) {
			this->device_oriented++;
			this->devices[need_bootstrap[best]->id].loss.oriented++;
		}
		commit(need_bootstrap[best], best_result.refined, true);
		need_bootstrap.erase(need_bootstrap.begin() + best);
	}
	for (Device *device : need_bootstrap) {
		failed(device);
	}

	/*
	 * Per-device illumination counts, now that every solved device has claimed its blobs: LED-shaped blobs no
	 * other device owns, plus the device's own matches. Another controller's lit ring, lamps and window glare all
	 * drop out, which raw blob counts could not do (25 Sep: a kept-lit right ring inflated the left's baseline).
	 */
	Clock::time_point led_count_start = Clock::now();
	for (std::unique_ptr<Device> &owned : ct->devices) {
		Device *device = owned.get();
		if (device->device->push_camera_led_blob_count == nullptr) {
			continue;
		}
		for (std::optional<CameraSample> &maybe : exposure.samples) {
			if (!maybe.has_value()) {
				continue;
			}
			const t_constellation_device_id_t *owner = nullptr;
			for (size_t i = 0; i < samples.size(); i++) {
				if (samples[i] == &maybe.value()) {
					owner = owners[i].data();
				}
			}
			uint32_t led_blobs = 0;
			uint32_t matched = 0;
			for (uint32_t b = 0; b < maybe->blob_count; b++) {
				t_constellation_device_id_t id = owner ? owner[b] : XRT_CONSTELLATION_INVALID_DEVICE_ID;
				if (id == device->id) {
					matched++;
					led_blobs++;
				} else if (id == XRT_CONSTELLATION_INVALID_DEVICE_ID &&
				           t_constellation_blob_is_led_shaped(maybe->blobs[b])) {
					led_blobs++;
				}
			}
			device->device->push_camera_led_blob_count(device->device, maybe->camera_index,
			                                           maybe->timestamp_ns, led_blobs, matched);
		}
	}

	led_count_ms = ms_since(led_count_start);

	if (!cameras.empty()) {
		accountLosses(exposure.timestamp_ns, cameras, owners, samples, predictions, solved);
	}

	Clock::time_point record_start = Clock::now();
	if (ct->data_recorder) {
		for (CameraSample *sample : samples) {
			ct->data_recorder->recordSample(*sample);
		}
	}
	record_ms = ms_since(record_start);

	const double total_ms = ms_since(process_start);
	const int64_t now_ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
	if (total_ms > kSlowProcessMs && now_ns - this->last_slow_log_ns >= kSlowLogIntervalNs) {
		size_t blobs = 0;
		for (const JointSolveCamera &cam : cameras) {
			blobs += cam.blob_count;
		}
		CT_WARN(ct,
		        "JOINT_SLOW ts=%" PRIi64
		        " total_ms=%.2f lock_wait_ms=%.2f predict_ms=%.2f push_ms=%.2f "
		        "led_count_ms=%.2f record_ms=%.2f solve_ms=%.2f blobs=%zu",
		        exposure.timestamp_ns, total_ms, lock_wait_ms, predict_ms, push_ms, led_count_ms, record_ms,
		        total_ms - lock_wait_ms - predict_ms - push_ms - led_count_ms - record_ms, blobs);
		this->last_slow_log_ns = now_ns;
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
		        "JOINT_STATUS exposures=%" PRIu64 " processed=%" PRIu64 " skipped=%" PRIu64
		        " late_samples=%" PRIu64 " tracked=%" PRIu64 " bootstrapped=%" PRIu64 " oriented=%" PRIu64
		        " failed=%" PRIu64 " unconfirmed=%" PRIu64 " mean_solve_us=%.0f max_solve_us=%.0f",
		        assembled, this->processed, skipped, late, this->device_tracked, this->device_bootstrapped,
		        this->device_oriented, this->device_failed, this->unconfirmed_dropped,
		        solves ? this->solve_us_total / (double)solves : 0.0, this->solve_us_max);
		this->last_status_ns = exposure.timestamp_ns;
		this->solve_us_max = 0.0;
	}
}

void
JointProcessor::accountLosses(int64_t timestamp_ns,
                              const std::vector<JointSolveCamera> &cameras,
                              const std::vector<std::vector<t_constellation_device_id_t>> &owners,
                              const std::vector<CameraSample *> &samples,
                              const std::map<t_constellation_device_id_t, xrt_space_relation> &predictions,
                              const std::map<t_constellation_device_id_t, bool> &solved)
{
	ConstellationTracker *ct = this->tracker;

	// LED-shaped blobs no device claimed, by camera of this exposure.
	std::vector<float> free_blobs(cameras.size(), 0.0f);
	for (size_t i = 0; i < cameras.size(); i++) {
		for (uint32_t b = 0; b < cameras[i].blob_count; b++) {
			if (owners[i][b] == XRT_CONSTELLATION_INVALID_DEVICE_ID &&
			    t_constellation_blob_is_led_shaped(cameras[i].blobs[b])) {
				free_blobs[i] += 1.0f;
			}
		}
	}

	auto confirmed = [&](t_constellation_device_id_t id) {
		auto it = solved.find(id);
		return it != solved.end() && it->second;
	};

	// Lamps, window glare and stray blobs: what is left over while every device is solved.
	bool all_confirmed = !ct->devices.empty();
	for (std::unique_ptr<Device> &owned : ct->devices) {
		all_confirmed = all_confirmed && confirmed(owned->id);
	}
	if (this->free_led_background.size() < this->camera_count) {
		this->free_led_background.resize(this->camera_count, 0.0f);
		this->free_led_background_seeded.resize(this->camera_count, false);
	}
	std::vector<float> excess(cameras.size(), 0.0f);
	for (size_t i = 0; i < cameras.size(); i++) {
		uint32_t index = samples[i]->camera_index;
		if (index >= this->camera_count) {
			continue;
		}
		float &background = this->free_led_background[index];
		if (all_confirmed) {
			background = this->free_led_background_seeded[index]
			                 ? background + kBackgroundAlpha * (free_blobs[i] - background)
			                 : free_blobs[i];
			this->free_led_background_seeded[index] = true;
		}
		excess[i] = free_blobs[i] - background;
	}

	for (std::unique_ptr<Device> &owned : ct->devices) {
		Device *device = owned.get();
		JointDeviceState &state = this->devices[device->id];
		JointDeviceState::Loss &loss = state.loss;

		if (confirmed(device->id)) {
			const int64_t gap_ns = timestamp_ns - loss.last_confirmed_ns;
			if (loss.active && gap_ns >= kLossLogMinNs) {
				std::string background;
				for (size_t c = 0; c < this->free_led_background.size(); c++) {
					char value[16];
					snprintf(value, sizeof(value), "%s%.1f", c ? "," : "",
					         this->free_led_background[c]);
					background += value;
				}
				CT_WARN(ct,
				        "JOINT_LOSS device=%u gap_ms=%.1f exposures=%u acquiring=%u lit_multi=%u "
				        "lit_ambiguous=%u lit_single=%u dark_in_view=%u dark_out_of_view=%u "
				        "dark_unpredicted=%u oriented=%u "
				        "max_excess_blobs=%.1f start_cameras=%u start_in_view=%u start_margin_px=%.0f "
				        "background=%s",
				        (unsigned)device->id, (double)gap_ns / 1e6, loss.exposures, loss.acquiring,
				        loss.lit_multi, loss.lit_ambiguous, loss.lit_single, loss.dark_in_view,
				        loss.dark_out_of_view, loss.dark_unpredicted, loss.oriented,
				        loss.max_excess_blobs, loss.start_cameras, loss.start_in_view,
				        loss.start_margin_px, background.c_str());
			}
			loss = JointDeviceState::Loss{};
			loss.last_confirmed_ns = timestamp_ns;
			loss.last_confirmed_position = state.Tcv_world_device.position;
			loss.start_cameras = state.last_cameras_used;
			continue;
		}
		// Only a loss after a confirmed track; the first acquisition is the LED bootstrap's business.
		if (loss.last_confirmed_ns == 0) {
			continue;
		}

		if (!loss.active) {
			loss.active = true;
			loss.start_in_view =
			    cameras_in_view(cameras, loss.last_confirmed_position, &loss.start_margin_px);
		}
		loss.exposures++;

		// Where the device should be, while its prediction still has a position (the filter expires it).
		bool have_prediction = false;
		uint32_t in_view = 0;
		auto prediction = predictions.find(device->id);
		if (prediction != predictions.end() &&
		    relation_has(prediction->second, XRT_SPACE_RELATION_POSITION_VALID_BIT)) {
			xrt_pose Tcv_predicted;
			math_pose_convert_from_opencv(&prediction->second.pose, &Tcv_predicted);
			float margin_px;
			in_view = cameras_in_view(cameras, Tcv_predicted.position, &margin_px);
			have_prediction = true;
		}

		uint32_t lit_cameras = 0;
		for (size_t i = 0; i < cameras.size(); i++) {
			loss.max_excess_blobs = std::max(loss.max_excess_blobs, excess[i]);
			lit_cameras += excess[i] >= kLitExcessBlobs ? 1 : 0;
		}
		// Another lost device's ring is just as free; with one around, lit cameras do not identify this one.
		bool others_solved = true;
		for (std::unique_ptr<Device> &other : ct->devices) {
			if (other->id != device->id && solved.find(other->id) == solved.end()) {
				others_solved = false;
			}
		}

		if (solved.find(device->id) != solved.end()) {
			loss.acquiring++;
		} else if (lit_cameras >= 2) {
			(others_solved ? loss.lit_multi : loss.lit_ambiguous)++;
		} else if (lit_cameras == 1) {
			loss.lit_single++;
		} else if (!have_prediction) {
			loss.dark_unpredicted++;
		} else if (in_view > 0) {
			loss.dark_in_view++;
		} else {
			loss.dark_out_of_view++;
		}
	}
}

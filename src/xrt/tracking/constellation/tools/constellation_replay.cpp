// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Offline replay of constellation tracker datasets (constellation.ctd).
 *
 * Loads a dataset written by the tracker's data recorder, regroups the camera samples into exposures and reports
 * what was recorded. Solvers are run against the same input so their results and cost can be compared.
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "t_constellation_tracker_dataset.hpp"
#include "joint_pose_solver.hpp"
#include "stereo_bootstrap.hpp"
#include "t_constellation_tracker.h"

#include "xrt/xrt_frame.h"

#include "math/m_api.h"

#include <Eigen/Geometry>
#include <Eigen/SVD>
#include "tracking/t_camera_models.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace xrt::tracking::constellation;

namespace {

//! Samples from different cameras of one exposure are this close in time.
constexpr int64_t kExposureToleranceNs = 2'000'000;

struct Exposure
{
	int64_t timestamp_ns;
	std::vector<const CameraSample *> samples;
};

std::vector<Exposure>
group_exposures(const std::vector<CameraSample> &samples)
{
	std::vector<const CameraSample *> sorted;
	sorted.reserve(samples.size());
	for (const CameraSample &sample : samples) {
		sorted.push_back(&sample);
	}
	std::sort(sorted.begin(), sorted.end(),
	          [](const CameraSample *a, const CameraSample *b) { return a->timestamp_ns < b->timestamp_ns; });

	std::vector<Exposure> exposures;
	for (const CameraSample *sample : sorted) {
		if (exposures.empty() || sample->timestamp_ns - exposures.back().timestamp_ns > kExposureToleranceNs) {
			exposures.push_back(Exposure{sample->timestamp_ns, {}});
		}
		exposures.back().samples.push_back(sample);
	}
	return exposures;
}

const char *
flags_name(xrt_space_relation_flags flags)
{
	bool orientation = (flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0;
	bool position = (flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0;
	if (orientation && position) {
		return "pose";
	}
	if (orientation) {
		return "orientation-only";
	}
	if (position) {
		return "position-only";
	}
	return "none";
}

int
summarise(const DatasetReader &dataset)
{
	std::printf("read: %s\n", dataset.stop_reason.empty() ? "clean end of file" : dataset.stop_reason.c_str());

	for (size_t m = 0; m < dataset.mosaics.size(); m++) {
		const DatasetMosaic &mosaic = dataset.mosaics[m];
		std::printf("mosaic %zu: %zu cameras\n", m, mosaic.camera_calibrations.size());
		for (size_t c = 0; c < mosaic.camera_calibrations.size(); c++) {
			const t_camera_calibration &cal = mosaic.camera_calibrations[c];
			std::printf("  camera %zu: %ux%u fx %.1f fy %.1f cx %.1f cy %.1f model %d\n", c,
			            cal.image_size_pixels.w, cal.image_size_pixels.h, cal.intrinsics[0][0],
			            cal.intrinsics[1][1], cal.intrinsics[0][2], cal.intrinsics[1][2],
			            (int)cal.distortion_model);
		}
	}

	for (const DatasetDevice &device : dataset.devices) {
		std::printf("device %d: %zu LEDs\n", (int)device.id, device.leds.size());
	}

	std::map<uint32_t, size_t> samples_per_camera;
	std::map<uint32_t, size_t> posed_per_camera;
	std::map<uint32_t, size_t> blobs_per_camera;
	std::map<std::pair<int, uint32_t>, size_t> found_per_device_camera;
	for (const CameraSample &sample : dataset.samples) {
		samples_per_camera[sample.camera_index]++;
		posed_per_camera[sample.camera_index] += sample.Txr_world_cam.has_value() ? 1 : 0;
		blobs_per_camera[sample.camera_index] += sample.blob_count;
		for (uint32_t d = 0; d < sample.device_count; d++) {
			const DeviceState &state = sample.device_states[d];
			if (state.found_pose.has_value()) {
				found_per_device_camera[{(int)state.device_id, sample.camera_index}]++;
			}
		}
	}
	for (const auto &[camera, count] : samples_per_camera) {
		std::printf("camera %u: %zu samples, %zu with a camera pose, mean %.1f blobs\n", camera, count,
		            posed_per_camera[camera], (double)blobs_per_camera[camera] / (double)count);
	}
	for (const auto &[key, count] : found_per_device_camera) {
		std::printf("device %d camera %u: %zu recorded poses\n", key.first, key.second, count);
	}

	std::vector<Exposure> exposures = group_exposures(dataset.samples);
	std::map<size_t, size_t> exposure_sizes;
	for (const Exposure &exposure : exposures) {
		exposure_sizes[exposure.samples.size()]++;
	}
	if (!exposures.empty()) {
		double span_s = (double)(exposures.back().timestamp_ns - exposures.front().timestamp_ns) / 1e9;
		std::printf("exposures: %zu over %.1f s (%.1f Hz); cameras per exposure:", exposures.size(), span_s,
		            span_s > 0 ? (double)exposures.size() / span_s : 0.0);
		for (const auto &[size, count] : exposure_sizes) {
			std::printf(" %zu:%zu", size, count);
		}
		std::printf("\n");
	}

	std::map<std::pair<int, std::string>, size_t> tracking_kinds;
	for (const DatasetDeviceTracking &tracking : dataset.device_tracking) {
		tracking_kinds[{(int)tracking.device_id, flags_name(tracking.relation_flags)}]++;
	}
	std::printf("device tracking packets: %zu\n", dataset.device_tracking.size());
	for (const auto &[key, count] : tracking_kinds) {
		std::printf("  device %d %s: %zu\n", key.first, key.second.c_str(), count);
	}

	return dataset.stop_reason.empty() ? 0 : 1;
}

/*
 *
 * M1 replay: joint multi-camera tracking from recorded blobs.
 *
 */

struct Stats
{
	std::vector<double> values;

	void
	add(double v)
	{
		values.push_back(v);
	}

	double
	pct(double p)
	{
		if (values.empty()) {
			return NAN;
		}
		std::sort(values.begin(), values.end());
		return values[std::min(values.size() - 1, (size_t)(p * (double)(values.size() - 1) + 0.5))];
	}
};

double
quat_angle_deg(const xrt_quat &a, const xrt_quat &b)
{
	double dot = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
	return 2.0 * std::acos(std::min(1.0, dot)) * 180.0 / M_PI;
}

double
distance_m(const xrt_vec3 &a, const xrt_vec3 &b)
{
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

struct DeviceTrack
{
	const DatasetDevice *device;
	bool tracking{false};
	xrt_pose Tcv_world_device{};
	int64_t last_solved_ns{0};
	uint32_t consecutive_failures{0};

	//! optical = align * imu, refreshed from every solve (as the driver's optical_from_imu_orientation).
	bool have_align{false};
	xrt_quat align{0, 0, 0, 1};

	// Results.
	uint32_t exposures_with_blobs{0};
	uint32_t solved{0};
	uint32_t seeds{0};
	uint32_t bootstrap_attempts{0};
	uint32_t bootstraps{0};
	Stats bootstrap_us;
	std::map<uint32_t, uint32_t> cameras_used;
	Stats rms_px, coverage, matches, solve_us, recorded_delta_mm, recorded_delta_deg;
	//! Tilt of the optical-from-IMU alignment: how far it moves the vertical. ~0 if both worlds agree on gravity.
	Stats align_tilt_deg;
	//! (optical, IMU) orientation pairs from solved exposures, for estimating the IMU-to-model body offset.
	std::vector<std::pair<Eigen::Quaterniond, Eigen::Quaterniond>> orientation_pairs;
	std::vector<std::pair<int64_t, xrt_vec3>> positions;
};

//! The device's recorded tracking-source orientation at @p timestamp_ns (OpenCV convention), if any.
bool
imu_orientation_at(const DatasetReader &dataset,
                   t_constellation_device_id_t device_id,
                   int64_t timestamp_ns,
                   xrt_quat &out)
{
	const DatasetDeviceTracking *best = nullptr;
	for (const DatasetDeviceTracking &t : dataset.device_tracking) {
		if (t.device_id != device_id || (t.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) == 0) {
			continue;
		}
		if (best == nullptr || std::llabs(t.timestamp_ns - timestamp_ns) < std::llabs(best->timestamp_ns - timestamp_ns)) {
			best = &t;
		}
	}
	if (best == nullptr || std::llabs(best->timestamp_ns - timestamp_ns) > kExposureToleranceNs) {
		return false;
	}
	xrt_pose cv;
	math_pose_convert_from_opencv(&best->pose, &cv);
	out = cv.orientation;
	return true;
}

//! A recorded per-camera pose for the device in this exposure (the live tracker's candidate), in the CV world.
bool
recorded_pose(const Exposure &exposure, t_constellation_device_id_t device_id, xrt_pose &out)
{
	for (const CameraSample *sample : exposure.samples) {
		if (!sample->Txr_world_cam.has_value()) {
			continue;
		}
		for (uint32_t d = 0; d < sample->device_count; d++) {
			const DeviceState &state = sample->device_states[d];
			if (state.device_id != device_id || !state.found_pose.has_value()) {
				continue;
			}
			xrt_pose Tcv_world_cam;
			math_pose_convert_from_opencv(&sample->Txr_world_cam.value(), &Tcv_world_cam);
			math_pose_transform(&Tcv_world_cam, &state.found_pose->Tcv_cam_device, &out);
			return true;
		}
	}
	return false;
}

/*!
 * Estimate the fixed rotation B between the IMU body frame and the LED model frame, from q_opt = A q_imu B with A a
 * constant world alignment. Body-frame relative rotations satisfy q_opt,rel = B^-1 q_imu,rel B, so B^-1 maps each IMU
 * relative-rotation axis onto the optical one (Kabsch). With B known, A = q_opt B^-1 q_imu^-1 should be constant and,
 * if both worlds are gravity-aligned, a pure rotation about the vertical.
 */
void
report_imu_offset(const DeviceTrack &track)
{
	const auto &pairs = track.orientation_pairs;
	if (pairs.size() < 50) {
		return;
	}

	Eigen::Matrix3d H = Eigen::Matrix3d::Zero();
	uint32_t used = 0;
	const size_t stride = std::max<size_t>(1, pairs.size() / 400);
	for (size_t i = 0; i < pairs.size(); i += stride) {
		for (size_t j = i + stride; j < pairs.size(); j += stride) {
			Eigen::AngleAxisd opt_rel(pairs[j].first.conjugate() * pairs[i].first);
			Eigen::AngleAxisd imu_rel(pairs[j].second.conjugate() * pairs[i].second);
			if (opt_rel.angle() < 10.0 * M_PI / 180.0 || imu_rel.angle() < 10.0 * M_PI / 180.0) {
				continue;
			}
			double weight = std::min(opt_rel.angle(), imu_rel.angle());
			H += weight * imu_rel.axis() * opt_rel.axis().transpose();
			used++;
		}
	}
	if (used < 20) {
		std::printf("  IMU body offset: not enough rotation to estimate (%u pairs over 10 degrees)\n", used);
		return;
	}
	Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
	Eigen::Matrix3d D = Eigen::Matrix3d::Identity();
	D(2, 2) = (svd.matrixV() * svd.matrixU().transpose()).determinant() < 0 ? -1.0 : 1.0;
	// R maps IMU axes to optical axes: axis_opt = R axis_imu, and R = B^-1.
	Eigen::Matrix3d R = svd.matrixV() * D * svd.matrixU().transpose();
	Eigen::Quaterniond B_inv(R);
	Eigen::Quaterniond B = B_inv.conjugate();

	Stats axis_residual_deg, tilt_deg, align_spread_deg;
	std::vector<Eigen::Quaterniond> aligns;
	for (size_t i = 0; i < pairs.size(); i += stride) {
		Eigen::Quaterniond A = pairs[i].first * B_inv * pairs[i].second.conjugate();
		aligns.push_back(A);
		Eigen::Vector3d up(0, -1, 0);
		double c = std::max(-1.0, std::min(1.0, up.dot(A * up)));
		tilt_deg.add(std::acos(c) * 180.0 / M_PI);
	}
	for (const auto &A : aligns) {
		align_spread_deg.add(A.angularDistance(aligns[aligns.size() / 2]) * 180.0 / M_PI);
	}
	Eigen::AngleAxisd b(B);
	std::printf("  IMU body offset B: %.1f deg about (%.3f, %.3f, %.3f) from %u relative rotations; quat "
	            "(x %.4f, y %.4f, z %.4f, w %.4f)\n",
	            b.angle() * 180.0 / M_PI, b.axis().x(), b.axis().y(), b.axis().z(), used, B.x(), B.y(), B.z(), B.w());
	std::printf("  with B: world alignment tilt deg p50 %.2f p95 %.2f; alignment spread deg p50 %.2f p95 %.2f\n",
	            tilt_deg.pct(0.5), tilt_deg.pct(0.95), align_spread_deg.pct(0.5), align_spread_deg.pct(0.95));
}

int
replay_m1(const DatasetReader &dataset, const char *csv_path, bool seed_recorded)
{
	if (dataset.mosaics.empty()) {
		std::fprintf(stderr, "no cameras in dataset\n");
		return 1;
	}
	const DatasetMosaic &mosaic = dataset.mosaics[0];
	std::vector<t_camera_model_params> models(mosaic.camera_calibrations.size());
	for (size_t c = 0; c < models.size(); c++) {
		t_camera_model_params_from_t_camera_calibration(&mosaic.camera_calibrations[c], &models[c]);
	}

	std::vector<DeviceTrack> tracks;
	for (const DatasetDevice &device : dataset.devices) {
		DeviceTrack track;
		track.device = &device;
		tracks.push_back(track);
	}

	FILE *csv = csv_path ? std::fopen(csv_path, "w") : nullptr;
	if (csv) {
		std::fprintf(csv, "timestamp_ns,device,solved,seeded,cameras,matches,rms_px,coverage,outliers,solve_us,"
		                  "px,py,pz,qx,qy,qz,qw,rms_cam0,rms_cam1,rms_cam2,rms_cam3,n_cam0,n_cam1,n_cam2,n_cam3\n");
	}

	JointSolveParams params;
	params.orientation_prior_sigma_deg = 3.0f;

	std::vector<Exposure> exposures = group_exposures(dataset.samples);
	for (const Exposure &exposure : exposures) {
		// Cameras of this exposure, blob ownership shared between devices.
		std::vector<JointSolveCamera> cameras;
		std::vector<uint32_t> camera_index_of;
		std::vector<std::vector<t_constellation_device_id_t>> owners;
		owners.reserve(exposure.samples.size());
		for (const CameraSample *sample : exposure.samples) {
			if (!sample->Txr_world_cam.has_value() || sample->camera_index >= models.size()) {
				continue;
			}
			xrt_pose Tcv_world_cam;
			math_pose_convert_from_opencv(&sample->Txr_world_cam.value(), &Tcv_world_cam);
			const t_camera_calibration &cal = mosaic.camera_calibrations[sample->camera_index];
			owners.emplace_back(sample->blob_count, XRT_CONSTELLATION_INVALID_DEVICE_ID);
			camera_index_of.push_back(sample->camera_index);
			cameras.push_back(JointSolveCamera{Tcv_world_cam, &models[sample->camera_index],
			                                   (int)cal.image_size_pixels.w, (int)cal.image_size_pixels.h,
			                                   sample->blobs, sample->blob_count, nullptr});
		}
		for (size_t i = 0; i < cameras.size(); i++) {
			cameras[i].blob_owner = owners[i].data();
		}
		bool any_blobs = false;
		for (const JointSolveCamera &cam : cameras) {
			any_blobs |= cam.blob_count > 0;
		}

		// Tracked devices first, so a lost device cannot claim a tracked ring's blobs.
		std::vector<DeviceTrack *> order;
		for (DeviceTrack &t : tracks) {
			order.push_back(&t);
		}
		std::stable_sort(order.begin(), order.end(),
		                 [](const DeviceTrack *a, const DeviceTrack *b) { return a->tracking && !b->tracking; });

		for (DeviceTrack *track : order) {
			t_constellation_device_id_t id = track->device->id;
			if (any_blobs) {
				track->exposures_with_blobs++;
			}

			xrt_quat imu;
			bool have_imu = imu_orientation_at(dataset, id, exposure.timestamp_ns, imu);

			xrt_pose prior;
			bool seeded = false;
			bool bootstrapped = false;
			JointSolveResult result;
			bool ok = false;
			double us = 0.0;
			if (track->tracking) {
				prior = track->Tcv_world_device;
				if (have_imu && track->have_align) {
					math_quat_rotate(&track->align, &imu, &prior.orientation);
				}
				auto start = std::chrono::steady_clock::now();
				ok = have_imu && track->have_align
				         ? joint_solve_refine(cameras, track->device->led_model, prior, prior.orientation,
				                              params, result)
				         : joint_solve_refine(cameras, track->device->led_model, prior, JointSolveParams{},
				                              result);
				us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
				track->solve_us.add(us);
			} else if (seed_recorded) {
				if (!recorded_pose(exposure, id, prior)) {
					continue;
				}
				seeded = true;
				auto start = std::chrono::steady_clock::now();
				ok = joint_solve_refine(cameras, track->device->led_model, prior, JointSolveParams{}, result);
				us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
				track->solve_us.add(us);
			} else {
				StereoBootstrapResult bootstrap;
				auto start = std::chrono::steady_clock::now();
				ok = stereo_bootstrap(cameras, track->device->led_model, StereoBootstrapParams{}, bootstrap);
				us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
				track->bootstrap_attempts++;
				track->bootstrap_us.add(us);
				result = bootstrap.refined;
				bootstrapped = ok;
				seeded = true;
			}

			if (csv) {
				const xrt_pose &p = result.Tcv_world_device;
				// Per recorded camera index: RMS and count of this solve's correspondences.
				double sum2[4] = {0, 0, 0, 0};
				int count[4] = {0, 0, 0, 0};
				for (const JointSolveMatch &m : result.correspondences) {
					uint32_t cam = camera_index_of[m.camera];
					if (cam < 4) {
						sum2[cam] += (double)m.residual_px * m.residual_px;
						count[cam]++;
					}
				}
				std::fprintf(csv, "%" PRIi64 ",%d,%d,%d,%u,%u,%.4f,%.3f,%u,%.1f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
				             exposure.timestamp_ns, (int)id, ok ? 1 : 0, seeded ? 1 : 0, result.cameras_used,
				             result.matches, result.rms_px, result.coverage, result.outliers, us, p.position.x,
				             p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z,
				             p.orientation.w);
				for (int c = 0; c < 4; c++) {
					std::fprintf(csv, ",%.4f", count[c] ? std::sqrt(sum2[c] / count[c]) : NAN);
				}
				for (int c = 0; c < 4; c++) {
					std::fprintf(csv, ",%d", count[c]);
				}
				std::fprintf(csv, "\n");
			}

			if (!ok) {
				if (track->tracking && ++track->consecutive_failures > 3) {
					track->tracking = false;
				}
				continue;
			}

			track->solved++;
			track->seeds += seeded && !bootstrapped ? 1 : 0;
			track->bootstraps += bootstrapped ? 1 : 0;
			track->tracking = true;
			track->consecutive_failures = 0;
			track->Tcv_world_device = result.Tcv_world_device;
			track->last_solved_ns = exposure.timestamp_ns;
			track->cameras_used[result.cameras_used]++;
			track->rms_px.add(result.rms_px);
			track->coverage.add(result.coverage);
			track->matches.add(result.matches);
			track->positions.push_back({exposure.timestamp_ns, result.Tcv_world_device.position});
			if (have_imu) {
				const xrt_quat &o = result.Tcv_world_device.orientation;
				track->orientation_pairs.push_back({Eigen::Quaterniond(o.w, o.x, o.y, o.z).normalized(),
				                                    Eigen::Quaterniond(imu.w, imu.x, imu.y, imu.z).normalized()});
				xrt_quat inverse_imu;
				math_quat_invert(&imu, &inverse_imu);
				math_quat_rotate(&result.Tcv_world_device.orientation, &inverse_imu, &track->align);
				math_quat_normalize(&track->align);
				track->have_align = true;
				// Vertical in the OpenCV-convention world is -y.
				xrt_vec3 up{0.0f, -1.0f, 0.0f}, rotated;
				math_quat_rotate_vec3(&track->align, &up, &rotated);
				double c = std::max(-1.0, std::min(1.0, (double)(-rotated.y)));
				track->align_tilt_deg.add(std::acos(c) * 180.0 / M_PI);
			}
			for (const JointSolveMatch &m : result.correspondences) {
				owners[m.camera][m.blob] = id;
			}

			xrt_pose recorded;
			if (!seeded && recorded_pose(exposure, id, recorded)) {
				track->recorded_delta_mm.add(1000.0 * distance_m(recorded.position, result.Tcv_world_device.position));
				track->recorded_delta_deg.add(quat_angle_deg(recorded.orientation, result.Tcv_world_device.orientation));
			}
		}
	}
	if (csv) {
		std::fclose(csv);
	}

	for (DeviceTrack &track : tracks) {
		// Static jitter: position spread within 1 s windows whose motion stays under 20 mm.
		Stats jitter_mm;
		size_t begin = 0;
		for (size_t i = 0; i < track.positions.size(); i++) {
			if (track.positions[i].first - track.positions[begin].first < 1'000'000'000 &&
			    i + 1 < track.positions.size()) {
				continue;
			}
			size_t n = i - begin;
			if (n >= 20) {
				double mx = 0, my = 0, mz = 0;
				for (size_t k = begin; k < i; k++) {
					mx += track.positions[k].second.x;
					my += track.positions[k].second.y;
					mz += track.positions[k].second.z;
				}
				mx /= n, my /= n, mz /= n;
				double worst = 0, sum2 = 0;
				for (size_t k = begin; k < i; k++) {
					double d = std::sqrt(std::pow(track.positions[k].second.x - mx, 2) +
					                     std::pow(track.positions[k].second.y - my, 2) +
					                     std::pow(track.positions[k].second.z - mz, 2));
					worst = std::max(worst, d);
					sum2 += d * d;
				}
				if (worst < 0.02) {
					jitter_mm.add(1000.0 * std::sqrt(sum2 / n));
				}
			}
			begin = i;
		}

		std::printf("M1 device %d: solved %u of %u exposures with blobs (%.1f%%), %u from recorded seeds, %u "
		            "bootstraps from %u attempts\n",
		            (int)track.device->id, track.solved, track.exposures_with_blobs,
		            track.exposures_with_blobs ? 100.0 * track.solved / track.exposures_with_blobs : 0.0, track.seeds,
		            track.bootstraps, track.bootstrap_attempts);
		std::printf("  bootstrap us p50 %.0f p95 %.0f max %.0f\n", track.bootstrap_us.pct(0.5),
		            track.bootstrap_us.pct(0.95), track.bootstrap_us.pct(1.0));
		std::printf("  cameras used:");
		for (const auto &[cams, count] : track.cameras_used) {
			std::printf(" %u:%u", cams, count);
		}
		std::printf("\n  matches p50 %.0f; rms px p50 %.3f p95 %.3f; coverage p50 %.2f p05 %.2f\n",
		            track.matches.pct(0.5), track.rms_px.pct(0.5), track.rms_px.pct(0.95), track.coverage.pct(0.5),
		            track.coverage.pct(0.05));
		std::printf("  solve us p50 %.0f p95 %.0f max %.0f\n", track.solve_us.pct(0.5), track.solve_us.pct(0.95),
		            track.solve_us.pct(1.0));
		std::printf("  static jitter mm (1 s windows) p50 %.2f p95 %.2f over %zu windows\n", jitter_mm.pct(0.5),
		            jitter_mm.pct(0.95), jitter_mm.values.size());
		std::printf("  optical-from-IMU alignment tilt deg p50 %.2f p95 %.2f\n", track.align_tilt_deg.pct(0.5),
		            track.align_tilt_deg.pct(0.95));
		std::printf("  vs recorded per-camera poses: mm p50 %.1f p95 %.1f, deg p50 %.2f p95 %.2f (n=%zu)\n",
		            track.recorded_delta_mm.pct(0.5), track.recorded_delta_mm.pct(0.95),
		            track.recorded_delta_deg.pct(0.5), track.recorded_delta_deg.pct(0.95),
		            track.recorded_delta_mm.values.size());
	}
	for (DeviceTrack &track : tracks) {
		report_imu_offset(track);
	}
	return 0;
}

/*
 *
 * Tracker replay: recorded blobs through the real ConstellationTracker (joint path, deterministic).
 *
 */

struct FakeOrigin
{
	t_constellation_tracker_tracking_source base;
	//! Camera 0's recorded world pose by timestamp; the origin is placed at camera 0.
	std::vector<std::pair<int64_t, xrt_pose>> poses;
};

void
fake_origin_get(t_constellation_tracker_tracking_source *source, int64_t when_ns, xrt_space_relation *out)
{
	FakeOrigin *origin = (FakeOrigin *)source;
	*out = XRT_SPACE_RELATION_ZERO;
	if (origin->poses.empty()) {
		return;
	}
	auto it = std::lower_bound(origin->poses.begin(), origin->poses.end(), when_ns,
	                           [](const std::pair<int64_t, xrt_pose> &p, int64_t t) { return p.first < t; });
	if (it == origin->poses.end() || (it != origin->poses.begin() && when_ns - (it - 1)->first < it->first - when_ns)) {
		it = it == origin->poses.begin() ? it : it - 1;
	}
	out->pose = it->second;
	out->relation_flags = (xrt_space_relation_flags)(XRT_SPACE_RELATION_POSITION_VALID_BIT |
	                                                 XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                 XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	                                                 XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
}

//! Stands in for the device driver: accepts every pushed sample and predicts the last one while it is recent.
struct FakeDevice
{
	t_constellation_tracker_device base;
	t_constellation_tracker_tracking_source source;
	std::vector<t_constellation_tracker_led> leds; // XR convention, as a driver provides them
	t_constellation_device_id_t id{XRT_CONSTELLATION_INVALID_DEVICE_ID};

	bool have_last{false};
	t_constellation_tracker_sample last{};
	//! The recorded tracking-source relations for this device, returned when there is no recent push, as a driver
	//! returns its (unaligned) IMU orientation before it has optical history.
	std::vector<std::pair<int64_t, xrt_space_relation>> recorded;
	uint32_t pushes{0};
	std::map<uint32_t, uint32_t> joint_cameras;
	Stats rms_px;
	std::vector<std::pair<int64_t, xrt_vec3>> positions;
	FILE *csv{nullptr};
};

FakeDevice *
fake_device_of_source(t_constellation_tracker_tracking_source *source)
{
	return (FakeDevice *)((char *)source - offsetof(FakeDevice, source));
}

bool
fake_device_push(t_constellation_tracker_device *device, t_constellation_tracker_sample *sample)
{
	FakeDevice *fake = (FakeDevice *)device;
	fake->pushes++;
	fake->joint_cameras[sample->joint_camera_count]++;
	fake->rms_px.add(sample->metrics.reprojection_error);
	fake->positions.push_back({sample->timestamp_ns, sample->pose.position});
	if (fake->csv) {
		const xrt_pose &p = sample->pose;
		std::fprintf(fake->csv, "%" PRIi64 ",%d,%u,%u,%.4f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", sample->timestamp_ns,
		             (int)fake->id, sample->joint_camera_count, sample->metrics.matched_blob_count,
		             sample->metrics.reprojection_error, p.position.x, p.position.y, p.position.z, p.orientation.x,
		             p.orientation.y, p.orientation.z, p.orientation.w);
	}
	fake->last = *sample;
	fake->have_last = true;
	return true;
}

void
fake_device_get(t_constellation_tracker_tracking_source *source, int64_t when_ns, xrt_space_relation *out)
{
	/*
	 * Like the Sense driver: orientation is the IMU's (the recorded relation's orientation, in the IMU's own world),
	 * position is the last optical pose while it is fresh. Before any push there is only the orientation.
	 */
	FakeDevice *fake = fake_device_of_source(source);
	*out = XRT_SPACE_RELATION_ZERO;
	auto it = std::lower_bound(fake->recorded.begin(), fake->recorded.end(), when_ns,
	                           [](const auto &p, int64_t t) { return p.first < t; });
	if (it != fake->recorded.end() && std::llabs(it->first - when_ns) < 5'000'000 &&
	    (it->second.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0) {
		out->pose.orientation = it->second.pose.orientation;
		out->relation_flags = (xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		                                                 XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	}
	if (fake->have_last && std::llabs(when_ns - fake->last.timestamp_ns) < 100'000'000) {
		out->pose.position = fake->last.pose.position;
		out->relation_flags = (xrt_space_relation_flags)(out->relation_flags | XRT_SPACE_RELATION_POSITION_VALID_BIT);
	}
}

int
replay_tracker(const DatasetReader &dataset, const char *csv_path)
{
	FILE *csv = csv_path ? std::fopen(csv_path, "w") : nullptr;
	if (csv) {
		std::fprintf(csv, "timestamp_ns,device,cameras,matches,rms_px,px,py,pz,qx,qy,qz,qw\n");
	}
	if (dataset.mosaics.empty() || dataset.samples.empty()) {
		std::fprintf(stderr, "nothing to replay\n");
		return 1;
	}
	const DatasetMosaic &mosaic = dataset.mosaics[0];
	const size_t camera_count = mosaic.camera_calibrations.size();

	// Rig geometry from the first exposure that has every camera's pose.
	std::vector<Exposure> exposures = group_exposures(dataset.samples);
	std::vector<std::optional<xrt_pose>> first_world(camera_count);
	for (const Exposure &exposure : exposures) {
		size_t have = 0;
		std::vector<std::optional<xrt_pose>> world(camera_count);
		for (const CameraSample *sample : exposure.samples) {
			if (sample->camera_index < camera_count && sample->Txr_world_cam.has_value()) {
				world[sample->camera_index] = sample->Txr_world_cam;
				have++;
			}
		}
		if (have == camera_count && world[0].has_value()) {
			first_world = world;
			break;
		}
	}
	if (!first_world[0].has_value()) {
		std::fprintf(stderr, "no exposure with every camera's pose\n");
		return 1;
	}

	FakeOrigin origin{};
	origin.base.get_tracked_pose = fake_origin_get;
	for (const CameraSample &sample : dataset.samples) {
		if (sample.camera_index == 0 && sample.Txr_world_cam.has_value()) {
			origin.poses.push_back({sample.timestamp_ns, sample.Txr_world_cam.value()});
		}
	}
	std::sort(origin.poses.begin(), origin.poses.end(),
	          [](const auto &a, const auto &b) { return a.first < b.first; });

	t_constellation_tracker_params params{};
	params.flags = T_CONSTELLATION_TRACKER_FLAGS_DETERMINISTIC;
	params.num_mosaics = 1;
	params.mosaics[0].tracking_origin = &origin.base;
	params.mosaics[0].num_cameras = camera_count;
	xrt_pose inverse_cam0;
	math_pose_invert(&first_world[0].value(), &inverse_cam0);
	for (size_t c = 0; c < camera_count; c++) {
		params.mosaics[0].cameras[c].calibration = mosaic.camera_calibrations[c];
		math_pose_transform(&inverse_cam0, &first_world[c].value(), &params.mosaics[0].cameras[c].pose_in_origin);
		params.mosaics[0].cameras[c].has_concrete_pose = true;
	}

	setenv("CONSTELLATION_TRACKER_JOINT", "1", 1);
	xrt_frame_context xfctx{};
	t_constellation_tracker *tracker = nullptr;
	if (t_constellation_tracker_create(&xfctx, &params, &tracker) != 0) {
		std::fprintf(stderr, "failed to create tracker\n");
		return 1;
	}

	std::vector<std::unique_ptr<FakeDevice>> fakes;
	for (const DatasetDevice &device : dataset.devices) {
		auto fake = std::make_unique<FakeDevice>();
		fake->base.push_constellation_tracker_sample = fake_device_push;
		fake->base.push_camera_blob_count = nullptr;
		fake->source.get_tracked_pose = fake_device_get;
		fake->csv = csv;
		for (const DatasetDeviceTracking &t : dataset.device_tracking) {
			if (t.device_id == device.id) {
				xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
				relation.pose = t.pose;
				relation.relation_flags = t.relation_flags;
				fake->recorded.push_back({t.timestamp_ns, relation});
			}
		}
		std::sort(fake->recorded.begin(), fake->recorded.end(),
		          [](const auto &a, const auto &b) { return a.first < b.first; });
		// The recorded model is in the tracker's OpenCV convention; drivers hand over OpenXR.
		fake->leds = device.leds;
		for (t_constellation_tracker_led &led : fake->leds) {
			led.position.y = -led.position.y;
			led.position.z = -led.position.z;
			led.normal.y = -led.normal.y;
			led.normal.z = -led.normal.z;
		}
		t_constellation_tracker_device_params dparams{};
		dparams.led_model = device.led_model;
		dparams.led_model.leds = fake->leds.data();
		dparams.led_model.led_count = fake->leds.size();
		dparams.led_model.compute_led_visibility = nullptr;
		dparams.tracking_source = &fake->source;
		t_constellation_tracker_add_device(tracker, &dparams, &fake->base, &fake->id);
		fakes.push_back(std::move(fake));
	}

	// Feed every camera's frames in time order through the normal blob sinks.
	std::vector<const CameraSample *> order;
	for (const CameraSample &sample : dataset.samples) {
		order.push_back(&sample);
	}
	std::stable_sort(order.begin(), order.end(), [](const CameraSample *a, const CameraSample *b) {
		return a->timestamp_ns != b->timestamp_ns ? a->timestamp_ns < b->timestamp_ns
		                                          : a->camera_index < b->camera_index;
	});
	auto start = std::chrono::steady_clock::now();
	std::vector<t_blob> blobs;
	for (const CameraSample *sample : order) {
		if (sample->camera_index >= camera_count) {
			continue;
		}
		blobs.assign(sample->blobs, sample->blobs + sample->blob_count);
		for (t_blob &b : blobs) {
			b.matched_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID;
			b.matched_device_led_id = XRT_CONSTELLATION_INVALID_LED_ID;
		}
		t_blob_observation observation{};
		observation.source = nullptr;
		observation.id = sample->id;
		observation.timestamp_ns = sample->timestamp_ns;
		observation.blobs = blobs.data();
		observation.num_blobs = (uint32_t)blobs.size();
		t_blob_sink *sink = params.mosaics[0].cameras[sample->camera_index].blob_sink;
		sink->push_blobs(sink, &observation);
	}
	double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

	xrt_frame_context_destroy_nodes(&xfctx);
	if (csv) {
		std::fclose(csv);
	}

	std::printf("tracker replay (joint path): %zu frames in %.2f s (%.0f us per exposure)\n", order.size(), seconds,
	            1e6 * seconds / (double)std::max<size_t>(1, exposures.size()));
	for (auto &fake : fakes) {
		std::printf("  device %d: %u pushed of %zu exposures; cameras:", (int)fake->id, fake->pushes,
		            exposures.size());
		for (const auto &[cams, count] : fake->joint_cameras) {
			std::printf(" %u:%u", cams, count);
		}
		std::printf("; rms px p50 %.3f p95 %.3f\n", fake->rms_px.pct(0.5), fake->rms_px.pct(0.95));
	}
	return 0;
}

} // namespace

int
main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s DATASET.ctd [--m1] [--seed-recorded] [--csv OUT.csv] [--tracker] [--tracker-csv OUT.csv]\n", argv[0]);
		return 2;
	}
	bool m1 = false;
	bool tracker = false;
	const char *tracker_csv = nullptr;
	const char *tracking_csv = nullptr;
	bool seed_recorded = false;
	const char *csv = nullptr;
	for (int i = 2; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--m1") {
			m1 = true;
		} else if (arg == "--tracker") {
			tracker = true;
		} else if (arg == "--tracking-csv" && i + 1 < argc) {
			tracking_csv = argv[++i];
		} else if (arg == "--tracker-csv" && i + 1 < argc) {
			tracker = true;
			tracker_csv = argv[++i];
		} else if (arg == "--seed-recorded") {
			seed_recorded = true;
		} else if (arg == "--csv" && i + 1 < argc) {
			csv = argv[++i];
		}
	}

	try {
		DatasetReader dataset(argv[1]);
		int status = summarise(dataset);
		if (tracking_csv) {
			// Every recorded tracking-source relation (what the device predicted at each exposure), XR convention.
			FILE *f = std::fopen(tracking_csv, "w");
			std::fprintf(f, "timestamp_ns,device,camera,flags,px,py,pz,qx,qy,qz,qw\n");
			for (const DatasetDeviceTracking &t : dataset.device_tracking) {
				std::fprintf(f, "%" PRIi64 ",%d,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n", t.timestamp_ns,
				             (int)t.device_id, t.camera_index, (unsigned)t.relation_flags, t.pose.position.x,
				             t.pose.position.y, t.pose.position.z, t.pose.orientation.x, t.pose.orientation.y,
				             t.pose.orientation.z, t.pose.orientation.w);
			}
			std::fclose(f);
		}
		if (m1) {
			status = replay_m1(dataset, csv, seed_recorded) != 0 ? 1 : status;
		}
		if (tracker) {
			status = replay_tracker(dataset, tracker_csv) != 0 ? 1 : status;
		}
		return status;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "failed to load %s: %s\n", argv[1], e.what());
		return 1;
	}
}

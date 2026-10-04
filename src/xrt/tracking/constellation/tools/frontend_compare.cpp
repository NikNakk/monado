// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Neutral scoring of optical front ends on one recording (constellation_replay --compare-frontend).
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "frontend_compare.hpp"

#include "math/m_api.h"
#include "tracking/t_camera_models.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace xrt::tracking::constellation {

namespace {

	//! Samples of one exposure are this close in time (as constellation_replay's grouping).
	constexpr int64_t kExposureToleranceNs = 2'000'000;
	//! A reprojected LED and a blob closer than this count as agreeing.
	constexpr double kInlierGatePx = 3.0;
	//! Consecutive poses further apart than this are not compared for motion consistency.
	constexpr int64_t kMaxPairGapNs = 60'000'000;
	//! Jump thresholds: position against a constant-velocity prediction, rotation against the gyro.
	constexpr double kJumpMm = 30.0;
	constexpr double kJumpDeg = 10.0;
	//! The gyro reads below this for 50 ms either side: the controller is still.
	constexpr double kStillRadPerSec = 0.05;
	constexpr int64_t kStillWindowNs = 50'000'000;

	struct Stats
	{
		std::vector<double> values;

		void
		add(double v)
		{
			values.push_back(v);
		}

		size_t
		size() const
		{
			return values.size();
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

		double
		rms() const
		{
			if (values.empty()) {
				return NAN;
			}
			double s = 0;
			for (double v : values) {
				s += v * v;
			}
			return std::sqrt(s / (double)values.size());
		}
	};

	Eigen::Vector3d
	vec(const xrt_vec3 &v)
	{
		return Eigen::Vector3d(v.x, v.y, v.z);
	}

	Eigen::Quaterniond
	quat(const xrt_quat &q)
	{
		return Eigen::Quaterniond(q.w, q.x, q.y, q.z).normalized();
	}

	double
	angle_deg(const Eigen::Quaterniond &a, const Eigen::Quaterniond &b)
	{
		return a.angularDistance(b) * 180.0 / M_PI;
	}

	//! Index of the exposure within the tolerance of @p t, or -1.
	int64_t
	exposure_index(const std::vector<int64_t> &times, int64_t t)
	{
		auto it = std::lower_bound(times.begin(), times.end(), t);
		int64_t best = -1;
		int64_t best_d = kExposureToleranceNs + 1;
		if (it != times.end() && *it - t < best_d) {
			best = it - times.begin();
			best_d = *it - t;
		}
		if (it != times.begin() && t - *(it - 1) < best_d) {
			best = (it - 1) - times.begin();
		}
		return best;
	}

	//! Camera samples by exposure.
	std::vector<std::vector<const CameraSample *>>
	samples_by_exposure(const DatasetReader &dataset, const std::vector<int64_t> &times)
	{
		std::vector<std::vector<const CameraSample *>> out(times.size());
		for (const CameraSample &s : dataset.samples) {
			int64_t i = exposure_index(times, s.timestamp_ns);
			if (i >= 0) {
				out[i].push_back(&s);
			}
		}
		return out;
	}

	//! Gyro samples of one device, ascending, for rotation-angle integration and stillness.
	struct Gyro
	{
		std::vector<int64_t> t;
		std::vector<Eigen::Vector3d> w;

		bool
		empty() const
		{
			return t.empty();
		}

		//! Rotation angle accumulated between @p t1 and @p t2 (rad); false if the samples do not cover the
		//! span.
		bool
		angle_between(int64_t t1, int64_t t2, double &out) const
		{
			auto it = std::upper_bound(t.begin(), t.end(), t1);
			if (it == t.begin() || it == t.end() || t.back() < t2) {
				return false;
			}
			Eigen::Quaterniond q = Eigen::Quaterniond::Identity();
			int64_t prev = t1;
			for (size_t k = it - t.begin(); k < t.size(); k++) {
				if (t[k] - t[k - 1] > 50'000'000) {
					return false; // a gap in the IMU stream
				}
				int64_t end = std::min(t[k], t2);
				double dt = (double)(end - prev) * 1e-9;
				double a = w[k].norm() * dt;
				if (a > 0) {
					q = q * Eigen::Quaterniond(Eigen::AngleAxisd(a, w[k].normalized()));
				}
				prev = end;
				if (t[k] >= t2) {
					break;
				}
			}
			out = 2.0 * std::acos(std::min(1.0, std::fabs(q.w())));
			return true;
		}

		//! True, false, or unknown (-1) without samples near @p when.
		int
		still_at(int64_t when) const
		{
			auto lo = std::lower_bound(t.begin(), t.end(), when - kStillWindowNs);
			auto hi = std::upper_bound(t.begin(), t.end(), when + kStillWindowNs);
			if (lo == hi) {
				return -1;
			}
			for (auto it = lo; it != hi; ++it) {
				if (w[it - t.begin()].norm() > kStillRadPerSec) {
					return 0;
				}
			}
			return 1;
		}
	};

	//! What the common evaluator finds for one pose.
	struct Reprojection
	{
		uint32_t predicted{0};
		uint32_t inliers{0};
		double rms_px{NAN};
	};

	/*!
	 * Project the device's LEDs at @p Tcv_world_device into every camera of the exposure and pair each with the
	 * nearest blob inside the gate, each blob used once. Blobs are not filtered by owner, so the evaluator needs
	 * nothing from any front end.
	 */
	Reprojection
	reproject(const DatasetDevice &device,
	          const xrt_pose &Tcv_world_device,
	          const std::vector<const CameraSample *> &samples,
	          const std::vector<t_camera_model_params> &models,
	          const DatasetMosaic &mosaic)
	{
		Reprojection out;
		const Eigen::Quaterniond q_wd = quat(Tcv_world_device.orientation);
		const Eigen::Vector3d p_wd = vec(Tcv_world_device.position);
		double sum2 = 0.0;
		for (const CameraSample *sample : samples) {
			if (!sample->Txr_world_cam.has_value() || sample->camera_index >= models.size()) {
				continue;
			}
			xrt_pose Tcv_world_cam;
			math_pose_convert_from_opencv(&sample->Txr_world_cam.value(), &Tcv_world_cam);
			const Eigen::Quaterniond q_cw = quat(Tcv_world_cam.orientation).conjugate();
			const Eigen::Vector3d p_wc = vec(Tcv_world_cam.position);
			const t_camera_calibration &cal = mosaic.camera_calibrations[sample->camera_index];

			struct Pair
			{
				double d2;
				size_t led;
				uint32_t blob;
			};
			std::vector<Pair> pairs;
			for (size_t l = 0; l < device.leds.size(); l++) {
				const t_constellation_tracker_led &led = device.leds[l];
				Eigen::Vector3d p_cam = q_cw * (q_wd * vec(led.position) + p_wd - p_wc);
				if (p_cam.z() < 0.05) {
					continue;
				}
				Eigen::Vector3d n_cam = q_cw * (q_wd * vec(led.normal));
				// The view ray points away from the camera and the normal towards it (as
				// joint_pose_solver).
				if (p_cam.normalized().dot(n_cam) > std::cos(M_PI - led.visibility_angle)) {
					continue;
				}
				float u, v;
				if (!t_camera_models_project(&models[sample->camera_index], (float)p_cam.x(),
				                             (float)p_cam.y(), (float)p_cam.z(), &u, &v) ||
				    u < 0 || v < 0 || u >= cal.image_size_pixels.w || v >= cal.image_size_pixels.h) {
					continue;
				}
				out.predicted++;
				for (uint32_t b = 0; b < sample->blob_count; b++) {
					double du = sample->blobs[b].center.x - u;
					double dv = sample->blobs[b].center.y - v;
					double d2 = du * du + dv * dv;
					if (d2 < kInlierGatePx * kInlierGatePx) {
						pairs.push_back(Pair{d2, l, b});
					}
				}
			}
			std::sort(pairs.begin(), pairs.end(), [](const Pair &a, const Pair &b) { return a.d2 < b.d2; });
			std::vector<bool> led_used(device.leds.size(), false);
			std::vector<bool> blob_used(sample->blob_count, false);
			for (const Pair &p : pairs) {
				if (led_used[p.led] || blob_used[p.blob]) {
					continue;
				}
				led_used[p.led] = true;
				blob_used[p.blob] = true;
				out.inliers++;
				sum2 += p.d2;
			}
		}
		if (out.inliers > 0) {
			out.rms_px = std::sqrt(sum2 / out.inliers);
		}
		return out;
	}

	struct DeviceScore
	{
		uint32_t solved{0};
		Stats reported_rms_px, eval_inliers, eval_rms_px, eval_ratio;
		//! Poses the recorded blobs do not support: under 4 inliers or under half the predicted LEDs found.
		uint32_t unsupported{0};
		Stats gt_mm, gt_deg;
		//! Poses further from the truth than the jump thresholds.
		uint32_t gt_wrong{0};
		Stats imu_residual_deg;
		uint32_t rotation_jumps{0}, position_jumps{0}, motion_pairs{0}, position_triples{0};
		Stats still_step_mm, still_step_deg;
		Stats static_rms_mm, static_rms_deg;
	};

	//! Spread of poses about their mean: RMS position distance (mm) and RMS angle to the chordal mean (deg).
	void
	spread(const std::vector<const FrontendRecord *> &poses, double &mm, double &deg)
	{
		Eigen::Vector3d mean = Eigen::Vector3d::Zero();
		Eigen::Vector4d qsum = Eigen::Vector4d::Zero();
		const Eigen::Quaterniond q0 = quat(poses[0]->Tcv_world_device.orientation);
		for (const FrontendRecord *r : poses) {
			mean += vec(r->Tcv_world_device.position);
			Eigen::Quaterniond q = quat(r->Tcv_world_device.orientation);
			if (q.dot(q0) < 0) {
				q.coeffs() = -q.coeffs();
			}
			qsum += q.coeffs();
		}
		mean /= (double)poses.size();
		Eigen::Quaterniond qmean;
		qmean.coeffs() = qsum.normalized();
		double s_mm = 0, s_deg = 0;
		for (const FrontendRecord *r : poses) {
			s_mm += (vec(r->Tcv_world_device.position) - mean).squaredNorm();
			double a = angle_deg(quat(r->Tcv_world_device.orientation), qmean);
			s_deg += a * a;
		}
		mm = 1000.0 * std::sqrt(s_mm / poses.size());
		deg = std::sqrt(s_deg / poses.size());
	}

} // namespace

bool
load_frontend_records(const DatasetReader &dataset,
                      const std::vector<int64_t> &exposure_times,
                      const char *path,
                      FrontendRun &out)
{
	std::ifstream in(path);
	if (!in) {
		std::fprintf(stderr, "cannot open %s\n", path);
		return false;
	}

	// Camera world poses by (exposure, camera).
	std::map<std::pair<int64_t, uint32_t>, xrt_pose> camera_pose;
	for (const CameraSample &s : dataset.samples) {
		int64_t i = exposure_index(exposure_times, s.timestamp_ns);
		if (i >= 0 && s.Txr_world_cam.has_value()) {
			xrt_pose cv;
			math_pose_convert_from_opencv(&s.Txr_world_cam.value(), &cv);
			camera_pose[{i, s.camera_index}] = cv;
		}
	}

	struct CameraPose
	{
		uint32_t camera;
		uint32_t matched;
		float rms_px;
		xrt_pose pose;
	};
	// Per (exposure, device): every camera's pose, and every camera's matches.
	std::map<std::pair<int64_t, int>, std::vector<CameraPose>> poses;
	std::map<std::pair<int64_t, int>, std::map<uint32_t, std::vector<FrontendMatch>>> matches;
	out.exposure_us.assign(exposure_times.size(), 0.0);
	bool have_cost = false;
	uint32_t unplaced = 0;

	std::string line;
	bool header = false;
	while (std::getline(in, line)) {
		if (line.empty()) {
			continue;
		}
		if (line[0] == '#') {
			if (line.find("constellation frontend records v1") != std::string::npos) {
				header = true;
				out.source = line.substr(1);
			}
			continue;
		}
		std::vector<std::string> f;
		std::stringstream ss(line);
		std::string cell;
		while (std::getline(ss, cell, ',')) {
			f.push_back(cell);
		}
		if (f.size() < 3) {
			continue;
		}
		int64_t t = std::strtoll(f[1].c_str(), nullptr, 10);
		int64_t i = exposure_index(exposure_times, t);
		if (i < 0) {
			unplaced++;
			continue;
		}
		if (f[0] == "R" && f.size() >= 13) {
			CameraPose p;
			int device = std::atoi(f[2].c_str());
			p.camera = (uint32_t)std::atoi(f[3].c_str());
			p.matched = (uint32_t)std::atoi(f[4].c_str());
			p.rms_px = (float)std::atof(f[5].c_str());
			p.pose.position = xrt_vec3{(float)std::atof(f[6].c_str()), (float)std::atof(f[7].c_str()),
			                           (float)std::atof(f[8].c_str())};
			p.pose.orientation = xrt_quat{(float)std::atof(f[9].c_str()), (float)std::atof(f[10].c_str()),
			                              (float)std::atof(f[11].c_str()), (float)std::atof(f[12].c_str())};
			math_quat_normalize(&p.pose.orientation);
			poses[{i, device}].push_back(p);
		} else if (f[0] == "M" && f.size() >= 7) {
			int device = std::atoi(f[2].c_str());
			uint32_t camera = (uint32_t)std::atoi(f[3].c_str());
			auto cam = camera_pose.find({i, camera});
			if (cam == camera_pose.end()) {
				continue;
			}
			FrontendMatch m{camera, cam->second,
			                xrt_vec2{(float)std::atof(f[5].c_str()), (float)std::atof(f[6].c_str())},
			                (uint32_t)std::atoi(f[4].c_str())};
			matches[{i, device}][camera].push_back(m);
		} else if (f[0] == "F" && f.size() >= 4) {
			out.exposure_us[i] += std::atof(f[3].c_str());
			have_cost = true;
		}
	}
	if (!header) {
		std::fprintf(stderr, "%s: not a constellation frontend records v1 file\n", path);
		return false;
	}
	if (!have_cost) {
		out.exposure_us.clear();
	}

	for (auto &[key, candidates] : poses) {
		const CameraPose *best = nullptr;
		for (const CameraPose &c : candidates) {
			if (best == nullptr || c.matched > best->matched ||
			    (c.matched == best->matched && c.rms_px < best->rms_px)) {
				best = &c;
			}
		}
		FrontendRecord r{exposure_times[key.first],
		                 (t_constellation_device_id_t)key.second,
		                 best->pose,
		                 best->rms_px,
		                 (uint32_t)candidates.size(),
		                 0,
		                 false,
		                 out.exposure_us.empty() ? 0.0 : out.exposure_us[key.first],
		                 {}};
		auto m = matches.find(key);
		if (m != matches.end()) {
			for (const CameraPose &c : candidates) {
				auto cm = m->second.find(c.camera);
				if (cm != m->second.end()) {
					r.correspondences.insert(r.correspondences.end(), cm->second.begin(),
					                         cm->second.end());
				}
			}
		}
		r.matches = (uint32_t)r.correspondences.size();
		out.records.push_back(std::move(r));
	}
	std::sort(out.records.begin(), out.records.end(), [](const FrontendRecord &a, const FrontendRecord &b) {
		return a.timestamp_ns != b.timestamp_ns ? a.timestamp_ns < b.timestamp_ns : a.device_id < b.device_id;
	});
	std::printf("imported %zu poses from %s%s\n", out.records.size(), path,
	            unplaced ? (" (" + std::to_string(unplaced) + " rows outside any exposure)").c_str() : "");
	return true;
}

int
frontend_compare(const DatasetReader &dataset,
                 const std::vector<int64_t> &exposure_times,
                 const std::vector<FrontendRun> &runs,
                 const char *out_csv)
{
	if (dataset.mosaics.empty() || runs.empty()) {
		return 1;
	}
	const DatasetMosaic &mosaic = dataset.mosaics[0];
	std::vector<t_camera_model_params> models(mosaic.camera_calibrations.size());
	for (size_t c = 0; c < models.size(); c++) {
		t_camera_model_params_from_t_camera_calibration(&mosaic.camera_calibrations[c], &models[c]);
	}
	const auto samples = samples_by_exposure(dataset, exposure_times);
	const size_t n = exposure_times.size();

	FILE *csv = out_csv ? std::fopen(out_csv, "w") : nullptr;
	if (csv) {
		std::fprintf(csv,
		             "frontend,timestamp_ns,device,px,py,pz,qx,qy,qz,qw,reported_rms_px,matches,eval_predicted,"
		             "eval_inliers,eval_rms_px,gt_mm,gt_deg,imu_residual_deg,still\n");
	}

	std::printf("\nfront-end comparison: %zu exposures; evaluator gate %.1f px; jumps > %.0f mm or > %.0f deg\n", n,
	            kInlierGatePx, kJumpMm, kJumpDeg);
	for (const FrontendRun &run : runs) {
		std::printf("  %s:%s%s\n", run.name.c_str(), run.source.empty() ? "" : " ", run.source.c_str());
	}

	for (const DatasetDevice &device : dataset.devices) {
		const t_constellation_device_id_t id = device.id;

		// Ground truth (CV) by exposure.
		std::vector<const DatasetGroundTruth *> truth(n, nullptr);
		for (const DatasetGroundTruth &g : dataset.ground_truth) {
			if (g.device_id == id) {
				int64_t i = exposure_index(exposure_times, g.timestamp_ns);
				if (i >= 0) {
					truth[i] = &g;
				}
			}
		}
		const bool have_truth = std::any_of(truth.begin(), truth.end(), [](auto *g) { return g != nullptr; });

		Gyro gyro;
		{
			std::vector<std::pair<int64_t, Eigen::Vector3d>> s;
			for (const DatasetImuSample &imu : dataset.imu_samples) {
				if (imu.device_id == id) {
					const xrt_vec3_f64 &w = imu.sample.gyro_rad_secs;
					s.push_back({imu.sample.timestamp_ns, Eigen::Vector3d(w.x, w.y, w.z)});
				}
			}
			std::sort(s.begin(), s.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
			for (const auto &[t, w] : s) {
				gyro.t.push_back(t);
				gyro.w.push_back(w);
			}
		}

		// Annotated static intervals for this device (or for every device).
		std::vector<std::pair<int64_t, int64_t>> intervals;
		{
			int64_t begin = -1;
			for (const DatasetAnnotation &a : dataset.annotations) {
				if (a.device_id != id && a.device_id != XRT_CONSTELLATION_INVALID_DEVICE_ID) {
					continue;
				}
				if (a.text == "static_begin") {
					begin = a.host_ns;
				} else if (a.text == "static_end" && begin >= 0) {
					intervals.push_back({begin, a.host_ns});
					begin = -1;
				}
			}
		}

		// Each run's poses by exposure.
		std::vector<std::vector<const FrontendRecord *>> by_exposure(runs.size());
		std::vector<bool> any_solved(n, false);
		for (size_t r = 0; r < runs.size(); r++) {
			by_exposure[r].assign(n, nullptr);
			for (const FrontendRecord &rec : runs[r].records) {
				if (rec.device_id != id) {
					continue;
				}
				int64_t i = exposure_index(exposure_times, rec.timestamp_ns);
				if (i >= 0) {
					by_exposure[r][i] = &rec;
					any_solved[i] = true;
				}
			}
		}
		const size_t union_solved = std::count(any_solved.begin(), any_solved.end(), true);
		size_t visible_truth = 0;
		for (size_t i = 0; i < n; i++) {
			if (truth[i] == nullptr) {
				continue;
			}
			xrt_pose cv;
			math_pose_convert_from_opencv(&truth[i]->Txr_world_device, &cv);
			visible_truth += reproject(device, cv, samples[i], models, mosaic).inliers >= 4 ? 1 : 0;
		}

		std::printf("\ndevice %d: %zu exposures solved by at least one front end", (int)id, union_solved);
		if (have_truth) {
			std::printf(", %zu where the true pose has >= 4 LEDs on blobs", visible_truth);
		}
		std::printf("; %zu gyro samples, %zu annotated static intervals\n", gyro.t.size(), intervals.size());

		for (size_t r = 0; r < runs.size(); r++) {
			DeviceScore s;
			const auto &poses = by_exposure[r];
			for (size_t i = 0; i < n; i++) {
				const FrontendRecord *rec = poses[i];
				if (rec == nullptr) {
					continue;
				}
				s.solved++;
				s.reported_rms_px.add(rec->rms_px);
				Reprojection rp = reproject(device, rec->Tcv_world_device, samples[i], models, mosaic);
				s.eval_inliers.add(rp.inliers);
				if (rp.inliers > 0) {
					s.eval_rms_px.add(rp.rms_px);
				}
				double ratio = rp.predicted ? (double)rp.inliers / rp.predicted : 0.0;
				s.eval_ratio.add(ratio);
				if (rp.inliers < 4 || ratio < 0.5) {
					s.unsupported++;
				}

				double gt_mm = NAN, gt_deg = NAN;
				if (truth[i] != nullptr) {
					xrt_pose cv;
					math_pose_convert_from_opencv(&truth[i]->Txr_world_device, &cv);
					gt_deg =
					    angle_deg(quat(rec->Tcv_world_device.orientation), quat(cv.orientation));
					s.gt_deg.add(gt_deg);
					if ((truth[i]->flags & T_CONSTELLATION_GROUND_TRUTH_ORIENTATION_ONLY) == 0) {
						gt_mm = 1000.0 *
						        (vec(rec->Tcv_world_device.position) - vec(cv.position)).norm();
						s.gt_mm.add(gt_mm);
					}
					s.gt_wrong += gt_deg > kJumpDeg || gt_mm > kJumpMm ? 1 : 0;
				}

				// Motion consistency with the previous pose, and the one before for a velocity.
				double imu_residual = NAN;
				const FrontendRecord *prev = nullptr;
				size_t prev_i = 0;
				for (size_t k = i; k-- > 0;) {
					if (exposure_times[i] - exposure_times[k] > kMaxPairGapNs) {
						break;
					}
					if (poses[k] != nullptr) {
						prev = poses[k];
						prev_i = k;
						break;
					}
				}
				if (prev != nullptr) {
					double gyro_angle;
					if (gyro.angle_between(exposure_times[prev_i], exposure_times[i], gyro_angle)) {
						double optical = angle_deg(quat(prev->Tcv_world_device.orientation),
						                           quat(rec->Tcv_world_device.orientation));
						imu_residual = std::fabs(optical - gyro_angle * 180.0 / M_PI);
						s.imu_residual_deg.add(imu_residual);
						s.motion_pairs++;
						s.rotation_jumps += imu_residual > kJumpDeg ? 1 : 0;
					}
					for (size_t k = prev_i; k-- > 0;) {
						if (exposure_times[prev_i] - exposure_times[k] > kMaxPairGapNs) {
							break;
						}
						if (poses[k] != nullptr) {
							Eigen::Vector3d p0 = vec(poses[k]->Tcv_world_device.position);
							Eigen::Vector3d p1 = vec(prev->Tcv_world_device.position);
							double dt01 =
							    (double)(exposure_times[prev_i] - exposure_times[k]);
							double dt12 =
							    (double)(exposure_times[i] - exposure_times[prev_i]);
							Eigen::Vector3d predicted = p1 + (p1 - p0) * (dt12 / dt01);
							double miss_mm =
							    1000.0 *
							    (vec(rec->Tcv_world_device.position) - predicted).norm();
							s.position_triples++;
							s.position_jumps += miss_mm > kJumpMm ? 1 : 0;
							break;
						}
					}
				}

				int still = gyro.still_at(exposure_times[i]);
				if (still == 1 && prev != nullptr && gyro.still_at(exposure_times[prev_i]) == 1) {
					s.still_step_mm.add(1000.0 * (vec(rec->Tcv_world_device.position) -
					                              vec(prev->Tcv_world_device.position))
					                                 .norm());
					s.still_step_deg.add(angle_deg(quat(prev->Tcv_world_device.orientation),
					                               quat(rec->Tcv_world_device.orientation)));
				}

				if (csv) {
					const xrt_pose &p = rec->Tcv_world_device;
					std::fprintf(
					    csv,
					    "%s,%" PRIi64
					    ",%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.4f,%u,%u,%u,%.4f,%.3f,%.3f,"
					    "%.3f,%d\n",
					    runs[r].name.c_str(), exposure_times[i], (int)id, p.position.x,
					    p.position.y, p.position.z, p.orientation.x, p.orientation.y,
					    p.orientation.z, p.orientation.w, rec->rms_px, rec->matches, rp.predicted,
					    rp.inliers, rp.rms_px, gt_mm, gt_deg, imu_residual, still);
				}
			}
			for (const auto &[begin, end] : intervals) {
				std::vector<const FrontendRecord *> inside;
				for (size_t i = 0; i < n; i++) {
					// Skip the half second either side of a button press.
					if (poses[i] != nullptr && exposure_times[i] > begin + 500'000'000 &&
					    exposure_times[i] < end - 500'000'000) {
						inside.push_back(poses[i]);
					}
				}
				if (inside.size() >= 10) {
					double mm, deg;
					spread(inside, mm, deg);
					s.static_rms_mm.add(mm);
					s.static_rms_deg.add(deg);
				}
			}

			std::printf("  %-10s solved %u (%.1f%% of exposures, %.1f%% of union", runs[r].name.c_str(),
			            s.solved, n ? 100.0 * s.solved / n : 0.0,
			            union_solved ? 100.0 * s.solved / union_solved : 0.0);
			if (have_truth) {
				std::printf(", %.1f%% of truly visible",
				            visible_truth ? 100.0 * s.solved / visible_truth : 0.0);
			}
			std::printf(")\n");
			std::printf(
			    "             own rms px p50 %.3f; evaluator: inliers p50 %.0f, rms px p50 %.3f p95 %.3f, "
			    "found/predicted p50 %.2f, unsupported %u (%.2f%%)\n",
			    s.reported_rms_px.pct(0.5), s.eval_inliers.pct(0.5), s.eval_rms_px.pct(0.5),
			    s.eval_rms_px.pct(0.95), s.eval_ratio.pct(0.5), s.unsupported,
			    s.solved ? 100.0 * s.unsupported / s.solved : 0.0);
			if (s.gt_deg.size() > 0) {
				std::printf(
				    "             ground truth: mm p50 %.2f p95 %.2f max %.1f; deg p50 %.3f p95 %.3f "
				    "max %.2f; "
				    "wrong (> %.0f mm or %.0f deg) %u\n",
				    s.gt_mm.pct(0.5), s.gt_mm.pct(0.95), s.gt_mm.pct(1.0), s.gt_deg.pct(0.5),
				    s.gt_deg.pct(0.95), s.gt_deg.pct(1.0), kJumpMm, kJumpDeg, s.gt_wrong);
			}
			std::printf(
			    "             gyro: rotation residual deg p50 %.3f p95 %.3f over %u pairs, %u > %.0f deg; "
			    "position jumps %u of %u\n",
			    s.imu_residual_deg.pct(0.5), s.imu_residual_deg.pct(0.95), s.motion_pairs, s.rotation_jumps,
			    kJumpDeg, s.position_jumps, s.position_triples);
			std::printf(
			    "             still (gyro): step mm rms %.3f p95 %.3f, deg rms %.4f, over %zu pairs\n",
			    s.still_step_mm.rms(), s.still_step_mm.pct(0.95), s.still_step_deg.rms(),
			    s.still_step_mm.size());
			if (s.static_rms_mm.size() > 0) {
				std::printf(
				    "             annotated static: spread mm p50 %.3f max %.3f, deg p50 %.4f, over "
				    "%zu "
				    "intervals\n",
				    s.static_rms_mm.pct(0.5), s.static_rms_mm.pct(1.0), s.static_rms_deg.pct(0.5),
				    s.static_rms_mm.size());
			}
		}

		// Agreement between front ends where both solved.
		for (size_t a = 0; a < runs.size(); a++) {
			for (size_t b = a + 1; b < runs.size(); b++) {
				Stats mm, deg;
				uint32_t only_a = 0, only_b = 0;
				for (size_t i = 0; i < n; i++) {
					const FrontendRecord *ra = by_exposure[a][i];
					const FrontendRecord *rb = by_exposure[b][i];
					if (ra != nullptr && rb != nullptr) {
						mm.add(1000.0 * (vec(ra->Tcv_world_device.position) -
						                 vec(rb->Tcv_world_device.position))
						                    .norm());
						deg.add(angle_deg(quat(ra->Tcv_world_device.orientation),
						                  quat(rb->Tcv_world_device.orientation)));
					} else if (ra != nullptr) {
						only_a++;
					} else if (rb != nullptr) {
						only_b++;
					}
				}
				std::printf(
				    "  %s vs %s: both %zu, only %s %u, only %s %u; difference mm p50 %.2f p95 %.2f, "
				    "deg "
				    "p50 %.3f p95 %.3f\n",
				    runs[a].name.c_str(), runs[b].name.c_str(), mm.size(), runs[a].name.c_str(), only_a,
				    runs[b].name.c_str(), only_b, mm.pct(0.5), mm.pct(0.95), deg.pct(0.5),
				    deg.pct(0.95));
			}
		}
	}

	// The exposure interval, for the share of exposures a front end could not keep up with.
	Stats intervals;
	for (size_t i = 1; i < n; i++) {
		intervals.add((double)(exposure_times[i] - exposure_times[i - 1]) * 1e-3);
	}
	const double interval_us = intervals.size() > 0 ? intervals.pct(0.5) : 0.0;
	std::printf("\nfront-end cost per exposure (all devices), exposure interval %.0f us:\n", interval_us);
	for (const FrontendRun &run : runs) {
		if (run.exposure_us.empty()) {
			std::printf("  %-10s unknown\n", run.name.c_str());
			continue;
		}
		Stats us;
		double total = 0;
		for (double v : run.exposure_us) {
			us.add(v);
			total += v;
		}
		const auto over = std::count_if(us.values.begin(), us.values.end(),
		                                [interval_us](double v) { return v > interval_us; });
		std::printf("  %-10s us mean %.0f p50 %.0f p95 %.0f max %.0f; %.1f%% of exposures over the interval\n",
		            run.name.c_str(), total / us.size(), us.pct(0.5), us.pct(0.95), us.pct(1.0),
		            100.0 * over / us.size());
	}
	if (csv) {
		std::fclose(csv);
	}
	return 0;
}

} // namespace xrt::tracking::constellation

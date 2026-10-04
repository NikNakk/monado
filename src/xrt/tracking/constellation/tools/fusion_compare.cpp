// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Offline comparison of the optical frontend, the branch's EKF and the experimental sliding-window fusion.
 *
 * Three paths consume identical input, the M1/M2 frontend's accepted solves of one recording plus its Sense IMU
 * samples:
 *  - "optical": the frontend's per-exposure poses as they are (what the M3 joint path pushes, open loop);
 *  - "ekf": those poses and the IMU through t_imu_optical_filter (PSSENSE_FILTER);
 *  - "swf": those solves' correspondences and the IMU through the MR 3015-derived sliding-window fusion.
 * The frontend runs once, open loop (no backend feeds its prior), so every path sees the same optical input.
 *
 * Every path is queried at every exposure, causally: IMU samples up to 20 ms past the exposure and optical input up
 * to and including it. Scenarios alter that shared input identically for all paths: "dropout" withholds the optical
 * input for 300 ms every 2 s, "corrupt" replaces three consecutive solves every 2 s with a self-consistent wrong lock.
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "fusion_compare.hpp"

#include "fusion/psvr2_fusion_frames.hpp"
#include "fusion/sliding_window_fusion.hpp"
#include "t_imu_optical_filter.h"

#include "math/m_api.h"
#include "tracking/t_camera_models.h"

#include <Eigen/Geometry>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace xrt::tracking::constellation {

namespace {

	using namespace fusion;

	constexpr int64_t kImuLookaheadNs = 20'000'000;
	constexpr int64_t kGapPeriodNs = 2'000'000'000;
	constexpr int64_t kGapLengthNs = 300'000'000;
	constexpr int64_t kCorruptOffsetNs = 1'000'000'000;
	constexpr int64_t kCorruptLengthNs = 50'000'000;
	//! Consecutive outputs further apart than this are not compared frame to frame.
	constexpr int64_t kConsecutiveNs = 25'000'000;
	//! Baseline of the IMU-versus-output orientation comparison.
	constexpr int64_t kImuBaselineNs = 100'000'000;

	struct Stats
	{
		std::vector<double> v;

		void
		add(double x)
		{
			v.push_back(x);
		}

		double
		pct(double p)
		{
			if (v.empty()) {
				return NAN;
			}
			std::sort(v.begin(), v.end());
			return v[std::min(v.size() - 1, (size_t)(p * (double)(v.size() - 1) + 0.5))];
		}

		double
		sum() const
		{
			double s = 0;
			for (double x : v) {
				s += x;
			}
			return s;
		}
	};

	Eigen::Quaterniond
	quat_of(const xrt_pose &p)
	{
		return Eigen::Quaterniond(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z)
		    .normalized();
	}

	Eigen::Vector3d
	pos_of(const xrt_pose &p)
	{
		return Eigen::Vector3d(p.position.x, p.position.y, p.position.z);
	}

	xrt_pose
	make_pose(const Eigen::Quaterniond &q, const Eigen::Vector3d &t)
	{
		xrt_pose p;
		p.orientation = xrt_quat{(float)q.x(), (float)q.y(), (float)q.z(), (float)q.w()};
		p.position = xrt_vec3{(float)t.x(), (float)t.y(), (float)t.z()};
		return p;
	}

	double
	deg_between(const Eigen::Quaterniond &a, const Eigen::Quaterniond &b)
	{
		return a.angularDistance(b) * 180.0 / M_PI;
	}

	/*
	 *
	 * Epochs from run.log.
	 *
	 */

	struct EpochEvent
	{
		int64_t t;
		char side;
		std::string kind;
	};

	/*!
	 * Events that change the controller's LED synchronisation, its clock mapping or the IMU preprocessing, placed
	 * at the host time of the LED_SCHEDULE line logged just before them (the log itself has no timestamps; the
	 * schedule lines come every ~8 ms per controller, so this is accurate to about a frame).
	 */
	std::vector<EpochEvent>
	parse_epoch_events(const char *path)
	{
		std::vector<EpochEvent> events;
		std::ifstream in(path);
		std::string line;
		int64_t now = 0;
		auto field = [](const std::string &l, const char *key) -> std::string {
			size_t at = l.find(key);
			if (at == std::string::npos) {
				return "";
			}
			at += std::strlen(key);
			size_t end = l.find(' ', at);
			return l.substr(at, end == std::string::npos ? std::string::npos : end - at);
		};
		while (std::getline(in, line)) {
			if (line.find("LED_SCHEDULE") != std::string::npos) {
				std::string n = field(line, " now=");
				if (!n.empty()) {
					now = std::stoll(n);
				}
				continue;
			}
			std::string side = field(line, " side=");
			if (side.empty()) {
				continue;
			}
			std::string kind;
			if (line.find("CLOCK_OFFSET") != std::string::npos &&
			    line.find("event=snap") != std::string::npos) {
				kind = "clock_snap";
			} else if (line.find("GYRO_BIAS") != std::string::npos &&
			           line.find("event=still") != std::string::npos) {
				kind = "gyro_bias";
			} else if (line.find("LED_BOOTSTRAP") != std::string::npos) {
				std::string e = field(line, " event=");
				if (e == "locked" || e == "lost" || e == "scan_start" || e == "stuck_lit") {
					kind = "led_" + e;
				}
			}
			if (!kind.empty()) {
				events.push_back(EpochEvent{now, side[0], kind});
			}
		}
		return events;
	}

	/*
	 *
	 * Paths and their outputs.
	 *
	 */

	struct Output
	{
		bool valid{false}; //!< position valid
		xrt_pose pose = XRT_POSE_IDENTITY;
		double us{0.0};
	};

	struct Input
	{
		const FrontendRecord *record{nullptr}; //!< what the paths see (null: nothing this exposure)
		const FrontendRecord *original{nullptr};
		bool hidden{false};
		bool corrupted{false};
		uint32_t epoch{0};
	};

	struct DeviceData
	{
		const DatasetDevice *device;
		char side;
		std::vector<xrt_imu_sample> imu_xr;          //!< IMU frame, XR, as recorded
		std::vector<int64_t> times;                  //!< every exposure
		std::vector<const FrontendRecord *> records; //!< per exposure, null if unsolved
		std::vector<std::vector<int64_t>> epoch_times;
	};

	//! Project LED @p led of the model at @p Tcv_world_device into a camera; false if behind it.
	bool
	project(const t_camera_model_params &model,
	        const xrt_pose &Tcv_world_cam,
	        const xrt_pose &Tcv_world_device,
	        const t_constellation_tracker_led &led,
	        Eigen::Vector2d &out)
	{
		const Eigen::Vector3d world =
		    quat_of(Tcv_world_device) * Eigen::Vector3d(led.position.x, led.position.y, led.position.z) +
		    pos_of(Tcv_world_device);
		const Eigen::Vector3d cam = quat_of(Tcv_world_cam).conjugate() * (world - pos_of(Tcv_world_cam));
		float u, v;
		if (cam.z() <= 0.01 ||
		    !t_camera_models_project(&model, (float)cam.x(), (float)cam.y(), (float)cam.z(), &u, &v)) {
			return false;
		}
		out = Eigen::Vector2d(u, v);
		return true;
	}

	//! RMS reprojection (pixels) of @p pose against a record's correspondences.
	double
	reprojection_rms(const std::vector<t_camera_model_params> &models,
	                 const DatasetDevice &device,
	                 const FrontendRecord &record,
	                 const xrt_pose &pose)
	{
		double sum2 = 0.0;
		int n = 0;
		for (const FrontendMatch &m : record.correspondences) {
			Eigen::Vector2d px;
			if (m.camera_index >= models.size() || m.led >= device.leds.size() ||
			    !project(models[m.camera_index], m.Tcv_world_cam, pose, device.leds[m.led], px)) {
				sum2 += 100.0 * 100.0;
			} else {
				sum2 += (px - Eigen::Vector2d(m.px.x, m.px.y)).squaredNorm();
			}
			n++;
		}
		return n > 0 ? std::sqrt(sum2 / n) : NAN;
	}

	//! A wrong but self-consistent lock: the ring turned 60 degrees about its axis and 8 cm to the side.
	FrontendRecord
	corrupt_record(const FrontendRecord &in,
	               const std::vector<t_camera_model_params> &models,
	               const DatasetDevice &device)
	{
		FrontendRecord out = in;
		const Eigen::Quaterniond q = quat_of(in.Tcv_world_device) *
		                             Eigen::Quaterniond(Eigen::AngleAxisd(M_PI / 3, Eigen::Vector3d::UnitY()));
		out.Tcv_world_device = make_pose(q, pos_of(in.Tcv_world_device) + Eigen::Vector3d(0.08, 0.0, 0.0));
		out.correspondences.clear();
		for (const FrontendMatch &m : in.correspondences) {
			Eigen::Vector2d px;
			if (m.camera_index < models.size() && project(models[m.camera_index], m.Tcv_world_cam,
			                                              out.Tcv_world_device, device.leds[m.led], px)) {
				FrontendMatch c = m;
				c.px = xrt_vec2{(float)px.x(), (float)px.y()};
				out.correspondences.push_back(c);
			}
		}
		return out;
	}

	std::vector<Output>
	run_optical(const std::vector<Input> &inputs)
	{
		std::vector<Output> out(inputs.size());
		for (size_t i = 0; i < inputs.size(); i++) {
			if (inputs[i].record != nullptr) {
				out[i].valid = true;
				out[i].pose = inputs[i].record->Tcv_world_device;
				out[i].us = inputs[i].record->solve_us;
			}
		}
		return out;
	}

	std::vector<Output>
	run_ekf(const DeviceData &dev, const std::vector<Input> &inputs, double imu_angle_deg)
	{
		t_imu_optical_filter_params params;
		t_imu_optical_filter_default_params(&params);
		t_imu_optical_filter *filter = t_imu_optical_filter_create(&params);
		const double half = imu_angle_deg * M_PI / 180.0 * 0.5;
		const xrt_quat imu_to_led{(float)-std::sin(half), 0.0f, 0.0f, (float)std::cos(half)};

		std::vector<Output> out(inputs.size());
		size_t next_imu = 0;
		for (size_t i = 0; i < inputs.size(); i++) {
			const int64_t t = dev.times[i];
			while (next_imu < dev.imu_xr.size() &&
			       dev.imu_xr[next_imu].timestamp_ns <= t + kImuLookaheadNs) {
				const xrt_imu_sample &s = dev.imu_xr[next_imu++];
				xrt_vec3 a{(float)s.accel_m_s2.x, (float)s.accel_m_s2.y, (float)s.accel_m_s2.z};
				xrt_vec3 g{(float)s.gyro_rad_secs.x, (float)s.gyro_rad_secs.y,
				           (float)s.gyro_rad_secs.z};
				xrt_vec3 a_led, g_led;
				math_quat_rotate_vec3(&imu_to_led, &a, &a_led);
				math_quat_rotate_vec3(&imu_to_led, &g, &g_led);
				t_imu_optical_filter_push_imu(filter, s.timestamp_ns, &a_led, &g_led);
			}
			auto start = std::chrono::steady_clock::now();
			if (inputs[i].record != nullptr) {
				const FrontendRecord &r = *inputs[i].record;
				const xrt_pose xr = pose_xr_cv(r.Tcv_world_device);
				const float scale = std::max(1.0f, r.rms_px / 0.5f);
				t_imu_optical_filter_push_pose(filter, t, &xr, 0.002f * scale, 0.008f * scale);
			}
			xrt_space_relation rel;
			if (t_imu_optical_filter_get_relation(filter, t, &rel) &&
			    (rel.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0) {
				out[i].valid = true;
				out[i].pose = pose_xr_cv(rel.pose);
			}
			out[i].us =
			    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
		}
		t_imu_optical_filter_stats st;
		t_imu_optical_filter_get_stats(filter, &st);
		std::printf("    ekf: updates %" PRIu64 " rejections %" PRIu64 " reinitialisations %" PRIu64
		            "; gyro bias deg/s %.2f %.2f %.2f\n",
		            st.updates, st.rejections, st.reinitialisations, st.gyro_bias_rad_s.x * 180 / M_PI,
		            st.gyro_bias_rad_s.y * 180 / M_PI, st.gyro_bias_rad_s.z * 180 / M_PI);
		t_imu_optical_filter_destroy(&filter);
		return out;
	}

	SlidingWindowFusionParams
	swf_params(double imu_angle_deg)
	{
		SlidingWindowFusionParams params;
		params.Q_imu_model = sense_q_imu_model(imu_angle_deg);
		// Tool-only overrides for offline sweeps.
		auto env = [](const char *name, double &value) {
			if (const char *v = std::getenv(name)) {
				value = std::strtod(v, nullptr);
			}
		};
		if (std::getenv("FUSION_UPSTREAM_NOISE") != nullptr) {
			params.noise = ImuNoiseModel::upstreamCv1();
			params.blob_sigma_px = 0.16;
		}
		env("FUSION_BLOB_SIGMA", params.blob_sigma_px);
		env("FUSION_GYRO_NOISE", params.noise.gyro_noise_density);
		env("FUSION_ACCEL_NOISE", params.noise.accel_noise_density);
		double window = params.window_size;
		env("FUSION_WINDOW", window);
		params.window_size = (uint32_t)window;
		double optimise = 0.0;
		env("FUSION_OPTIMISE_EXTRINSICS", optimise);
		params.optimize_extrinsics = optimise != 0.0;
		env("FUSION_GATE_PX", params.gate_reprojection_px);
		env("FUSION_GYRO_BIAS_ANCHOR", params.noise.gyro_bias_anchor_sigma);
		env("FUSION_MAX_GYRO_BIAS", params.noise.max_gyro_bias);
		double no_gate = 0.0;
		env("FUSION_NO_GATE", no_gate);
		params.gate = no_gate == 0.0;
		if (no_gate != 0.0) {
			params.post_solve_max_rms_px = 1e9;
			params.marginalise_max_rms_px = 1e9;
		}
		return params;
	}

	std::vector<Output>
	run_swf(const DeviceData &dev,
	        const std::vector<Input> &inputs,
	        const std::vector<t_camera_model_params> &models,
	        const SlidingWindowFusionParams &params,
	        FusionStats &stats_out,
	        std::map<std::string, uint32_t> &reasons)
	{
		SlidingWindowFusion fusion(params);
		std::vector<Output> out(inputs.size());
		// Tool-only: FUSION_TRACE=PATH appends every exposure's update result.
		FILE *trace =
		    std::getenv("FUSION_TRACE") != nullptr ? std::fopen(std::getenv("FUSION_TRACE"), "a") : nullptr;
		size_t next_imu = 0;
		for (size_t i = 0; i < inputs.size(); i++) {
			const int64_t t = dev.times[i];
			while (next_imu < dev.imu_xr.size() &&
			       dev.imu_xr[next_imu].timestamp_ns <= t + kImuLookaheadNs) {
				fusion.pushImu(imu_sample_xr_to_cv(dev.imu_xr[next_imu++]));
			}
			auto start = std::chrono::steady_clock::now();
			if (inputs[i].record != nullptr) {
				const FrontendRecord &r = *inputs[i].record;
				FusionExposure e;
				e.timestamp_ns = t;
				e.sync_epoch = inputs[i].epoch;
				e.Tcv_world_model_seed = r.Tcv_world_device;
				std::map<uint32_t, size_t> by_camera;
				for (const FrontendMatch &m : r.correspondences) {
					if (m.camera_index >= models.size()) {
						continue;
					}
					auto it = by_camera.find(m.camera_index);
					if (it == by_camera.end()) {
						FusionCameraObservation obs;
						obs.camera_index = m.camera_index;
						obs.Tcv_world_cam = m.Tcv_world_cam;
						obs.model = models[m.camera_index];
						e.observations.push_back(std::move(obs));
						it = by_camera.emplace(m.camera_index, e.observations.size() - 1).first;
					}
					const t_constellation_tracker_led &led = dev.device->leds[m.led];
					e.observations[it->second].points2d.emplace_back(m.px.x, m.px.y);
					e.observations[it->second].points3d.emplace_back(led.position.x, led.position.y,
					                                                 led.position.z);
				}
				FusionUpdateResult result = fusion.pushExposure(e);
				if (trace != nullptr) {
					const Eigen::Vector3d bg = fusion.stats().gyro_bias * 180.0 / M_PI;
					std::fprintf(trace,
					             "%" PRIi64
					             ",%d,%s,%s,%.4f,%.3f,%.3f,%d,%.0f,%u,%.2f,%.2f,%.2f,%.3f\n",
					             t, (int)dev.device->id, fusion_update_status_name(result.status),
					             result.reason.c_str(), result.gate_position_error_m,
					             result.gate_orientation_error_deg, result.solved_rms_px,
					             result.iterations, result.solve_us, e.sync_epoch, bg.x(), bg.y(),
					             bg.z(), result.gate_reprojection_px);
				}
				std::string key = fusion_update_status_name(result.status);
				if (!result.reason.empty()) {
					key += ":" + result.reason;
				}
				if (result.epoch_reset) {
					reasons["epoch_reset"]++;
				}
				reasons[key]++;
			}
			FusionPose pose = fusion.getPose(t);
			out[i].valid = pose.position_valid;
			out[i].pose = pose.Tcv_world_model;
			out[i].us =
			    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
		}
		stats_out = fusion.stats();
		if (trace != nullptr) {
			std::fclose(trace);
		}
		return out;
	}

	/*
	 *
	 * Metrics, one definition for every path.
	 *
	 */

	//! Gyro-integrated relative rotation of the LED model frame over [t0, t1] (CV), from the recorded IMU.
	Eigen::Quaterniond
	imu_relative_rotation(const std::vector<xrt_imu_sample> &imu_xr,
	                      int64_t t0,
	                      int64_t t1,
	                      const Eigen::Quaterniond &q_imu_model,
	                      int64_t shift_ns = 0)
	{
		Eigen::Quaterniond q = Eigen::Quaterniond::Identity();
		auto it = std::lower_bound(imu_xr.begin(), imu_xr.end(), t0 - shift_ns,
		                           [](const xrt_imu_sample &s, int64_t t) { return s.timestamp_ns < t; });
		int64_t cursor = t0;
		for (; it != imu_xr.end(); ++it) {
			const int64_t ts = it->timestamp_ns + shift_ns;
			const int64_t end = std::min(ts, t1);
			const xrt_vec3_f64 g = vec_xr_cv(it->gyro_rad_secs);
			const Eigen::Vector3d w(g.x, g.y, g.z);
			if (end > cursor) {
				q = q * quat_exp_so3(Eigen::Vector3d(w * ((end - cursor) * 1e-9)));
				cursor = end;
			}
			if (ts >= t1) {
				break;
			}
		}
		return (q_imu_model.conjugate() * q * q_imu_model).normalized();
	}

	struct Metrics
	{
		size_t exposures{0};
		size_t valid{0};
		double acquisition_ms{NAN};
		uint32_t dropouts{0};
		uint32_t dropouts_over_100ms{0};
		double dropout_total_s{0.0};
		Stats dropout_ms;
		Stats reproj_px;
		Stats jitter_mm, jitter_deg;
		Stats dv_m_s, dw_deg_s;
		Stats imu_residual_deg;
		uint32_t jumps{0};
		//! Outputs whose orientation disagrees by > 20 deg with the path's previous output (<= 0.5 s back)
		//! carried forward by the gyro: an adopted wrong lock, or a wrong lock left behind.
		uint32_t imu_inconsistent{0};
		uint32_t imu_checked{0};
		Stats us;
		// dropout scenario
		Stats hidden_mm, hidden_deg;
		//! Hidden exposures the path had no output for, scored by holding its last output.
		Stats hold_mm, hold_deg;
		size_t hidden{0}, hidden_valid{0};
		// corrupt scenario
		uint32_t corrupted{0}, followed{0};
		Stats recovery_ms, excursion_mm;
	};

	struct JitterWindow
	{
		size_t begin, end; //!< exposure index range
	};

	//! 1 s windows (from the first solve) where the nominal optical path solved >= 20 times and moved < 20 mm.
	std::vector<JitterWindow>
	static_windows(const DeviceData &dev, const std::vector<Output> &optical)
	{
		std::vector<JitterWindow> windows;
		size_t begin = 0;
		while (begin < optical.size() && !optical[begin].valid) {
			begin++;
		}
		while (begin < optical.size()) {
			size_t end = begin;
			while (end < optical.size() && dev.times[end] - dev.times[begin] < 1'000'000'000) {
				end++;
			}
			Eigen::Vector3d mean = Eigen::Vector3d::Zero();
			int n = 0;
			for (size_t i = begin; i < end; i++) {
				if (optical[i].valid) {
					mean += pos_of(optical[i].pose);
					n++;
				}
			}
			if (n >= 20) {
				mean /= n;
				double worst = 0.0;
				for (size_t i = begin; i < end; i++) {
					if (optical[i].valid) {
						worst = std::max(worst, (pos_of(optical[i].pose) - mean).norm());
					}
				}
				if (worst < 0.02) {
					windows.push_back({begin, end});
				}
			}
			begin = end;
		}
		return windows;
	}

	Metrics
	compute_metrics(const DeviceData &dev,
	                const std::vector<t_camera_model_params> &models,
	                const std::vector<Input> &inputs,
	                const std::vector<Output> &out,
	                const std::vector<JitterWindow> &windows,
	                const std::vector<Output> *nominal,
	                const Eigen::Quaterniond &q_imu_model)
	{
		Metrics m;
		// Evaluation interval: from the device's first frontend solve to the end.
		size_t first = 0;
		while (first < inputs.size() && inputs[first].original == nullptr) {
			first++;
		}
		if (first == inputs.size()) {
			return m;
		}

		bool seen_valid = false;
		int64_t last_valid_t = 0;
		size_t last_valid = 0;
		size_t run = 0;
		for (size_t i = first; i < out.size(); i++) {
			m.exposures++;
			if (inputs[i].record != nullptr) {
				m.us.add(out[i].us);
			}
			if (out[i].valid && seen_valid && dev.times[i] - last_valid_t <= 500'000'000) {
				const Eigen::Quaterniond predicted =
				    quat_of(out[last_valid].pose) *
				    imu_relative_rotation(dev.imu_xr, dev.times[last_valid], dev.times[i], q_imu_model);
				m.imu_checked++;
				m.imu_inconsistent += deg_between(predicted, quat_of(out[i].pose)) > 20.0 ? 1 : 0;
			}
			if (out[i].valid) {
				last_valid = i;
				m.valid++;
				if (!seen_valid) {
					m.acquisition_ms = (dev.times[i] - dev.times[first]) * 1e-6;
				} else if (run > 0) {
					const double ms = (dev.times[i] - last_valid_t) * 1e-6;
					m.dropouts++;
					m.dropouts_over_100ms += ms > 100.0 ? 1 : 0;
					m.dropout_ms.add(ms);
					m.dropout_total_s += ms * 1e-3;
				}
				seen_valid = true;
				last_valid_t = dev.times[i];
				run = 0;
			} else if (seen_valid) {
				run++;
			}

			// Fit against the frontend's (uncorrupted) correspondences where the path was given them.
			if (out[i].valid && inputs[i].record != nullptr && !inputs[i].corrupted &&
			    inputs[i].original != nullptr) {
				m.reproj_px.add(
				    reprojection_rms(models, *dev.device, *inputs[i].original, out[i].pose));
			}

			// Frame-to-frame continuity.
			if (i >= 2 && out[i].valid && out[i - 1].valid && out[i - 2].valid &&
			    dev.times[i] - dev.times[i - 2] < 2 * kConsecutiveNs) {
				const double dt1 = (dev.times[i] - dev.times[i - 1]) * 1e-9;
				const double dt0 = (dev.times[i - 1] - dev.times[i - 2]) * 1e-9;
				const Eigen::Vector3d v1 = (pos_of(out[i].pose) - pos_of(out[i - 1].pose)) / dt1;
				const Eigen::Vector3d v0 = (pos_of(out[i - 1].pose) - pos_of(out[i - 2].pose)) / dt0;
				m.dv_m_s.add((v1 - v0).norm());
				const Eigen::AngleAxisd a1(quat_of(out[i - 1].pose).conjugate() * quat_of(out[i].pose));
				const Eigen::AngleAxisd a0(quat_of(out[i - 2].pose).conjugate() *
				                           quat_of(out[i - 1].pose));
				const Eigen::Vector3d w1 = a1.axis() * a1.angle() / dt1;
				const Eigen::Vector3d w0 = a0.axis() * a0.angle() / dt0;
				m.dw_deg_s.add((w1 - w0).norm() * 180.0 / M_PI);
			}
			if (i >= 1 && out[i].valid && out[i - 1].valid &&
			    dev.times[i] - dev.times[i - 1] < kConsecutiveNs) {
				const double mm = 1000.0 * (pos_of(out[i].pose) - pos_of(out[i - 1].pose)).norm();
				const double deg = deg_between(quat_of(out[i].pose), quat_of(out[i - 1].pose));
				m.jumps += (mm > 30.0 || deg > 10.0) ? 1 : 0;
			}

			// Output's rotation over ~100 ms against the gyro's, in the LED model frame.
			size_t j = i;
			while (j + 1 < out.size() && dev.times[j] - dev.times[i] < kImuBaselineNs) {
				j++;
			}
			if (j > i && out[i].valid && out[j].valid &&
			    std::llabs(dev.times[j] - dev.times[i] - kImuBaselineNs) < 10'000'000) {
				const Eigen::Quaterniond rel = quat_of(out[i].pose).conjugate() * quat_of(out[j].pose);
				const Eigen::Quaterniond imu =
				    imu_relative_rotation(dev.imu_xr, dev.times[i], dev.times[j], q_imu_model);
				m.imu_residual_deg.add(deg_between(rel, imu));
			}

			// Hidden exposures (dropout scenario): against the withheld solve.
			if (inputs[i].hidden && inputs[i].original != nullptr) {
				m.hidden++;
				if (out[i].valid) {
					m.hidden_valid++;
					m.hidden_mm.add(1000.0 * (pos_of(out[i].pose) -
					                          pos_of(inputs[i].original->Tcv_world_device))
					                             .norm());
					m.hidden_deg.add(deg_between(quat_of(out[i].pose),
					                             quat_of(inputs[i].original->Tcv_world_device)));
				} else if (seen_valid) {
					const xrt_pose &held = out[last_valid].pose;
					m.hold_mm.add(
					    1000.0 *
					    (pos_of(held) - pos_of(inputs[i].original->Tcv_world_device)).norm());
					m.hold_deg.add(
					    deg_between(quat_of(held), quat_of(inputs[i].original->Tcv_world_device)));
				}
			}

			// Corrupted exposures: did the output follow the wrong lock?
			if (inputs[i].corrupted) {
				m.corrupted++;
				const xrt_pose &wrong = inputs[i].record->Tcv_world_device;
				if (out[i].valid && (pos_of(out[i].pose) - pos_of(wrong)).norm() < 0.02) {
					m.followed++;
				}
			}
		}

		// Recovery after each corrupted burst: from its last corrupted exposure until the output is back within
		// 5 mm / 2 deg of the same path's nominal output (1 s if never).
		if (nominal != nullptr) {
			for (size_t i = first; i < out.size(); i++) {
				if (!inputs[i].corrupted || (i > 0 && inputs[i - 1].corrupted)) {
					continue;
				}
				size_t burst_end = i;
				while (burst_end + 1 < out.size() && inputs[burst_end + 1].corrupted) {
					burst_end++;
				}
				double excursion = 0.0;
				double recovered_ms = 1000.0;
				for (size_t k = i;
				     k < out.size() && dev.times[k] - dev.times[burst_end] < 1'000'000'000; k++) {
					if (!(*nominal)[k].valid || !out[k].valid) {
						continue;
					}
					const double mm =
					    1000.0 * (pos_of(out[k].pose) - pos_of((*nominal)[k].pose)).norm();
					const double deg =
					    deg_between(quat_of(out[k].pose), quat_of((*nominal)[k].pose));
					excursion = std::max(excursion, mm);
					if (k > burst_end && mm < 5.0 && deg < 2.0) {
						recovered_ms = (dev.times[k] - dev.times[burst_end]) * 1e-6;
						break;
					}
				}
				m.excursion_mm.add(excursion);
				m.recovery_ms.add(recovered_ms);
			}
		}

		// Static jitter over the shared windows.
		for (const JitterWindow &w : windows) {
			std::vector<size_t> idx;
			for (size_t i = w.begin; i < w.end; i++) {
				if (out[i].valid) {
					idx.push_back(i);
				}
			}
			if (idx.size() < 20) {
				continue;
			}
			Eigen::Vector3d mean = Eigen::Vector3d::Zero();
			Eigen::Vector4d qsum = Eigen::Vector4d::Zero();
			const Eigen::Quaterniond ref = quat_of(out[idx[0]].pose);
			for (size_t i : idx) {
				mean += pos_of(out[i].pose);
				Eigen::Quaterniond q = quat_of(out[i].pose);
				if (q.dot(ref) < 0) {
					q.coeffs() = -q.coeffs();
				}
				qsum += q.coeffs();
			}
			mean /= (double)idx.size();
			Eigen::Quaterniond qmean;
			qmean.coeffs() = qsum.normalized();
			double sum_mm = 0.0, sum_deg = 0.0;
			for (size_t i : idx) {
				sum_mm += std::pow(1000.0 * (pos_of(out[i].pose) - mean).norm(), 2);
				sum_deg += std::pow(deg_between(quat_of(out[i].pose), qmean), 2);
			}
			m.jitter_mm.add(std::sqrt(sum_mm / idx.size()));
			m.jitter_deg.add(std::sqrt(sum_deg / idx.size()));
		}
		return m;
	}

	void
	print_metrics(const char *name, Metrics &m, bool dropout, bool corrupt)
	{
		std::printf(
		    "    %-7s valid %5.1f%% (%zu/%zu), acquire %.0f ms, dropouts %u (>100 ms: %u, total %.2f s, p50 "
		    "%.0f max %.0f ms)\n",
		    name, m.exposures ? 100.0 * m.valid / m.exposures : 0.0, m.valid, m.exposures, m.acquisition_ms,
		    m.dropouts, m.dropouts_over_100ms, m.dropout_total_s, m.dropout_ms.pct(0.5), m.dropout_ms.pct(1.0));
		std::printf(
		    "            reproj px p50 %.3f p95 %.3f; static jitter mm p50 %.2f p95 %.2f, deg p50 %.3f p95 "
		    "%.3f (%zu windows)\n",
		    m.reproj_px.pct(0.5), m.reproj_px.pct(0.95), m.jitter_mm.pct(0.5), m.jitter_mm.pct(0.95),
		    m.jitter_deg.pct(0.5), m.jitter_deg.pct(0.95), m.jitter_mm.v.size());
		std::printf(
		    "            |dv| m/s p50 %.3f p95 %.3f max %.2f; |dw| deg/s p50 %.1f p95 %.1f max %.0f; jumps %u; "
		    "IMU-inconsistent outputs %u of %u\n",
		    m.dv_m_s.pct(0.5), m.dv_m_s.pct(0.95), m.dv_m_s.pct(1.0), m.dw_deg_s.pct(0.5), m.dw_deg_s.pct(0.95),
		    m.dw_deg_s.pct(1.0), m.jumps, m.imu_inconsistent, m.imu_checked);
		std::printf(
		    "            IMU-vs-output rotation over 100 ms deg p50 %.2f p95 %.2f; us per optical input p50 "
		    "%.1f p95 %.1f max %.0f (total %.2f s)\n",
		    m.imu_residual_deg.pct(0.5), m.imu_residual_deg.pct(0.95), m.us.pct(0.5), m.us.pct(0.95),
		    m.us.pct(1.0), m.us.sum() * 1e-6);
		if (dropout) {
			std::printf(
			    "            hidden exposures %zu, output valid %zu (%.1f%%); vs withheld solve mm p50 "
			    "%.1f p95 %.1f, deg p50 %.2f p95 %.2f\n",
			    m.hidden, m.hidden_valid, m.hidden ? 100.0 * m.hidden_valid / m.hidden : 0.0,
			    m.hidden_mm.pct(0.5), m.hidden_mm.pct(0.95), m.hidden_deg.pct(0.5), m.hidden_deg.pct(0.95));
			if (!m.hold_mm.v.empty()) {
				std::printf(
				    "            holding the last output through the other %zu: mm p50 %.1f p95 %.1f, "
				    "deg p50 %.2f p95 %.2f\n",
				    m.hold_mm.v.size(), m.hold_mm.pct(0.5), m.hold_mm.pct(0.95), m.hold_deg.pct(0.5),
				    m.hold_deg.pct(0.95));
			}
		}
		if (corrupt) {
			std::printf(
			    "            corrupted %u, output followed the wrong lock %u; excursion from nominal mm "
			    "p50 %.1f max %.1f; recovery after the burst ms p50 %.0f max %.0f\n",
			    m.corrupted, m.followed, m.excursion_mm.pct(0.5), m.excursion_mm.pct(1.0),
			    m.recovery_ms.pct(0.5), m.recovery_ms.pct(1.0));
		}
	}

	/*!
	 * IMU-to-camera time offset: the shift of the IMU timestamps that best matches the gyro's 100 ms rotations to
	 * the optical ones. A diagnostic of the two host clock mappings (Sense Bluetooth vs PS VR2 VTS), not used by
	 * any path.
	 */
	void
	report_time_offset(const DeviceData &dev,
	                   const std::vector<Output> &optical,
	                   const Eigen::Quaterniond &q_imu_model)
	{
		double best_shift = 0.0, best = INFINITY, at_zero = NAN;
		for (int shift_ms = -40; shift_ms <= 40; shift_ms += 1) {
			Stats s;
			for (size_t i = 0; i < optical.size(); i += 3) {
				size_t j = i;
				while (j + 1 < optical.size() && dev.times[j] - dev.times[i] < kImuBaselineNs) {
					j++;
				}
				if (j == i || !optical[i].valid || !optical[j].valid ||
				    std::llabs(dev.times[j] - dev.times[i] - kImuBaselineNs) > 10'000'000) {
					continue;
				}
				const Eigen::Quaterniond rel =
				    quat_of(optical[i].pose).conjugate() * quat_of(optical[j].pose);
				const Eigen::Quaterniond imu = imu_relative_rotation(
				    dev.imu_xr, dev.times[i], dev.times[j], q_imu_model, (int64_t)shift_ms * 1'000'000);
				s.add(deg_between(rel, imu));
			}
			const double p50 = s.pct(0.5);
			if (shift_ms == 0) {
				at_zero = p50;
			}
			if (p50 < best) {
				best = p50;
				best_shift = shift_ms;
			}
		}
		std::printf(
		    "  IMU time shift that best matches optical rotation: %+.0f ms (residual p50 %.2f deg; at 0 ms "
		    "%.2f deg)\n",
		    best_shift, best, at_zero);
	}

} // namespace

int
fusion_compare(const DatasetReader &dataset,
               const std::vector<int64_t> &exposure_times,
               const std::vector<FrontendRecord> &records,
               const FusionCompareOptions &options)
{
	if (dataset.mosaics.empty() || dataset.imu_samples.empty()) {
		std::printf("fusion compare: the recording has no cameras or no IMU samples; nothing to compare\n");
		return 1;
	}
	std::vector<t_camera_model_params> models(dataset.mosaics[0].camera_calibrations.size());
	for (size_t c = 0; c < models.size(); c++) {
		t_camera_model_params_from_t_camera_calibration(&dataset.mosaics[0].camera_calibrations[c], &models[c]);
	}

	std::vector<EpochEvent> events;
	if (options.run_log != nullptr) {
		events = parse_epoch_events(options.run_log);
		std::map<std::string, uint32_t> kinds;
		for (const EpochEvent &e : events) {
			kinds[std::string(1, e.side) + ":" + e.kind]++;
		}
		std::printf("fusion compare: %zu epoch events from %s:", events.size(), options.run_log);
		for (const auto &[k, n] : kinds) {
			std::printf(" %s=%u", k.c_str(), n);
		}
		std::printf("\n");
	}

	const Eigen::Quaterniond q_imu_model = sense_q_imu_model(options.imu_angle_deg);

	for (const DatasetDevice &device : dataset.devices) {
		DeviceData dev;
		dev.device = &device;
		// The left ring's LED 0 sits at negative x (pssense_led_model.h).
		dev.side = !device.leds.empty() && device.leds[0].position.x < 0 ? 'L' : 'R';
		for (const DatasetImuSample &s : dataset.imu_samples) {
			if (s.device_id == device.id) {
				dev.imu_xr.push_back(s.sample);
			}
		}
		std::sort(dev.imu_xr.begin(), dev.imu_xr.end(), [](const xrt_imu_sample &a, const xrt_imu_sample &b) {
			return a.timestamp_ns < b.timestamp_ns;
		});
		dev.times = exposure_times;
		dev.records.assign(exposure_times.size(), nullptr);
		size_t solved = 0;
		for (const FrontendRecord &r : records) {
			if (r.device_id != device.id) {
				continue;
			}
			auto it = std::lower_bound(exposure_times.begin(), exposure_times.end(), r.timestamp_ns);
			if (it != exposure_times.end() && *it == r.timestamp_ns) {
				dev.records[it - exposure_times.begin()] = &r;
				solved++;
			}
		}
		// Solves on top of another device's solve in the same exposure: one ring fitted to the other's blobs.
		uint32_t near_other = 0;
		for (size_t i = 0; i < dev.records.size(); i++) {
			if (dev.records[i] == nullptr) {
				continue;
			}
			for (const FrontendRecord &r : records) {
				if (r.device_id != device.id && r.timestamp_ns == exposure_times[i] &&
				    (pos_of(r.Tcv_world_device) - pos_of(dev.records[i]->Tcv_world_device)).norm() <
				        0.06) {
					near_other++;
				}
			}
		}
		std::printf(
		    "fusion compare device %d (%c): %zu exposures, %zu frontend solves (%u within 60 mm of another "
		    "device's), %zu IMU samples\n",
		    (int)device.id, dev.side, exposure_times.size(), solved, near_other, dev.imu_xr.size());
		if (solved == 0 || dev.imu_xr.empty()) {
			continue;
		}

		// Epoch of each exposure: events of this controller up to then.
		std::vector<uint32_t> epochs(exposure_times.size(), 0);
		for (size_t i = 0; i < exposure_times.size(); i++) {
			for (const EpochEvent &e : events) {
				epochs[i] += (e.side == dev.side && e.t <= exposure_times[i]) ? 1 : 0;
			}
		}

		// The corrupted versions are made once, so every path sees the same wrong solves.
		std::vector<FrontendRecord> corrupted(exposure_times.size());
		int64_t t_first = 0;
		for (size_t i = 0; i < dev.records.size(); i++) {
			if (dev.records[i] != nullptr) {
				t_first = exposure_times[i];
				break;
			}
		}

		// The paths: "swf" is the upstream-style formulation (reprojection factors only); "swf_op" adds the
		// seed orientation prior (3 degrees, as M1's own IMU orientation prior).
		SlidingWindowFusionParams swf_plain = swf_params(options.imu_angle_deg);
		SlidingWindowFusionParams swf_op = swf_plain;
		swf_op.seed_orientation_sigma_deg = 3.0;
		const std::vector<std::string> names = {"optical", "ekf", "swf", "swf_op"};
		std::vector<std::vector<Output>> nominal(names.size());

		std::stringstream list(options.scenarios);
		std::string scenario;
		while (std::getline(list, scenario, ',')) {
			const bool dropout = scenario == "dropout";
			const bool corrupt = scenario == "corrupt";
			std::vector<Input> inputs(exposure_times.size());
			for (size_t i = 0; i < inputs.size(); i++) {
				Input &in = inputs[i];
				in.original = dev.records[i];
				in.record = dev.records[i];
				in.epoch = epochs[i];
				const int64_t phase = (exposure_times[i] - t_first) % kGapPeriodNs;
				if (dropout && in.record != nullptr && exposure_times[i] >= t_first &&
				    phase >= kGapPeriodNs - kGapLengthNs) {
					in.hidden = true;
					in.record = nullptr;
				}
				if (corrupt && in.record != nullptr && exposure_times[i] >= t_first &&
				    phase >= kCorruptOffsetNs && phase < kCorruptOffsetNs + kCorruptLengthNs) {
					corrupted[i] = corrupt_record(*in.record, models, device);
					in.record = &corrupted[i];
					in.corrupted = true;
				}
			}

			std::printf("  scenario %s\n", scenario.c_str());
			std::vector<std::vector<Output>> outputs;
			outputs.push_back(run_optical(inputs));
			outputs.push_back(run_ekf(dev, inputs, options.imu_angle_deg));
			for (const SlidingWindowFusionParams *params : {&swf_plain, &swf_op}) {
				FusionStats st;
				std::map<std::string, uint32_t> reasons;
				outputs.push_back(run_swf(dev, inputs, models, *params, st, reasons));
				const char *name = names[outputs.size() - 1].c_str();
				std::printf("    %s: accepted %" PRIu64 ", initialised %" PRIu64
				            ", rejected gate %" PRIu64 " / post-solve %" PRIu64 "; resets gap %" PRIu64
				            " rejections %" PRIu64 " epoch %" PRIu64 "; marginalisations %" PRIu64
				            ", factors kept out of the prior %" PRIu64 ", priors dropped %" PRIu64
				            "; peak IMU buffer %zu\n",
				            name, st.accepted, st.initialisations, st.rejected_gate,
				            st.rejected_post_solve, st.resets_gap, st.resets_rejections,
				            st.resets_epoch, st.marginalisations, st.factors_excluded_from_prior,
				            st.priors_dropped, st.peak_imu_buffer);
				const Eigen::AngleAxisd extrinsic_change(q_imu_model.conjugate() * st.Q_imu_model);
				std::printf(
				    "    %s: final gyro bias deg/s %.2f %.2f %.2f, accel bias m/s^2 %.3f %.3f %.3f, "
				    "IMU-to-model "
				    "change %.2f deg;",
				    name, st.gyro_bias.x() * 180 / M_PI, st.gyro_bias.y() * 180 / M_PI,
				    st.gyro_bias.z() * 180 / M_PI, st.accel_bias.x(), st.accel_bias.y(),
				    st.accel_bias.z(), extrinsic_change.angle() * 180.0 / M_PI);
				for (const auto &[k, n] : reasons) {
					std::printf(" %s=%u", k.c_str(), n);
				}
				std::printf("\n");
			}

			if (scenario == "nominal") {
				nominal = outputs;
				report_time_offset(dev, outputs[0], q_imu_model);
			}
			// Jitter windows come from the unaltered optical input, the same for every path and scenario.
			const std::vector<JitterWindow> windows =
			    static_windows(dev, nominal[0].empty() ? outputs[0] : nominal[0]);

			for (size_t p = 0; p < outputs.size(); p++) {
				Metrics m = compute_metrics(dev, models, inputs, outputs[p], windows,
				                            nominal[p].empty() ? nullptr : &nominal[p], q_imu_model);
				print_metrics(names[p].c_str(), m, dropout, corrupt);
			}

			if (options.out_prefix != nullptr) {
				std::string path = std::string(options.out_prefix) + "-" + scenario + ".csv";
				const bool first_device = device.id == dataset.devices.front().id;
				FILE *f = std::fopen(path.c_str(), first_device ? "w" : "a");
				if (first_device) {
					std::fprintf(f,
					             "timestamp_ns,device,input,hidden,corrupted,epoch,path,valid,px,"
					             "py,pz,qx,qy,qz,qw,us\n");
				}
				for (size_t i = 0; i < inputs.size(); i++) {
					for (size_t p = 0; p < outputs.size(); p++) {
						const Output &o = outputs[p][i];
						std::fprintf(
						    f,
						    "%" PRIi64
						    ",%d,%d,%d,%d,%u,%s,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.1f\n",
						    exposure_times[i], (int)device.id, inputs[i].record != nullptr,
						    inputs[i].hidden, inputs[i].corrupted, inputs[i].epoch,
						    names[p].c_str(), o.valid, o.pose.position.x, o.pose.position.y,
						    o.pose.position.z, o.pose.orientation.x, o.pose.orientation.y,
						    o.pose.orientation.z, o.pose.orientation.w, o.us);
					}
				}
				std::fclose(f);
			}
		}
	}
	return 0;
}

} // namespace xrt::tracking::constellation

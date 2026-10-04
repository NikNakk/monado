// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Coordinate, timing and robustness tests for the experimental PS Sense sliding-window fusion.
 * @author Nick Kennedy
 */

#include "fusion/imu_preintegration.hpp"
#include "fusion/psvr2_fusion_frames.hpp"
#include "fusion/sliding_window_fusion.hpp"

// Ceres brings in glog, whose CHECK would shadow Catch's.
#undef CHECK
#include "catch_amalgamated.hpp"

#include "tracking/t_camera_models.h"

#include <Eigen/Geometry>

#include <cmath>
#include <random>
#include <vector>

using namespace xrt::tracking::constellation::fusion;

namespace {

constexpr double kG = 9.80665;

xrt_pose
make_pose(const Eigen::Quaterniond &q, const Eigen::Vector3d &t)
{
	xrt_pose p;
	p.orientation = xrt_quat{(float)q.x(), (float)q.y(), (float)q.z(), (float)q.w()};
	p.position = xrt_vec3{(float)t.x(), (float)t.y(), (float)t.z()};
	return p;
}

Eigen::Quaterniond
quat_of(const xrt_pose &p)
{
	return Eigen::Quaterniond(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z).normalized();
}

Eigen::Vector3d
pos_of(const xrt_pose &p)
{
	return Eigen::Vector3d(p.position.x, p.position.y, p.position.z);
}

Eigen::Isometry3d
iso_of(const xrt_pose &p)
{
	Eigen::Isometry3d i = Eigen::Isometry3d::Identity();
	i.linear() = quat_of(p).toRotationMatrix();
	i.translation() = pos_of(p);
	return i;
}

double
angle_deg(const Eigen::Quaterniond &a, const Eigen::Quaterniond &b)
{
	return a.angularDistance(b) * 180.0 / M_PI;
}

xrt_imu_sample
imu_sample(int64_t t, const Eigen::Vector3d &accel, const Eigen::Vector3d &gyro)
{
	xrt_imu_sample s{};
	s.timestamp_ns = t;
	s.accel_m_s2 = {accel.x(), accel.y(), accel.z()};
	s.gyro_rad_secs = {gyro.x(), gyro.y(), gyro.z()};
	return s;
}

/*
 * A synthetic PS VR2 + Sense scene, all in the OpenCV convention: a moving headset carrying four KB4 cameras, and a
 * 17-LED ring (like tests_joint_pose_solver's) whose IMU sits at the Sense mounting angle.
 */
struct Scene
{
	std::vector<Eigen::Vector3d> leds;
	std::vector<Eigen::Vector3d> normals;
	t_camera_model_params model{};
	std::vector<xrt_pose> hmd_from_camera;
	Eigen::Quaterniond q_imu_model = sense_q_imu_model();

	double head_yaw_rate{0.0}; //!< rad/s
	double head_sway_m{0.0};

	Scene()
	{
		static const double jitter[17] = {0.0,   0.21, -0.13, 0.34, -0.27, 0.08, 0.41, -0.19, 0.15,
		                                  -0.36, 0.29, -0.05, 0.23, -0.31, 0.11, 0.38, -0.22};
		static const double height_mm[17] = {-6, 4, 9, -2, -8, 7, 1, -5, 10, -9, 3, 6, -4, 8, -7, 2, -1};
		for (int i = 0; i < 17; i++) {
			double a = 2.0 * M_PI * (i + jitter[i]) / 17.0;
			leds.emplace_back(0.045 * std::cos(a), 0.045 * std::sin(a), height_mm[i] / 1000.0);
			normals.push_back(Eigen::Vector3d(std::cos(a), std::sin(a), -0.8).normalized());
		}
		model.fx = model.fy = 188.0f;
		model.cx = model.cy = 254.0f;
		model.fisheye = t_camera_calibration_kb4_params_float{0.05f, -0.01f, 0.002f, -0.0005f};
		model.model = T_DISTORTION_FISHEYE_KB4;
		auto yaw_pitch = [](double yaw_deg, double pitch_deg) {
			return Eigen::Quaterniond(
			    Eigen::AngleAxisd(yaw_deg * M_PI / 180.0, Eigen::Vector3d::UnitY()) *
			    Eigen::AngleAxisd(pitch_deg * M_PI / 180.0, Eigen::Vector3d::UnitX()));
		};
		hmd_from_camera.push_back(make_pose(yaw_pitch(-20, 20), Eigen::Vector3d(-0.0405, 0.03, 0)));
		hmd_from_camera.push_back(make_pose(yaw_pitch(20, 20), Eigen::Vector3d(0.0405, 0.03, 0)));
		hmd_from_camera.push_back(make_pose(yaw_pitch(-35, -15), Eigen::Vector3d(-0.075, -0.04, -0.02)));
		hmd_from_camera.push_back(make_pose(yaw_pitch(35, -15), Eigen::Vector3d(0.075, -0.04, -0.02)));
	}

	xrt_pose
	hmd(double t) const
	{
		Eigen::Quaterniond q(Eigen::AngleAxisd(head_yaw_rate * t, Eigen::Vector3d::UnitY()));
		return make_pose(q, Eigen::Vector3d(head_sway_m * std::sin(1.3 * t), 0.02 * std::sin(0.7 * t), 0));
	}

	//! LED model pose in the world. Faces the headset (ring axis roughly -z) while moving and turning.
	void
	model_state(double t, Eigen::Vector3d &p, Eigen::Quaterniond &q) const
	{
		p = Eigen::Vector3d(0.08 * std::sin(1.1 * t), 0.05 * std::sin(1.7 * t + 0.3),
		                    0.40 + 0.05 * std::sin(0.9 * t));
		q = Eigen::AngleAxisd(0.35 * std::sin(1.2 * t), Eigen::Vector3d::UnitX()) *
		    Eigen::AngleAxisd(0.40 * std::sin(0.8 * t + 0.5), Eigen::Vector3d::UnitY()) *
		    Eigen::AngleAxisd(0.8 * t, Eigen::Vector3d::UnitZ());
	}

	xrt_pose
	model_pose(double t) const
	{
		Eigen::Vector3d p;
		Eigen::Quaterniond q;
		model_state(t, p, q);
		return make_pose(q, p);
	}

	//! IMU reading at t (IMU frame, CV): body rate and specific force, by central differences.
	xrt_imu_sample
	imu(int64_t t_ns) const
	{
		const double t = t_ns * 1e-9;
		const double h = 1e-4;
		Eigen::Vector3d p0, p1, p2;
		Eigen::Quaterniond q0, q1, q2;
		model_state(t - h, p0, q0);
		model_state(t, p1, q1);
		model_state(t + h, p2, q2);
		const Eigen::Quaterniond qi0 = q0 * q_imu_model.conjugate();
		const Eigen::Quaterniond qi2 = q2 * q_imu_model.conjugate();
		const Eigen::Quaterniond qi1 = q1 * q_imu_model.conjugate();
		const Eigen::AngleAxisd d(qi0.conjugate() * qi2);
		const Eigen::Vector3d gyro = d.axis() * d.angle() / (2 * h);
		const Eigen::Vector3d accel_world = (p2 - 2 * p1 + p0) / (h * h);
		const Eigen::Vector3d accel = qi1.conjugate() * (accel_world - Eigen::Vector3d(0, kG, 0));
		return imu_sample(t_ns, accel, gyro);
	}

	//! Every camera's view of the ring at @p t, seen from @p model_world (normally the truth).
	FusionExposure
	exposure(int64_t t_ns,
	         const xrt_pose &model_world,
	         double noise_px,
	         std::mt19937 &rng,
	         bool freeze_cameras = false,
	         double freeze_t = 0.0) const
	{
		const double t = t_ns * 1e-9;
		std::normal_distribution<double> noise(0.0, noise_px);
		FusionExposure e;
		e.timestamp_ns = t_ns;
		e.Tcv_world_model_seed = model_world;
		const xrt_pose head = hmd(freeze_cameras ? freeze_t : t);
		const xrt_pose head_now = hmd(t);
		const Eigen::Isometry3d M = iso_of(model_world);
		for (size_t c = 0; c < hmd_from_camera.size(); c++) {
			FusionCameraObservation obs;
			obs.camera_index = (uint32_t)c;
			obs.Tcv_world_cam = world_from_camera(head, hmd_from_camera[c]);
			obs.model = model;
			// Rendered from where the camera really was; the pose handed over may be frozen.
			const Eigen::Isometry3d C_inv =
			    iso_of(world_from_camera(head_now, hmd_from_camera[c])).inverse();
			for (size_t i = 0; i < leds.size(); i++) {
				Eigen::Vector3d p = C_inv * (M * leds[i]);
				Eigen::Vector3d n = C_inv.linear() * (M.linear() * normals[i]);
				if (p.z() < 0.05 || p.normalized().dot(n) > std::cos(M_PI - 80.0 * M_PI / 180.0)) {
					continue;
				}
				float u, v;
				if (!t_camera_models_project(&model, (float)p.x(), (float)p.y(), (float)p.z(), &u,
				                             &v) ||
				    u < 0 || v < 0 || u >= 508 || v >= 508) {
					continue;
				}
				obs.points2d.emplace_back(u + noise(rng), v + noise(rng));
				obs.points3d.push_back(leds[i]);
			}
			if (obs.points2d.size() >= 3) {
				e.observations.push_back(obs);
			}
		}
		return e;
	}
};

SlidingWindowFusionParams
test_params(const Scene &scene)
{
	SlidingWindowFusionParams params;
	params.Q_imu_model = scene.q_imu_model;
	params.optimize_extrinsics = false;
	// Synthetic IMU: clean apart from the 15 ms zero-order hold.
	params.noise.gyro_noise_density = 0.005;
	params.noise.accel_noise_density = 0.05;
	params.blob_sigma_px = 0.3;
	return params;
}

struct RunErrors
{
	double max_mm{0.0};
	double max_deg{0.0};
	double rms_mm{0.0};
	uint32_t accepted{0};
	uint32_t rejected{0};
};

/*!
 * Run the scene for @p seconds: 66.7 Hz IMU (15 ms, not a multiple of the 60 Hz camera period), exposures at 60 Hz
 * with @p hidden(t) exposures withheld. Errors are of the fused pose at each exposure against the truth.
 */
template <typename Hidden, typename Mutate>
RunErrors
run_scene(const Scene &scene,
          SlidingWindowFusion &fusion,
          double seconds,
          Hidden hidden,
          Mutate mutate,
          std::mt19937 &rng,
          double seed_noise_mm = 0.5,
          double seed_noise_deg = 0.2,
          double from_s = 0.3)
{
	std::normal_distribution<double> n(0.0, 1.0);
	RunErrors errors;
	double sum2 = 0.0;
	int count = 0;
	int64_t next_imu = 0;
	const int64_t imu_period = 15'000'000;
	const int64_t cam_period = 16'683'000;
	for (int64_t t = cam_period; t < (int64_t)(seconds * 1e9); t += cam_period) {
		// IMU up to 5 ms past the exposure, so its tail is available.
		while (next_imu <= t + 5'000'000) {
			fusion.pushImu(scene.imu(next_imu));
			next_imu += imu_period;
		}
		const xrt_pose truth = scene.model_pose(t * 1e-9);
		if (!hidden(t * 1e-9)) {
			Eigen::Quaterniond dq(Eigen::AngleAxisd(seed_noise_deg * M_PI / 180.0 * n(rng),
			                                        Eigen::Vector3d(n(rng), n(rng), n(rng)).normalized()));
			xrt_pose seed =
			    make_pose(dq * quat_of(truth),
			              pos_of(truth) + 0.001 * seed_noise_mm * Eigen::Vector3d(n(rng), n(rng), n(rng)));
			FusionExposure e = scene.exposure(t, truth, 0.2, rng);
			e.Tcv_world_model_seed = seed;
			mutate(t * 1e-9, e);
			FusionUpdateResult r = fusion.pushExposure(e);
			if (r.status == FusionUpdateStatus::Accepted) {
				errors.accepted++;
			}
			if (r.status == FusionUpdateStatus::RejectedGate ||
			    r.status == FusionUpdateStatus::RejectedPostSolve) {
				errors.rejected++;
			}
		}
		if (t * 1e-9 < from_s) {
			continue;
		}
		FusionPose pose = fusion.getPose(t);
		REQUIRE(pose.orientation_valid);
		const double mm = 1000.0 * (pos_of(pose.Tcv_world_model) - pos_of(truth)).norm();
		const double deg = angle_deg(quat_of(pose.Tcv_world_model), quat_of(truth));
		errors.max_mm = std::max(errors.max_mm, mm);
		errors.max_deg = std::max(errors.max_deg, deg);
		sum2 += mm * mm;
		count++;
	}
	errors.rms_mm = count ? std::sqrt(sum2 / count) : 0.0;
	return errors;
}

auto never = [](double) { return false; };
auto unchanged = [](double, FusionExposure &) {};

} // namespace

TEST_CASE("HMD-to-camera composition gives the camera's world pose")
{
	Scene scene;
	const xrt_pose head =
	    make_pose(Eigen::Quaterniond(Eigen::AngleAxisd(0.7, Eigen::Vector3d(0.3, 1, 0.2).normalized())),
	              Eigen::Vector3d(0.3, -1.6, 0.4));
	for (const xrt_pose &hmd_from_cam : scene.hmd_from_camera) {
		const xrt_pose world_cam = world_from_camera(head, hmd_from_cam);
		const Eigen::Isometry3d expected = iso_of(head) * iso_of(hmd_from_cam);
		CHECK((pos_of(world_cam) - expected.translation()).norm() < 1e-6);
		CHECK(angle_deg(quat_of(world_cam), Eigen::Quaterniond(expected.linear())) < 1e-3);

		// A world point seen by the composed camera equals the head's view of it seen from the camera.
		const Eigen::Vector3d world_point(0.1, -1.5, 0.9);
		const Eigen::Vector3d in_cam = iso_of(world_cam).inverse() * world_point;
		const Eigen::Vector3d via_head =
		    iso_of(hmd_from_cam).inverse() * (iso_of(head).inverse() * world_point);
		CHECK((in_cam - via_head).norm() < 1e-6);
	}

	// Recovering the head from camera 0, which world-frame recordings store as head * head_from_camera0.
	const xrt_pose head_from_camera0 = scene.hmd_from_camera[0];
	const xrt_pose camera0 = world_from_camera(head, head_from_camera0);
	const xrt_pose recovered = hmd_from_world_camera0(camera0, head_from_camera0);
	CHECK((pos_of(recovered) - pos_of(head)).norm() < 1e-6);
	CHECK(angle_deg(quat_of(recovered), quat_of(head)) < 1e-3);
}

TEST_CASE("XR and CV conventions and the Sense IMU mounting")
{
	// The pose conversion is conjugation by a 180 degree rotation about x, and is its own inverse.
	const Eigen::Quaterniond C(Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitX()));
	const xrt_pose xr = make_pose(Eigen::Quaterniond(Eigen::AngleAxisd(0.9, Eigen::Vector3d(1, 2, 3).normalized())),
	                              Eigen::Vector3d(0.1, 0.2, 0.3));
	const xrt_pose cv = pose_xr_cv(xr);
	CHECK((pos_of(cv) - C * pos_of(xr)).norm() < 1e-6);
	CHECK(angle_deg(quat_of(cv), C * quat_of(xr) * C.conjugate()) < 1e-3);
	const xrt_pose back = pose_xr_cv(cv);
	CHECK((pos_of(back) - pos_of(xr)).norm() < 1e-7);
	CHECK(angle_deg(quat_of(back), quat_of(xr)) < 1e-4);
	// Right-handed in both: the determinant of the converted rotation stays +1.
	CHECK(quat_of(cv).toRotationMatrix().determinant() == Catch::Approx(1.0).margin(1e-6));

	// An accelerometer at rest reads +g up in XR, i.e. -g along CV's y (down); with the CV world gravity along +y
	// the preintegration's world acceleration q a + g is zero.
	const xrt_imu_sample rest_xr = imu_sample(0, Eigen::Vector3d(0, kG, 0), Eigen::Vector3d(0.1, 0.2, 0.3));
	const xrt_imu_sample rest_cv = imu_sample_xr_to_cv(rest_xr);
	const Eigen::Vector3d a_cv(rest_cv.accel_m_s2.x, rest_cv.accel_m_s2.y, rest_cv.accel_m_s2.z);
	CHECK((a_cv + WorldGravity<double>().toVector()).norm() < 1e-9);
	CHECK(rest_cv.gyro_rad_secs.y == Catch::Approx(-0.2));
	CHECK(rest_cv.gyro_rad_secs.z == Catch::Approx(-0.3));

	// The extrinsic maps model to IMU; its inverse is the EKF path's imu_to_led rotation, in either convention.
	const double half = kSenseImuAngleDeg * M_PI / 180.0 * 0.5;
	const Eigen::Quaterniond imu_to_led(std::cos(half), -std::sin(half), 0, 0);
	CHECK(angle_deg(sense_q_imu_model().conjugate(), imu_to_led) < 1e-6);
	CHECK(angle_deg(C * sense_q_imu_model() * C.conjugate(), sense_q_imu_model()) < 1e-6);
}

TEST_CASE("Preintegration handles camera timestamps between IMU samples")
{
	// Constant rate and specific force: whatever the sample phase, the result is exact.
	const Eigen::Vector3d gyro(0.3, -0.5, 0.8);
	const Eigen::Vector3d accel(1.0, -2.0, 0.5);
	std::vector<xrt_imu_sample> all;
	for (int64_t t = 0; t <= 200'000'000; t += 15'000'000) {
		all.push_back(imu_sample(t, accel, gyro));
	}
	const int64_t start = 16'683'000; // between samples at 15 and 30 ms
	const int64_t end = 50'049'000;   // between 45 and 60 ms
	std::vector<xrt_imu_sample> inside;
	xrt_imu_sample after{};
	for (const auto &s : all) {
		if (s.timestamp_ns > start && s.timestamp_ns <= end) {
			inside.push_back(s);
		} else if (s.timestamp_ns > end) {
			after = s;
			break;
		}
	}
	const ImuNoiseModel noise = ImuNoiseModel::psSense();
	PreintegratedImuSamples p = preintegrate(inside, after, ImuBias<double>().seed(), start, end, noise);
	const double dt = (end - start) * 1e-9;
	CHECK(p.dt == Catch::Approx(dt));
	CHECK(p.num_steps == inside.size() + 1); // head, between samples, tail
	CHECK(angle_deg(p.Q_start_end, Eigen::Quaterniond(Eigen::AngleAxisd(gyro.norm() * dt, gyro.normalized()))) <
	      1e-6);
	// No sample inside the interval: the next one covers it.
	PreintegratedImuSamples q = preintegrate({}, all[2], ImuBias<double>().seed(), 16'683'000, 29'000'000, noise);
	CHECK(q.num_steps == 1);
	CHECK(angle_deg(q.Q_start_end,
	                Eigen::Quaterniond(Eigen::AngleAxisd(gyro.norm() * 12.317e-3, gyro.normalized()))) < 1e-6);
	// The whitening is finite and positive.
	CHECK(p.whitening.allFinite());
	CHECK(p.whitening(0, 0) > 0.0);
}

TEST_CASE("Preintegration composes across an optical keyframe boundary")
{
	Scene scene;
	std::vector<xrt_imu_sample> all;
	for (int64_t t = 0; t <= 400'000'000; t += 15'000'000) {
		all.push_back(scene.imu(t));
	}
	auto integrate = [&](int64_t a, int64_t b) {
		std::vector<xrt_imu_sample> inside;
		xrt_imu_sample after{};
		for (const auto &s : all) {
			if (s.timestamp_ns > a && s.timestamp_ns <= b) {
				inside.push_back(s);
			} else if (s.timestamp_ns > b) {
				after = s;
				break;
			}
		}
		return preintegrate(inside, after, ImuBias<double>().seed(), a, b, ImuNoiseModel::psSense());
	};
	const int64_t t0 = 16'683'000, t1 = 133'464'000, t2 = 250'245'000; // t1 falls between samples
	const PreintegratedImuSamples a = integrate(t0, t1);
	const PreintegratedImuSamples b = integrate(t1, t2);
	const PreintegratedImuSamples ab = integrate(t0, t2);

	const Eigen::Quaterniond q = a.Q_start_end * b.Q_start_end;
	const Eigen::Vector3d dv = a.delta_velocity + a.Q_start_end * b.delta_velocity;
	const Eigen::Vector3d dp = a.delta_position + a.delta_velocity * b.dt + a.Q_start_end * b.delta_position;
	// Splitting at t1 holds the reading that ends the interval over a shorter span, so allow for the hold.
	CHECK(angle_deg(q, ab.Q_start_end) < 0.05);
	CHECK((dv - ab.delta_velocity).norm() < 0.01);
	CHECK((dp - ab.delta_position).norm() < 0.001);
	CHECK(ab.dt == Catch::Approx(a.dt + b.dt));
}

TEST_CASE("Fusion tracks a moving controller seen by moving cameras")
{
	Scene scene;
	scene.head_yaw_rate = 0.35; // 20 deg/s
	scene.head_sway_m = 0.1;
	std::mt19937 rng(3);

	SlidingWindowFusion fusion(test_params(scene));
	RunErrors moving = run_scene(scene, fusion, 3.0, never, unchanged, rng);
	CAPTURE(moving.max_mm, moving.max_deg, moving.rms_mm, moving.accepted, moving.rejected);
	CHECK(moving.rejected == 0);
	CHECK(moving.rms_mm < 2.0);
	CHECK(moving.max_mm < 5.0);
	CHECK(moving.max_deg < 1.0);

	// The same run with every camera pose frozen at the start (a static-camera assumption) cannot fit.
	SlidingWindowFusion frozen(test_params(scene));
	int rejected = 0;
	int64_t next_imu = 0;
	for (int64_t t = 16'683'000; t < 3'000'000'000; t += 16'683'000) {
		while (next_imu <= t + 5'000'000) {
			frozen.pushImu(scene.imu(next_imu));
			next_imu += 15'000'000;
		}
		FusionExposure e = scene.exposure(t, scene.model_pose(t * 1e-9), 0.2, rng, true, 0.0);
		FusionUpdateResult r = frozen.pushExposure(e);
		rejected += r.status == FusionUpdateStatus::RejectedPostSolve ||
		            r.status == FusionUpdateStatus::RejectedGate ||
		            r.status == FusionUpdateStatus::ResetAndInitialised;
	}
	CHECK(rejected > 20);
}

TEST_CASE("Fusion bridges a temporary optical dropout and reacquires")
{
	Scene scene;
	std::mt19937 rng(5);
	SlidingWindowFusion fusion(test_params(scene));
	auto hidden = [](double t) { return t >= 1.5 && t < 1.8; };
	RunErrors e = run_scene(scene, fusion, 3.0, hidden, unchanged, rng);
	CAPTURE(e.max_mm, e.max_deg, e.rms_mm, e.accepted, e.rejected);
	CHECK(e.max_mm < 30.0);
	CHECK(e.max_deg < 3.0);
	CHECK(e.rejected == 0);
	CHECK(fusion.stats().resets_gap == 0);
	// After the gap it tracks closely again.
	FusionPose after = fusion.getPose(2'900'000'000);
	CHECK(after.position_valid);
	CHECK((pos_of(after.Tcv_world_model) - pos_of(scene.model_pose(2.9))).norm() < 0.003);
}

TEST_CASE("Fusion rejects an implausible optical solve without being poisoned")
{
	Scene scene;
	std::mt19937 rng(9);
	SlidingWindowFusion fusion(test_params(scene));
	// A self-consistent but wrong lock: the ring as if turned 90 degrees and 10 cm away, rendered from that pose.
	int corrupted = 0;
	auto mutate = [&](double t, FusionExposure &e) {
		if (std::fabs(t - 1.5) < 0.009 || std::fabs(t - 2.2) < 0.009) {
			const xrt_pose truth = scene.model_pose(t);
			const xrt_pose wrong =
			    make_pose(Eigen::AngleAxisd(M_PI / 2, Eigen::Vector3d::UnitZ()) * quat_of(truth),
			              pos_of(truth) + Eigen::Vector3d(0.1, 0, 0));
			e = scene.exposure(e.timestamp_ns, wrong, 0.2, rng);
			corrupted++;
		}
	};
	RunErrors e = run_scene(scene, fusion, 3.0, never, mutate, rng);
	CAPTURE(e.max_mm, e.max_deg, e.accepted, e.rejected, corrupted);
	CHECK(corrupted == 2);
	CHECK(e.rejected == 2);
	CHECK(e.max_mm < 5.0);
	CHECK(e.max_deg < 1.0);
	CHECK(fusion.stats().resets_rejections == 0);
}

TEST_CASE("A change of LED synchronisation epoch resets the time-dependent state")
{
	Scene scene;
	std::mt19937 rng(11);
	SlidingWindowFusion fusion(test_params(scene));
	auto mutate = [](double t, FusionExposure &e) { e.sync_epoch = t < 1.0 ? 0 : 1; };
	RunErrors e = run_scene(scene, fusion, 1.5, never, mutate, rng);
	CHECK(fusion.stats().resets_epoch == 1);
	CHECK(fusion.stats().initialisations == 2);
	CHECK(e.max_mm < 5.0);
}

TEST_CASE("Fusion keeps its IMU buffer bounded")
{
	SlidingWindowFusionParams params;
	params.max_imu_samples = 1000;
	SlidingWindowFusion fusion(params);
	for (int64_t t = 0; t < 100'000; t++) {
		fusion.pushImu(imu_sample(t * 15'000'000, Eigen::Vector3d(0, -kG, 0), Eigen::Vector3d::Zero()));
	}
	CHECK(fusion.stats().peak_imu_buffer <= 1001);
	CHECK(fusion.stats().imu_samples == 100'000);
}

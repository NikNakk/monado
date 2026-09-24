// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "catch_amalgamated.hpp"
#include "joint_pose_solver.hpp"
#include "stereo_bootstrap.hpp"

#include <Eigen/Geometry>

#include <cmath>
#include <random>
#include <vector>

using namespace xrt::tracking::constellation;

namespace {

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
	return Eigen::Quaterniond(p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z);
}

Eigen::Vector3d
pos_of(const xrt_pose &p)
{
	return Eigen::Vector3d(p.position.x, p.position.y, p.position.z);
}

/*!
 * A 17-LED ring roughly the size of a Sense ring (radius 45 mm), LEDs facing outwards and forwards (-z device).
 * Spacing and heights are irregular, like the real ring: an evenly spaced ring is ambiguous under rotation by one
 * LED, and a solve can then settle one LED round with a plausible error.
 */
struct Ring
{
	std::vector<t_constellation_tracker_led> leds;
	t_constellation_tracker_led_model model{};

	//! @p mirror builds the other hand's ring: positions and normals reflected in x.
	explicit Ring(bool mirror = false)
	{
		for (int i = 0; i < 17; i++) {
			static const double jitter[17] = {0.0,  0.21, -0.13, 0.34, -0.27, 0.08, 0.41, -0.19, 0.15,
			                                  -0.36, 0.29, -0.05, 0.23, -0.31, 0.11, 0.38, -0.22};
			static const double height_mm[17] = {-6, 4, 9, -2, -8, 7, 1, -5, 10, -9, 3, 6, -4, 8, -7, 2, -1};
			double a = 2.0 * M_PI * (i + jitter[i]) / 17.0;
			double z = height_mm[i] / 1000.0;
			Eigen::Vector3d n = Eigen::Vector3d(std::cos(a), std::sin(a), -0.8).normalized();
			t_constellation_tracker_led led{};
			double sx = mirror ? -1.0 : 1.0;
			led.position = xrt_vec3{(float)(sx * 0.045 * std::cos(a)), (float)(0.045 * std::sin(a)), (float)z};
			led.normal = xrt_vec3{(float)(sx * n.x()), (float)n.y(), (float)n.z()};
			led.radius_m = 0.002f;
			led.visibility_angle = (float)(80.0 * M_PI / 180.0);
			led.id = (t_constellation_led_id_it)i;
			leds.push_back(led);
		}
		model.leds = leds.data();
		model.led_count = leds.size();
	}
};

//! Four fisheye cameras on a headset-like rig: two lower (81 mm apart) and two upper, all looking forward (+z).
struct Rig
{
	t_camera_model_params model{};
	std::vector<xrt_pose> cam_poses;

	Rig()
	{
		model.fx = model.fy = 188.0f;
		model.cx = model.cy = 254.0f;
		model.fisheye = t_camera_calibration_kb4_params_float{0.05f, -0.01f, 0.002f, -0.0005f};
		model.model = T_DISTORTION_FISHEYE_KB4;

		auto yaw_pitch = [](double yaw_deg, double pitch_deg) {
			return Eigen::Quaterniond(Eigen::AngleAxisd(yaw_deg * M_PI / 180.0, Eigen::Vector3d::UnitY()) *
			                          Eigen::AngleAxisd(pitch_deg * M_PI / 180.0, Eigen::Vector3d::UnitX()));
		};
		cam_poses.push_back(make_pose(yaw_pitch(-20, 20), Eigen::Vector3d(-0.0405, 0.03, 0)));
		cam_poses.push_back(make_pose(yaw_pitch(20, 20), Eigen::Vector3d(0.0405, 0.03, 0)));
		cam_poses.push_back(make_pose(yaw_pitch(-35, -15), Eigen::Vector3d(-0.075, -0.04, -0.02)));
		cam_poses.push_back(make_pose(yaw_pitch(35, -15), Eigen::Vector3d(0.075, -0.04, -0.02)));
	}
};

//! Project the ring at @p truth into every camera as blobs, with noise and distractors.
std::vector<std::vector<t_blob>>
render(const Rig &rig, const Ring &ring, const xrt_pose &truth, double noise_px, int distractors, std::mt19937 &rng)
{
	std::normal_distribution<double> noise(0.0, noise_px);
	std::uniform_real_distribution<double> anywhere(0.0, 500.0);
	std::vector<std::vector<t_blob>> out(rig.cam_poses.size());

	Eigen::Quaterniond qd = quat_of(truth);
	Eigen::Vector3d td = pos_of(truth);
	for (size_t c = 0; c < rig.cam_poses.size(); c++) {
		Eigen::Matrix3d R_cw = quat_of(rig.cam_poses[c]).toRotationMatrix().transpose();
		Eigen::Vector3d t_wc = pos_of(rig.cam_poses[c]);
		for (const auto &led : ring.leds) {
			Eigen::Vector3d p = R_cw * (qd * Eigen::Vector3d(led.position.x, led.position.y, led.position.z) + td - t_wc);
			Eigen::Vector3d n = R_cw * (qd * Eigen::Vector3d(led.normal.x, led.normal.y, led.normal.z));
			if (p.z() <= 0.02 || p.normalized().dot(n) > std::cos(M_PI - led.visibility_angle)) {
				continue;
			}
			float u, v;
			if (!t_camera_models_project(&rig.model, (float)p.x(), (float)p.y(), (float)p.z(), &u, &v) || u < 0 ||
			    v < 0 || u >= 508 || v >= 508) {
				continue;
			}
			t_blob b{};
			b.center = xrt_vec2{(float)(u + noise(rng)), (float)(v + noise(rng))};
			out[c].push_back(b);
		}
		for (int i = 0; i < distractors; i++) {
			t_blob b{};
			b.center = xrt_vec2{(float)anywhere(rng), (float)anywhere(rng)};
			out[c].push_back(b);
		}
	}
	return out;
}

std::vector<JointSolveCamera>
cameras_for(const Rig &rig, const std::vector<std::vector<t_blob>> &blobs)
{
	std::vector<JointSolveCamera> cams;
	for (size_t c = 0; c < rig.cam_poses.size(); c++) {
		cams.push_back(JointSolveCamera{rig.cam_poses[c], &rig.model, 508, 508, blobs[c].data(),
		                                (uint32_t)blobs[c].size(), nullptr});
	}
	return cams;
}

double
angle_deg(const Eigen::Quaterniond &a, const Eigen::Quaterniond &b)
{
	return a.angularDistance(b) * 180.0 / M_PI;
}

} // namespace

namespace {

struct Trial
{
	xrt_pose truth;
	xrt_pose prior;
	std::vector<std::vector<t_blob>> blobs;
};

//! Ring 30-50 cm in front of the rig, its LEDs (device -z) facing the cameras, random tilt; prior off by the given
//! position and angle.
Trial
make_trial(const Rig &rig, const Ring &ring, std::mt19937 &rng, double prior_m, double prior_deg)
{
	std::uniform_real_distribution<double> u(-1.0, 1.0);
	Eigen::Quaterniond q_truth(Eigen::AngleAxisd(0.4 * u(rng), Eigen::Vector3d::UnitX()) *
	                           Eigen::AngleAxisd(0.4 * u(rng), Eigen::Vector3d::UnitY()));
	Eigen::Vector3d t_truth(0.1 * u(rng), 0.05 * u(rng), 0.4 + 0.1 * u(rng));
	Eigen::Vector3d axis = Eigen::Vector3d(u(rng), u(rng), u(rng)).normalized();
	Eigen::Quaterniond q_prior = Eigen::AngleAxisd(prior_deg * M_PI / 180.0, axis) * q_truth;
	Eigen::Vector3d t_prior = t_truth + prior_m * Eigen::Vector3d(u(rng), u(rng), u(rng)).normalized();

	Trial trial;
	trial.truth = make_pose(q_truth, t_truth);
	trial.prior = make_pose(q_prior, t_prior);
	trial.blobs = render(rig, ring, trial.truth, 0.2, 3, rng);
	return trial;
}

} // namespace

TEST_CASE("Joint solve converges from a tracking-sized prior")
{
	// 6 mm and 2 degrees: 60 Hz tracking of a controller moving ~35 cm/s, with an IMU orientation prior.
	Ring ring;
	Rig rig;
	std::mt19937 rng(7);

	for (int i = 0; i < 50; i++) {
		CAPTURE(i);
		Trial trial = make_trial(rig, ring, rng, 0.006, 2.0);
		auto cams = cameras_for(rig, trial.blobs);

		JointSolveResult result;
		bool ok = joint_solve_refine(cams, ring.model, trial.prior, JointSolveParams{}, result);
		CAPTURE(result.matches, result.cameras_used, result.rms_px, result.coverage);
		REQUIRE(ok);
		CHECK(result.cameras_used >= 2);
		CHECK(result.rms_px < 0.5f);
		// A 45 mm ring at 40 cm constrains tilt weakly: 0.6 degrees moves the LEDs ~0.2 px.
		CHECK((pos_of(result.Tcv_world_device) - pos_of(trial.truth)).norm() < 0.002);
		CHECK(angle_deg(quat_of(result.Tcv_world_device), quat_of(trial.truth)) < 1.0);
	}
}

TEST_CASE("Joint solve never accepts a wrong pose from a re-acquisition-sized prior")
{
	/*
	 * 2 cm of position error shifts the image by about the ~8 px between neighbouring LEDs, so association can slip
	 * one LED round; that is the bootstrap's job. The solve may fail here, but must not accept a wrong pose. The
	 * orientation comes from an aligned IMU (2 degrees off, used as a 2-degree prior): without it a ring at arm's
	 * length can settle several degrees off in tilt at a sub-pixel RMS.
	 */
	Ring ring;
	Rig rig;
	std::mt19937 rng(19);

	int accepted = 0;
	for (int i = 0; i < 100; i++) {
		CAPTURE(i);
		Trial trial = make_trial(rig, ring, rng, 0.02, 2.0);
		auto cams = cameras_for(rig, trial.blobs);

		JointSolveParams params;
		params.orientation_prior_sigma_deg = 2.0f;
		// Provisional: synthetic noise is 0.2 px, so correct solves sit near 0.28 px while a stuck tilt reaches
		// ~0.95 px. The default must be set from real multi-camera residuals, which include rig calibration error.
		params.max_rms_px = 0.8f;
		JointSolveResult result;
		if (!joint_solve_refine(cams, ring.model, trial.prior, trial.prior.orientation, params, result)) {
			continue;
		}
		accepted++;
		CAPTURE(result.matches, result.rms_px, result.coverage, result.outliers);
		CHECK((pos_of(result.Tcv_world_device) - pos_of(trial.truth)).norm() < 0.003);
		// Weakly constrained tilt sits between the truth and the 2-degree prior.
		CHECK(angle_deg(quat_of(result.Tcv_world_device), quat_of(trial.truth)) < 2.0);
	}
	INFO("accepted " << accepted << " of 100");
	CHECK(accepted > 50);
}

TEST_CASE("Joint solve rejects a prior with nothing to match")
{
	Ring ring;
	Rig rig;
	std::mt19937 rng(11);
	std::vector<std::vector<t_blob>> blobs(4);
	std::uniform_real_distribution<double> anywhere(0.0, 500.0);
	for (auto &cam : blobs) {
		for (int i = 0; i < 6; i++) {
			t_blob b{};
			b.center = xrt_vec2{(float)anywhere(rng), (float)anywhere(rng)};
			cam.push_back(b);
		}
	}
	auto cams = cameras_for(rig, blobs);
	xrt_pose prior = make_pose(Eigen::Quaterniond::Identity(), Eigen::Vector3d(0, 0, 0.4));
	JointSolveResult result;
	CHECK_FALSE(joint_solve_refine(cams, ring.model, prior, JointSolveParams{}, result));
}

TEST_CASE("Joint solve skips blobs owned by another device")
{
	Ring ring;
	Rig rig;
	std::mt19937 rng(3);
	xrt_pose truth = make_pose(Eigen::Quaterniond::Identity(), Eigen::Vector3d(0.02, 0.0, 0.4));
	auto blobs = render(rig, ring, truth, 0.2, 0, rng);
	std::vector<std::vector<t_constellation_device_id_t>> owners(4);
	for (size_t c = 0; c < 4; c++) {
		owners[c].assign(blobs[c].size(), 1); // everything already claimed by device 1
	}
	auto cams = cameras_for(rig, blobs);
	for (size_t c = 0; c < 4; c++) {
		cams[c].blob_owner = owners[c].data();
	}
	JointSolveResult result;
	CHECK_FALSE(joint_solve_refine(cams, ring.model, truth, JointSolveParams{}, result));
	CHECK(result.matches == 0);
}

namespace {

//! Merge two rings' renders (e.g. both controllers in view).
std::vector<std::vector<t_blob>>
merge_blobs(std::vector<std::vector<t_blob>> a, const std::vector<std::vector<t_blob>> &b)
{
	for (size_t c = 0; c < a.size(); c++) {
		a[c].insert(a[c].end(), b[c].begin(), b[c].end());
	}
	return a;
}

xrt_pose
random_pose(std::mt19937 &rng, double x_offset)
{
	std::uniform_real_distribution<double> u(-1.0, 1.0);
	Eigen::Quaterniond q(Eigen::AngleAxisd(0.5 * u(rng), Eigen::Vector3d::UnitX()) *
	                     Eigen::AngleAxisd(0.5 * u(rng), Eigen::Vector3d::UnitY()) *
	                     Eigen::AngleAxisd(0.5 * u(rng), Eigen::Vector3d::UnitZ()));
	return make_pose(q, Eigen::Vector3d(x_offset + 0.05 * u(rng), 0.05 * u(rng), 0.4 + 0.1 * u(rng)));
}

} // namespace

TEST_CASE("Stereo bootstrap finds a ring with no prior")
{
	Ring ring;
	Rig rig;
	std::mt19937 rng(23);

	int found = 0;
	for (int i = 0; i < 30; i++) {
		CAPTURE(i);
		xrt_pose truth = random_pose(rng, 0.0);
		auto blobs = render(rig, ring, truth, 0.2, 3, rng);
		auto cams = cameras_for(rig, blobs);

		StereoBootstrapResult result;
		if (!stereo_bootstrap(cams, ring.model, StereoBootstrapParams{}, result)) {
			continue;
		}
		found++;
		CAPTURE(result.points, result.inliers, result.hypotheses, result.refined.rms_px);
		CHECK((pos_of(result.refined.Tcv_world_device) - pos_of(truth)).norm() < 0.003);
		CHECK(angle_deg(quat_of(result.refined.Tcv_world_device), quat_of(truth)) < 2.0);
	}
	INFO("found " << found << " of 30");
	CHECK(found >= 27);
}

TEST_CASE("Stereo bootstrap separates two mirror-image rings")
{
	Ring left(false);
	Ring right(true);
	Rig rig;
	std::mt19937 rng(29);

	for (int i = 0; i < 10; i++) {
		CAPTURE(i);
		xrt_pose left_truth = random_pose(rng, -0.12);
		xrt_pose right_truth = random_pose(rng, 0.12);
		auto blobs = merge_blobs(render(rig, left, left_truth, 0.2, 2, rng), render(rig, right, right_truth, 0.2, 0, rng));
		auto cams = cameras_for(rig, blobs);
		std::vector<std::vector<t_constellation_device_id_t>> owners(cams.size());
		for (size_t c = 0; c < cams.size(); c++) {
			owners[c].assign(cams[c].blob_count, XRT_CONSTELLATION_INVALID_DEVICE_ID);
			cams[c].blob_owner = owners[c].data();
		}

		StereoBootstrapResult left_result;
		REQUIRE(stereo_bootstrap(cams, left.model, StereoBootstrapParams{}, left_result));
		CHECK((pos_of(left_result.refined.Tcv_world_device) - pos_of(left_truth)).norm() < 0.003);
		for (const JointSolveMatch &m : left_result.refined.correspondences) {
			owners[m.camera][m.blob] = 0;
		}

		StereoBootstrapResult right_result;
		REQUIRE(stereo_bootstrap(cams, right.model, StereoBootstrapParams{}, right_result));
		CHECK((pos_of(right_result.refined.Tcv_world_device) - pos_of(right_truth)).norm() < 0.003);
	}
}

TEST_CASE("Stereo bootstrap does not fit a model to its mirror image")
{
	Ring left(false);
	Ring right(true);
	Rig rig;
	std::mt19937 rng(31);

	int wrong = 0;
	for (int i = 0; i < 30; i++) {
		xrt_pose truth = random_pose(rng, 0.0);
		auto cams_blobs = render(rig, right, truth, 0.2, 2, rng);
		auto cams = cameras_for(rig, cams_blobs);
		StereoBootstrapResult result;
		bool ok = stereo_bootstrap(cams, left.model, StereoBootstrapParams{}, result);
		if (ok) {
			UNSCOPED_INFO("mirror accepted: points " << result.points << " inliers " << result.inliers << " matches "
			              << result.refined.matches << " rms " << result.refined.rms_px << " coverage "
			              << result.refined.coverage << " outliers " << result.refined.outliers << " cameras "
			              << result.refined.cameras_used);
		}
		wrong += ok ? 1 : 0;
	}
	CHECK(wrong == 0);
}

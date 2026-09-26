// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "catch_amalgamated.hpp"
#include "t_imu_optical_filter.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cmath>
#include <deque>
#include <random>

namespace {

using Vec3 = Eigen::Vector3d;
using Quat = Eigen::Quaterniond;

const Vec3 kGravity(0, -9.80665, 0);

//! A smooth hand-like trajectory: position sinusoids (~10 cm, ~1 Hz) and a tumbling orientation (up to ~2 rad/s).
struct Truth
{
	Vec3
	p(double t) const
	{
		return Vec3(0.1 * std::sin(2.1 * t), 1.2 + 0.08 * std::sin(1.3 * t + 0.4), -0.4 + 0.1 * std::cos(1.7 * t));
	}
	Vec3
	v(double t) const
	{
		return Vec3(0.21 * std::cos(2.1 * t), 0.104 * std::cos(1.3 * t + 0.4), -0.17 * std::sin(1.7 * t));
	}
	Vec3
	a(double t) const
	{
		return Vec3(-0.441 * std::sin(2.1 * t), -0.1352 * std::sin(1.3 * t + 0.4), -0.289 * std::cos(1.7 * t));
	}
	//! Body angular velocity.
	Vec3
	omega(double t) const
	{
		return Vec3(1.2 * std::sin(0.9 * t), 0.8 * std::cos(1.4 * t), 1.5 * std::sin(0.6 * t + 1.0));
	}
};

struct Sim
{
	Truth truth;
	Vec3 gyro_bias{0.03, -0.05, 0.02};
	Vec3 accel_bias{0.05, -0.04, 0.08};
	double gyro_noise = 0.003, accel_noise = 0.03;
	double pos_noise = 0.0015, rot_noise = 0.004;
	int64_t imu_period_ns = 1'000'000;      // 1 kHz
	int64_t camera_period_ns = 16'683'000;  // 60 Hz
	int64_t optical_delay_ns = 40'000'000;  // arrives 40 ms after exposure
	std::mt19937 rng{7};

	Quat q{Quat::Identity()};
	int64_t t_ns = 0;
	std::deque<std::pair<int64_t, xrt_pose>> pending; // optical poses waiting for their delay

	double
	n(double s)
	{
		return std::normal_distribution<double>(0.0, s)(rng);
	}

	//! Truth orientation is integrated alongside the IMU so both agree exactly.
	void
	step_truth(double dt, double t)
	{
		Vec3 w = truth.omega(t);
		double angle = w.norm() * dt;
		if (angle > 0) {
			q = (q * Quat(Eigen::AngleAxisd(angle, w.normalized()))).normalized();
		}
	}

	/*!
	 * Run for @p seconds. Optical exposures are pushed after their delay unless @p optical_on returns false for the
	 * exposure time. @p outlier, if >= 0, is the exposure index whose pose is displaced 20 cm. Calls @p check with
	 * (time, filter) every IMU sample.
	 */
	template <typename On, typename Check>
	void
	run(t_imu_optical_filter *f, double seconds, On optical_on, Check check, int outlier = -1)
	{
		int64_t end = t_ns + (int64_t)(seconds * 1e9);
		int64_t next_camera = ((t_ns / camera_period_ns) + 1) * camera_period_ns;
		int exposure = 0;
		while (t_ns < end) {
			t_ns += imu_period_ns;
			double t = t_ns * 1e-9;
			step_truth(imu_period_ns * 1e-9, t);
			Vec3 gyro = truth.omega(t) + gyro_bias + Vec3(n(gyro_noise), n(gyro_noise), n(gyro_noise));
			Vec3 accel = q.conjugate() * (truth.a(t) - kGravity) + accel_bias +
			             Vec3(n(accel_noise), n(accel_noise), n(accel_noise));
			xrt_vec3 a{(float)accel.x(), (float)accel.y(), (float)accel.z()};
			xrt_vec3 g{(float)gyro.x(), (float)gyro.y(), (float)gyro.z()};
			t_imu_optical_filter_push_imu(f, t_ns, &a, &g);

			if (t_ns >= next_camera) {
				if (optical_on(next_camera * 1e-9)) {
					Vec3 p = truth.p(t) + Vec3(n(pos_noise), n(pos_noise), n(pos_noise));
					if (exposure == outlier) {
						p += Vec3(0.2, 0, 0);
					}
					Quat qm = (q * Quat(Eigen::AngleAxisd(std::abs(n(rot_noise)),
					                                     Vec3(n(1), n(1), n(1)).normalized())))
					              .normalized();
					xrt_pose pose{{(float)qm.x(), (float)qm.y(), (float)qm.z(), (float)qm.w()},
					              {(float)p.x(), (float)p.y(), (float)p.z()}};
					pending.emplace_back(next_camera, pose);
				}
				exposure++;
				next_camera += camera_period_ns;
			}
			while (!pending.empty() && pending.front().first + optical_delay_ns <= t_ns) {
				t_imu_optical_filter_push_pose(f, pending.front().first, &pending.front().second, 0.002f, 0.006f);
				pending.pop_front();
			}
			check(t, f);
		}
	}

	//! Position (m) and orientation (rad) error of the filter at the current time.
	std::pair<double, double>
	error(t_imu_optical_filter *f)
	{
		xrt_space_relation rel;
		REQUIRE(t_imu_optical_filter_get_relation(f, t_ns, &rel));
		Vec3 p(rel.pose.position.x, rel.pose.position.y, rel.pose.position.z);
		Quat qf(rel.pose.orientation.w, rel.pose.orientation.x, rel.pose.orientation.y, rel.pose.orientation.z);
		return {(p - truth.p(t_ns * 1e-9)).norm(), Eigen::AngleAxisd(qf.conjugate() * q).angle()};
	}
};

t_imu_optical_filter *
make_filter()
{
	t_imu_optical_filter_params params;
	t_imu_optical_filter_default_params(&params);
	// Match the simulated 1 kHz IMU; the defaults are tuned for the real 66 Hz Sense IMU.
	params.gyro_noise_rad_s = 0.005f;
	params.accel_noise_m_s2 = 0.08f;
	return t_imu_optical_filter_create(&params);
}

auto always = [](double) { return true; };
auto nothing = [](double, t_imu_optical_filter *) {};

} // namespace

TEST_CASE("IMU optical filter converges and learns the biases")
{
	t_imu_optical_filter *f = make_filter();
	Sim sim;
	sim.run(f, 6.0, always, nothing);

	double pos2 = 0, rot2 = 0;
	int n = 0;
	sim.run(f, 2.0, always, [&](double, t_imu_optical_filter *filter) {
		auto [pe, re] = sim.error(filter);
		pos2 += pe * pe;
		rot2 += re * re;
		n++;
	});
	double pos_rms_mm = std::sqrt(pos2 / n) * 1000, rot_rms_deg = std::sqrt(rot2 / n) * 180 / M_PI;
	CAPTURE(pos_rms_mm, rot_rms_deg);
	// The optical poses have 1.5 mm noise per axis (2.6 mm RMS overall) and arrive 40 ms late; the filter is
	// asked for "now", so it must beat the raw noise while also carrying each pose 40 ms forward.
	CHECK(pos_rms_mm < 2.3);
	CHECK(rot_rms_deg < 0.5);

	t_imu_optical_filter_stats stats;
	t_imu_optical_filter_get_stats(f, &stats);
	Vec3 bg(stats.gyro_bias_rad_s.x, stats.gyro_bias_rad_s.y, stats.gyro_bias_rad_s.z);
	CAPTURE(bg.transpose());
	CHECK((bg - sim.gyro_bias).norm() < 0.01);
	CHECK(stats.rejections == 0);
	t_imu_optical_filter_destroy(&f);
}

TEST_CASE("IMU optical filter bridges a 300 ms optical dropout")
{
	t_imu_optical_filter *f = make_filter();
	Sim sim;
	sim.run(f, 6.0, always, nothing);
	double start = sim.t_ns * 1e-9;
	double worst_pos = 0, worst_rot = 0;
	sim.run(
	    f, 0.3, [&](double t) { return t < start; },
	    [&](double, t_imu_optical_filter *filter) {
		    auto [pe, re] = sim.error(filter);
		    worst_pos = std::max(worst_pos, pe);
		    worst_rot = std::max(worst_rot, re);
	    });
	CAPTURE(worst_pos * 1000, worst_rot * 180 / M_PI);
	CHECK(worst_pos < 0.02);
	CHECK(worst_rot * 180 / M_PI < 1.0);

	// Optical returns: back within a few mm quickly.
	sim.run(f, 0.3, always, nothing);
	CHECK(sim.error(f).first < 0.004);
	t_imu_optical_filter_destroy(&f);
}

TEST_CASE("IMU optical filter rejects an outlier and re-initialises after a long gap")
{
	t_imu_optical_filter *f = make_filter();
	Sim sim;
	sim.run(f, 4.0, always, nothing);

	sim.run(f, 0.5, always, nothing, 10);
	t_imu_optical_filter_stats stats;
	t_imu_optical_filter_get_stats(f, &stats);
	CHECK(stats.rejections == 1);
	CHECK(sim.error(f).first < 0.004);

	// Two seconds hidden: IMU-only drift is large, the next optical pose re-initialises.
	double start = sim.t_ns * 1e-9;
	sim.run(f, 2.0, [&](double t) { return t < start; }, nothing);
	sim.run(f, 0.3, always, nothing);
	t_imu_optical_filter_get_stats(f, &stats);
	CHECK(stats.reinitialisations == 1);
	CHECK(sim.error(f).first < 0.005);
	t_imu_optical_filter_destroy(&f);
}

TEST_CASE("IMU optical filter predicts ahead from the latest IMU state")
{
	t_imu_optical_filter *f = make_filter();
	Sim sim;
	sim.run(f, 6.0, always, nothing);
	xrt_space_relation rel;
	int64_t ahead = sim.t_ns + 20'000'000;
	REQUIRE(t_imu_optical_filter_get_relation(f, ahead, &rel));
	Vec3 p(rel.pose.position.x, rel.pose.position.y, rel.pose.position.z);
	// Constant-velocity prediction over 20 ms: within a few mm of the truth for this trajectory.
	CHECK((p - sim.truth.p(ahead * 1e-9)).norm() < 0.005);
	CHECK((rel.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) != 0);
	t_imu_optical_filter_destroy(&f);
}

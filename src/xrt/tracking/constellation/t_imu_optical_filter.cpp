// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Error-state EKF fusing a device's IMU with delayed optical poses.
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "t_imu_optical_filter.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cmath>
#include <deque>
#include <memory>

namespace {

using Vec3 = Eigen::Vector3d;
using Mat3 = Eigen::Matrix3d;
using Quat = Eigen::Quaterniond;
using Mat15 = Eigen::Matrix<double, 15, 15>;

// Error-state layout.
constexpr int P_ = 0, V_ = 3, TH = 6, BG = 9, BA = 12;

Mat3
skew(const Vec3 &v)
{
	Mat3 m;
	m << 0, -v.z(), v.y(), v.z(), 0, -v.x(), -v.y(), v.x(), 0;
	return m;
}

Quat
exp_quat(const Vec3 &theta)
{
	double angle = theta.norm();
	if (angle < 1e-12) {
		return Quat(1, 0.5 * theta.x(), 0.5 * theta.y(), 0.5 * theta.z()).normalized();
	}
	return Quat(Eigen::AngleAxisd(angle, theta / angle));
}

Vec3
log_quat(const Quat &q_in)
{
	Quat q = q_in.w() < 0 ? Quat(-q_in.w(), -q_in.x(), -q_in.y(), -q_in.z()) : q_in;
	Eigen::AngleAxisd aa(q);
	return aa.angle() * aa.axis();
}

struct State
{
	int64_t t{0};
	Vec3 p{Vec3::Zero()}, v{Vec3::Zero()};
	Quat q{Quat::Identity()}; // world <- body
	Vec3 bg{Vec3::Zero()}, ba{Vec3::Zero()};
	Mat15 P{Mat15::Identity()};
	//! The (bias-corrected) IMU sample that brought the state to t, kept for prediction.
	Vec3 omega{Vec3::Zero()}, accel_world{Vec3::Zero()};
};

struct ImuSample
{
	int64_t t;
	Vec3 accel, gyro;
};

} // namespace

struct t_imu_optical_filter
{
	t_imu_optical_filter_params params;
	bool initialised{false};
	State state;
	//! States after each IMU sample (and each update), oldest first; the last is the current state.
	std::deque<State> history;
	std::deque<ImuSample> imu;
	int64_t last_accepted_ns{0};
	uint32_t consecutive_rejections{0};
	t_imu_optical_filter_stats stats{};

	Vec3
	gravity() const
	{
		return Vec3(params.gravity_m_s2.x, params.gravity_m_s2.y, params.gravity_m_s2.z);
	}

	//! Propagate @p s by one IMU sample to @p t.
	void
	propagate(State &s, const ImuSample &m, int64_t t) const
	{
		double dt = (double)(t - s.t) * 1e-9;
		if (dt <= 0.0) {
			return;
		}
		if (dt > 0.1) {
			dt = 0.1; // a gap: do not integrate a stale sample for long
		}
		Vec3 omega = m.gyro - s.bg;
		Vec3 a_body = m.accel - s.ba;
		Mat3 R = s.q.toRotationMatrix();
		Vec3 a_world = R * a_body + gravity();

		s.p += s.v * dt + 0.5 * a_world * dt * dt;
		s.v += a_world * dt;
		s.q = (s.q * exp_quat(omega * dt)).normalized();

		Mat15 F = Mat15::Identity();
		F.block<3, 3>(P_, V_) = Mat3::Identity() * dt;
		F.block<3, 3>(V_, TH) = -R * skew(a_body) * dt;
		F.block<3, 3>(V_, BA) = -R * dt;
		F.block<3, 3>(TH, TH) = exp_quat(-omega * dt).toRotationMatrix();
		F.block<3, 3>(TH, BG) = -Mat3::Identity() * dt;

		Mat15 Q = Mat15::Zero();
		double sg = params.gyro_noise_rad_s, sa = params.accel_noise_m_s2;
		double wg = params.gyro_bias_walk_rad_s2, wa = params.accel_bias_walk_m_s3;
		Q.block<3, 3>(V_, V_) = Mat3::Identity() * sa * sa * dt;
		Q.block<3, 3>(TH, TH) = Mat3::Identity() * sg * sg * dt;
		Q.block<3, 3>(BG, BG) = Mat3::Identity() * wg * wg * dt;
		Q.block<3, 3>(BA, BA) = Mat3::Identity() * wa * wa * dt;

		s.P = F * s.P * F.transpose() + Q;
		s.P = 0.5 * (s.P + s.P.transpose());
		s.t = t;
		s.omega = omega;
		s.accel_world = a_world;
	}

	void
	trim(int64_t now)
	{
		while (history.size() > 2 && history[1].t < now - params.history_ns) {
			history.pop_front();
		}
		while (!imu.empty() && imu.front().t < history.front().t) {
			imu.pop_front();
		}
	}

	void
	initialise(int64_t t, const Vec3 &p, const Quat &q, double sp, double sr)
	{
		State s;
		s.t = t;
		s.p = p;
		s.q = q;
		if (initialised) {
			// Keep the learnt biases across re-initialisations.
			s.bg = state.bg;
			s.ba = state.ba;
		}
		s.P = Mat15::Zero();
		s.P.block<3, 3>(P_, P_) = Mat3::Identity() * sp * sp;
		double sv = params.initial_velocity_sigma_m_s;
		s.P.block<3, 3>(V_, V_) = Mat3::Identity() * sv * sv;
		s.P.block<3, 3>(TH, TH) = Mat3::Identity() * sr * sr;
		double sbg = params.initial_gyro_bias_sigma_rad_s, sba = params.initial_accel_bias_sigma_m_s2;
		s.P.block<3, 3>(BG, BG) = Mat3::Identity() * sbg * sbg;
		s.P.block<3, 3>(BA, BA) = Mat3::Identity() * sba * sba;
		state = s;
		history.clear();
		history.push_back(state);
		// Re-propagate any IMU samples newer than t.
		for (const ImuSample &m : imu) {
			if (m.t > t) {
				propagate(state, m, m.t);
				history.push_back(state);
			}
		}
		initialised = true;
	}
};

extern "C" void
t_imu_optical_filter_default_params(t_imu_optical_filter_params *params)
{
	*params = t_imu_optical_filter_params{};
	params->gravity_m_s2 = {0.0f, -9.80665f, 0.0f};
	params->gyro_noise_rad_s = 0.005f;
	params->accel_noise_m_s2 = 0.08f;
	params->gyro_bias_walk_rad_s2 = 0.0005f;
	params->accel_bias_walk_m_s3 = 0.005f;
	params->initial_velocity_sigma_m_s = 0.5f;
	params->initial_gyro_bias_sigma_rad_s = 0.05f;
	params->initial_accel_bias_sigma_m_s2 = 0.3f;
	params->gate_chi2 = 30.0f;
	params->reinit_gap_ns = 500'000'000;
	params->reinit_after_rejections = 5;
	params->history_ns = 250'000'000;
	params->max_prediction_ns = 100'000'000;
	params->position_tracked_ns = 250'000'000;
}

extern "C" t_imu_optical_filter *
t_imu_optical_filter_create(const t_imu_optical_filter_params *params)
{
	auto *filter = new t_imu_optical_filter();
	filter->params = *params;
	return filter;
}

extern "C" void
t_imu_optical_filter_destroy(t_imu_optical_filter **filter_ptr)
{
	if (filter_ptr == nullptr || *filter_ptr == nullptr) {
		return;
	}
	delete *filter_ptr;
	*filter_ptr = nullptr;
}

extern "C" void
t_imu_optical_filter_push_imu(t_imu_optical_filter *f,
                              int64_t timestamp_ns,
                              const xrt_vec3 *accel_m_s2,
                              const xrt_vec3 *gyro_rad_s)
{
	ImuSample m{timestamp_ns, Vec3(accel_m_s2->x, accel_m_s2->y, accel_m_s2->z),
	            Vec3(gyro_rad_s->x, gyro_rad_s->y, gyro_rad_s->z)};
	if (!f->imu.empty() && timestamp_ns <= f->imu.back().t) {
		return;
	}
	f->imu.push_back(m);
	f->stats.imu_samples++;
	if (!f->initialised) {
		// Keep a history's worth so the first optical pose can be re-propagated to now.
		while (!f->imu.empty() && f->imu.front().t < timestamp_ns - f->params.history_ns) {
			f->imu.pop_front();
		}
		return;
	}
	f->propagate(f->state, m, timestamp_ns);
	f->history.push_back(f->state);
	f->trim(timestamp_ns);
}

extern "C" t_imu_optical_filter_update_result
t_imu_optical_filter_push_pose(t_imu_optical_filter *f,
                               int64_t timestamp_ns,
                               const xrt_pose *pose,
                               float position_sigma_m,
                               float orientation_sigma_rad)
{
	Vec3 pm(pose->position.x, pose->position.y, pose->position.z);
	Quat qm(pose->orientation.w, pose->orientation.x, pose->orientation.y, pose->orientation.z);
	qm.normalize();
	double sp = position_sigma_m, sr = orientation_sigma_rad;

	if (!f->initialised || f->imu.empty()) {
		if (f->imu.empty()) {
			return T_IMU_OPTICAL_FILTER_IGNORED;
		}
		f->initialise(timestamp_ns, pm, qm, sp, sr);
		f->last_accepted_ns = timestamp_ns;
		return T_IMU_OPTICAL_FILTER_INITIALISED;
	}

	const bool stale = timestamp_ns - f->last_accepted_ns > f->params.reinit_gap_ns;
	if (stale || f->consecutive_rejections >= f->params.reinit_after_rejections) {
		f->initialise(timestamp_ns, pm, qm, sp, sr);
		f->last_accepted_ns = timestamp_ns;
		f->consecutive_rejections = 0;
		f->stats.reinitialisations++;
		return T_IMU_OPTICAL_FILTER_REINITIALISED;
	}

	// Rewind to the last state at or before the exposure.
	if (timestamp_ns < f->history.front().t) {
		return T_IMU_OPTICAL_FILTER_IGNORED;
	}
	size_t k = f->history.size() - 1;
	while (k > 0 && f->history[k].t > timestamp_ns) {
		k--;
	}
	State s = f->history[k];
	// Propagate to the exposure with the next IMU sample's reading.
	for (const ImuSample &m : f->imu) {
		if (m.t > s.t) {
			f->propagate(s, m, std::min(m.t, timestamp_ns));
			break;
		}
	}

	// Residual in the error-state convention: p = p_hat + dp, q = q_hat * exp(dtheta).
	Eigen::Matrix<double, 6, 1> r;
	r.head<3>() = pm - s.p;
	r.tail<3>() = log_quat(s.q.conjugate() * qm);
	Eigen::Matrix<double, 6, 15> H = Eigen::Matrix<double, 6, 15>::Zero();
	H.block<3, 3>(0, P_) = Mat3::Identity();
	H.block<3, 3>(3, TH) = Mat3::Identity();
	Eigen::Matrix<double, 6, 6> R = Eigen::Matrix<double, 6, 6>::Zero();
	R.block<3, 3>(0, 0) = Mat3::Identity() * sp * sp;
	R.block<3, 3>(3, 3) = Mat3::Identity() * sr * sr;
	Eigen::Matrix<double, 6, 6> S = H * s.P * H.transpose() + R;
	Eigen::Matrix<double, 6, 6> S_inv = S.inverse();
	double m2 = r.dot(S_inv * r);
	f->stats.last_mahalanobis2 = (float)m2;
	if (m2 > f->params.gate_chi2) {
		f->consecutive_rejections++;
		f->stats.rejections++;
		return T_IMU_OPTICAL_FILTER_REJECTED;
	}

	Eigen::Matrix<double, 15, 6> K = s.P * H.transpose() * S_inv;
	Eigen::Matrix<double, 15, 1> dx = K * r;
	s.p += dx.segment<3>(P_);
	s.v += dx.segment<3>(V_);
	s.q = (s.q * exp_quat(dx.segment<3>(TH))).normalized();
	s.bg += dx.segment<3>(BG);
	s.ba += dx.segment<3>(BA);
	Mat15 I_KH = Mat15::Identity() - K * H;
	s.P = I_KH * s.P * I_KH.transpose() + K * R * K.transpose(); // Joseph form
	s.P = 0.5 * (s.P + s.P.transpose());

	// Replace everything after the exposure and re-propagate the buffered IMU samples.
	f->history.erase(f->history.begin() + (long)k + 1, f->history.end());
	f->history.push_back(s);
	for (const ImuSample &m : f->imu) {
		if (m.t > s.t) {
			f->propagate(s, m, m.t);
			f->history.push_back(s);
		}
	}
	f->state = s;
	f->last_accepted_ns = std::max(f->last_accepted_ns, timestamp_ns);
	f->consecutive_rejections = 0;
	f->stats.updates++;
	return T_IMU_OPTICAL_FILTER_UPDATED;
}

extern "C" bool
t_imu_optical_filter_get_relation(t_imu_optical_filter *f, int64_t timestamp_ns, xrt_space_relation *out)
{
	if (!f->initialised) {
		return false;
	}
	State s = f->state;
	if (timestamp_ns < s.t) {
		// From the history: the last state at or before the time.
		for (auto it = f->history.rbegin(); it != f->history.rend(); ++it) {
			if (it->t <= timestamp_ns) {
				s = *it;
				break;
			}
		}
	}
	double dt = (double)(timestamp_ns - s.t) * 1e-9;
	dt = std::max(0.0, std::min(dt, (double)f->params.max_prediction_ns * 1e-9));
	Vec3 p = s.p + s.v * dt;
	Vec3 v = s.v;
	Quat q = (s.q * exp_quat(s.omega * dt)).normalized();
	Vec3 omega_world = q * s.omega;

	*out = XRT_SPACE_RELATION_ZERO;
	out->pose.position = {(float)p.x(), (float)p.y(), (float)p.z()};
	out->pose.orientation = {(float)q.x(), (float)q.y(), (float)q.z(), (float)q.w()};
	out->linear_velocity = {(float)v.x(), (float)v.y(), (float)v.z()};
	out->angular_velocity = {(float)omega_world.x(), (float)omega_world.y(), (float)omega_world.z()};
	int flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	            XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	            XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT;
	if (timestamp_ns - f->last_accepted_ns <= f->params.position_tracked_ns) {
		flags |= XRT_SPACE_RELATION_POSITION_TRACKED_BIT;
	}
	out->relation_flags = (xrt_space_relation_flags)flags;
	return true;
}

extern "C" void
t_imu_optical_filter_get_stats(const t_imu_optical_filter *f, t_imu_optical_filter_stats *out)
{
	*out = f->stats;
	out->gyro_bias_rad_s = {(float)f->state.bg.x(), (float)f->state.bg.y(), (float)f->state.bg.z()};
	out->accel_bias_m_s2 = {(float)f->state.ba.x(), (float)f->state.ba.y(), (float)f->state.ba.z()};
}

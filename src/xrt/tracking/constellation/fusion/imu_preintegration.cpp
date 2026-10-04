// Copyright 2026, Beyley Cardellio
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  IMU preintegration for the experimental sliding-window fusion.
 *
 * Adapted from Monado MR 3015 (head cd7174257), `optimizer/internal_math.cpp` (preintegrate() and its helpers), by
 * Beyley Cardellio. The only change is that the noise densities come from an @ref ImuNoiseModel.
 *
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "imu_preintegration.hpp"

#include <Eigen/Cholesky>

namespace xrt::tracking::constellation::fusion {

namespace {

	Matrix3d
	skew(const Vector3d &v)
	{
		Matrix3d m;
		m << 0.0, -v.z(), v.y(), v.z(), 0.0, -v.x(), -v.y(), v.x(), 0.0;
		return m;
	}

	//! Right Jacobian of SO(3): Exp(phi + delta) ~= Exp(phi) Exp(Jr(phi) delta).
	Matrix3d
	rightJacobianSo3(const Vector3d &phi)
	{
		const double theta_squared = phi.squaredNorm();
		const Matrix3d phi_skew = skew(phi);

		double a, b;
		if (theta_squared < 1e-8) {
			a = 0.5 - (theta_squared / 24.0);
			b = (1.0 / 6.0) - (theta_squared / 120.0);
		} else {
			const double theta = std::sqrt(theta_squared);
			a = (1.0 - std::cos(theta)) / theta_squared;
			b = (theta - std::sin(theta)) / (theta_squared * theta);
		}
		return Matrix3d::Identity() - (a * phi_skew) + (b * phi_skew * phi_skew);
	}

	template <typename Derived>
	Matrix<double, Derived::RowsAtCompileTime * Derived::ColsAtCompileTime, ImuBias<double>::kNumParameters>
	extractJacobianFromJetMatrix(const Eigen::MatrixBase<Derived> &m)
	{
		constexpr int kRows = Derived::RowsAtCompileTime;
		constexpr int kCols = Derived::ColsAtCompileTime;

		Matrix<double, kRows * kCols, ImuBias<double>::kNumParameters> J;
		for (int r = 0; r < kRows; ++r) {
			for (int c = 0; c < kCols; ++c) {
				for (int j = 0; j < ImuBias<double>::kNumParameters; ++j) {
					J(r * kCols + c, j) = m(r, c).v[j];
				}
			}
		}
		return J;
	}

	template <int N>
	Matrix3d
	quaternionJetToSO3Jacobian(const Quaternion<ceres::Jet<double, N>> &q_jet, int parameter_offset)
	{
		Quaterniond q(q_jet.w().a, q_jet.x().a, q_jet.y().a, q_jet.z().a);
		q.normalize();
		const Quaterniond q_inv = q.conjugate();

		Matrix3d J;
		for (int j = 0; j < 3; j++) {
			Quaterniond dq(q_jet.w().v[parameter_offset + j], q_jet.x().v[parameter_offset + j],
			               q_jet.y().v[parameter_offset + j], q_jet.z().v[parameter_offset + j]);
			Quaterniond local = q_inv * dq;
			J.col(j) = 2.0 * Vector3d(local.x(), local.y(), local.z());
		}
		return J;
	}

	void
	propagateCovariance(const Quaterniond &Q_start_cur,
	                    const Vector3d &corrected_accel,
	                    const Vector3d &corrected_gyro,
	                    const Vector3d &accel_scale,
	                    double dt,
	                    const ImuNoiseModel &noise_model,
	                    PreintegrationCovarianceMatrix &covariance)
	{
		if (dt <= 0.0) {
			return;
		}

		const Matrix3d dR = Q_start_cur.toRotationMatrix();
		const Matrix3d accel_skew = skew(corrected_accel);
		const Matrix3d scale = accel_scale.asDiagonal();

		PreintegrationCovarianceMatrix A = PreintegrationCovarianceMatrix::Identity();
		A.block<3, 3>(kImuBiasRotCovIndex, kImuBiasRotCovIndex) =
		    quat_exp_so3(Vector3d(-corrected_gyro * dt)).toRotationMatrix();
		A.block<3, 3>(kImuBiasVelCovIndex, kImuBiasRotCovIndex) = -dR * accel_skew * dt;
		A.block<3, 3>(kImuBiasPosCovIndex, kImuBiasRotCovIndex) = -0.5 * dR * accel_skew * dt * dt;
		A.block<3, 3>(kImuBiasPosCovIndex, kImuBiasVelCovIndex) = Matrix3d::Identity() * dt;

		Matrix<double, kNumImuBiasCovIndices, 6> B = Matrix<double, kNumImuBiasCovIndices, 6>::Zero();
		B.block<3, 3>(kImuBiasRotCovIndex, 0) = rightJacobianSo3(corrected_gyro * dt) * dt;
		B.block<3, 3>(kImuBiasVelCovIndex, 3) = dR * scale * dt;
		B.block<3, 3>(kImuBiasPosCovIndex, 3) = 0.5 * dR * scale * dt * dt;

		const double g2 = noise_model.gyro_noise_density * noise_model.gyro_noise_density;
		const double a2 = noise_model.accel_noise_density * noise_model.accel_noise_density;
		Matrix<double, 6, 6> noise = Matrix<double, 6, 6>::Zero();
		noise.block<3, 3>(0, 0) = Matrix3d::Identity() * (g2 / dt);
		noise.block<3, 3>(3, 3) = Matrix3d::Identity() * (a2 / dt);

		covariance = A * covariance * A.transpose() + B * noise * B.transpose();
	}

	void
	integrateSingleSample(const xrt_imu_sample &sample,
	                      double dt,
	                      const Vector3<ImuBiasJet> &accel_bias,
	                      const Vector3<ImuBiasJet> &gyro_bias,
	                      const Vector3<ImuBiasJet> &accel_scale,
	                      const ImuNoiseModel &noise_model,
	                      Quaternion<ImuBiasJet> &Q_start_cur,
	                      Vector3<ImuBiasJet> &delta_velocity,
	                      Vector3<ImuBiasJet> &delta_position,
	                      PreintegrationCovarianceMatrix &covariance)
	{
		Vector3<ImuBiasJet> cur_accel = map_vec3_f64(sample.accel_m_s2).cast<ImuBiasJet>();
		Vector3<ImuBiasJet> cur_gyro = map_vec3_f64(sample.gyro_rad_secs).cast<ImuBiasJet>();

		cur_accel = (cur_accel - accel_bias).cwiseProduct(accel_scale);
		cur_gyro = cur_gyro - gyro_bias;

		{
			const Vector3d real_accel = {cur_accel.x().a, cur_accel.y().a, cur_accel.z().a};
			const Vector3d real_gyro = {cur_gyro.x().a, cur_gyro.y().a, cur_gyro.z().a};
			const Vector3d real_scale = {accel_scale.x().a, accel_scale.y().a, accel_scale.z().a};
			const Quaterniond real_Q_start_cur = {Q_start_cur.w().a, Q_start_cur.x().a, Q_start_cur.y().a,
			                                      Q_start_cur.z().a};
			propagateCovariance(real_Q_start_cur, real_accel, real_gyro, real_scale, dt, noise_model,
			                    covariance);
		}

		const Vector3<ImuBiasJet> start_accel = Q_start_cur * cur_accel;
		delta_position += delta_velocity * dt + ImuBiasJet(0.5) * start_accel * dt * dt;
		delta_velocity += start_accel * dt;
		Q_start_cur *= quat_exp_so3(Vector3<ImuBiasJet>(cur_gyro * ImuBiasJet(dt)));
	}

} // namespace

PreintegratedImuSamples
preintegrate(std::span<const xrt_imu_sample> imu_samples,
             const xrt_imu_sample &first_sample_after_end_time,
             const ImuBias<ImuBiasJet> &imu_bias,
             int64_t start_time_ns,
             int64_t end_time_ns,
             const ImuNoiseModel &noise)
{
	Quaternion<ImuBiasJet> Q_start_cur = Quaternion<ImuBiasJet>::Identity();
	Vector3<ImuBiasJet> delta_velocity = Vector3<ImuBiasJet>::Zero();
	Vector3<ImuBiasJet> delta_position = Vector3<ImuBiasJet>::Zero();
	PreintegrationCovarianceMatrix covariance = PreintegrationCovarianceMatrix::Zero();
	uint32_t steps = 0;

	const auto &accel_bias = imu_bias.accel_bias;
	const auto &gyro_bias = imu_bias.gyro_bias;
	const auto &accel_scale = imu_bias.accel_scale;

	auto step = [&](const xrt_imu_sample &sample, double dt) {
		integrateSingleSample(sample, dt, accel_bias, gyro_bias, accel_scale, noise, Q_start_cur,
		                      delta_velocity, delta_position, covariance);
		steps++;
	};

	if (imu_samples.empty()) {
		// No sample inside the interval: the one after it covers the whole of it (a keyframe gap shorter than
		// the IMU period, which the ~66 Hz Sense IMU makes common).
		step(first_sample_after_end_time, static_cast<double>(end_time_ns - start_time_ns) * 1e-9);
	} else {
		const xrt_imu_sample &first_sample = imu_samples.front();
		const xrt_imu_sample &last_sample = imu_samples.back();
		assert(first_sample.timestamp_ns >= start_time_ns);
		assert(last_sample.timestamp_ns <= end_time_ns);

		if (first_sample.timestamp_ns > start_time_ns) {
			step(first_sample, static_cast<double>(first_sample.timestamp_ns - start_time_ns) * 1e-9);
		}
		for (size_t i = 0; i + 1 < imu_samples.size(); i++) {
			step(imu_samples[i + 1],
			     static_cast<double>(imu_samples[i + 1].timestamp_ns - imu_samples[i].timestamp_ns) * 1e-9);
		}
		if (last_sample.timestamp_ns < end_time_ns) {
			step(first_sample_after_end_time,
			     static_cast<double>(end_time_ns - last_sample.timestamp_ns) * 1e-9);
		}
	}

	const auto J_v = extractJacobianFromJetMatrix(delta_velocity);
	const auto J_p = extractJacobianFromJetMatrix(delta_position);

	PreintegratedImuSamples out = PreintegratedImuSamples::Identity();
	out.Q_start_end = Quaterniond(Q_start_cur.w().a, Q_start_cur.x().a, Q_start_cur.y().a, Q_start_cur.z().a);
	out.delta_velocity = {delta_velocity[0].a, delta_velocity[1].a, delta_velocity[2].a};
	out.delta_position = {delta_position[0].a, delta_position[1].a, delta_position[2].a};
	out.dt = static_cast<double>(end_time_ns - start_time_ns) * 1e-9;
	out.accel_bias_at_integration = {accel_bias[0].a, accel_bias[1].a, accel_bias[2].a};
	out.gyro_bias_at_integration = {gyro_bias[0].a, gyro_bias[1].a, gyro_bias[2].a};
	out.accel_scale_at_integration = {accel_scale[0].a, accel_scale[1].a, accel_scale[2].a};
	out.J_R_bg = quaternionJetToSO3Jacobian(Q_start_cur, ImuBias<double>::kGyroBiasIndex);
	out.J_v_ba = J_v.block<3, 3>(0, ImuBias<double>::kAccelBiasIndex);
	out.J_v_sa = J_v.block<3, 3>(0, ImuBias<double>::kAccelScaleIndex);
	out.J_v_bg = J_v.block<3, 3>(0, ImuBias<double>::kGyroBiasIndex);
	out.J_p_ba = J_p.block<3, 3>(0, ImuBias<double>::kAccelBiasIndex);
	out.J_p_sa = J_p.block<3, 3>(0, ImuBias<double>::kAccelScaleIndex);
	out.J_p_bg = J_p.block<3, 3>(0, ImuBias<double>::kGyroBiasIndex);
	out.num_steps = steps;

	// Sigma = L L^T, so the whitener is L^-1.
	Eigen::LLT<PreintegrationCovarianceMatrix> llt(covariance);
	if (llt.info() != Eigen::Success) {
		out.whitening = PreintegrationCovarianceMatrix::Identity() * 1e-3;
	} else {
		out.whitening = llt.matrixL().solve(PreintegrationCovarianceMatrix::Identity());
	}
	return out;
}

} // namespace xrt::tracking::constellation::fusion

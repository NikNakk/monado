// Copyright 2026, Beyley Cardellio
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  IMU preintegration and its factors for the experimental sliding-window fusion.
 *
 * Adapted from Monado MR 3015 (head cd7174257), `optimizer/imu_preintegration.hpp` and the preintegration half of
 * `optimizer/internal_math.cpp`, by Beyley Cardellio. The integration, covariance propagation, bias Jacobians,
 * residual and factors are upstream's. Changes:
 *  - the noise densities, random walks, anchors and bias bounds are a runtime @ref ImuNoiseModel instead of CV1
 *    (BMI055) constants, since the PS Sense IMU differs and arrives at ~66 Hz rather than 1 kHz. Upstream's values are
 *    kept as @ref ImuNoiseModel::upstreamCv1();
 *  - the residual and factor functors carry the model they were whitened with.
 *
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "xrt/xrt_tracking.h"

#include "fusion_math.hpp"

#include <span>

namespace xrt::tracking::constellation::fusion {

//! IMU noise model, whitening the preintegration and bias factors.
struct ImuNoiseModel
{
	//! White noise densities: rad/s/sqrt(Hz) and (m/s^2)/sqrt(Hz).
	double gyro_noise_density;
	double accel_noise_density;

	//! Bias random walks: (m/s^2)/sqrt(s), (rad/s)/sqrt(s), 1/sqrt(s).
	double accel_bias_random_walk;
	double gyro_bias_random_walk;
	double accel_scale_random_walk;

	//! How strongly the oldest bias is held to its seed when no prior covers it.
	double accel_bias_anchor_sigma;
	double gyro_bias_anchor_sigma;
	double accel_scale_anchor_sigma;

	//! Bounds on the solved biases and scale.
	double max_accel_bias;
	double max_gyro_bias;
	double min_accel_scale;
	double max_accel_scale;

	//! Upstream's constants for the CV1's BMI055 (imu_preintegration.hpp).
	static ImuNoiseModel
	upstreamCv1()
	{
		return {
		    .gyro_noise_density = 2.44e-4,
		    .accel_noise_density = 1.47e-3,
		    .accel_bias_random_walk = 1e-3,
		    .gyro_bias_random_walk = 1e-4,
		    .accel_scale_random_walk = 1e-5,
		    .accel_bias_anchor_sigma = 0.687,
		    .gyro_bias_anchor_sigma = 0.0175,
		    .accel_scale_anchor_sigma = 0.05,
		    .max_accel_bias = 1.0,
		    .max_gyro_bias = 0.05,
		    .min_accel_scale = 0.8,
		    .max_accel_scale = 1.0 / 0.8,
		};
	}

	/*!
	 * Defaults for the PS Sense as recorded: the noise densities the branch's EKF was tuned to on
	 * `20260926-010135-imu-capture` (they absorb host timestamp jitter and the ~15 ms sample spacing, not just sensor
	 * noise), upstream's random walks and scale, and a wider gyro-bias bound because the right controller's ~20 deg/s
	 * bias is only removed by the driver once it has been still.
	 */
	static ImuNoiseModel
	psSense()
	{
		ImuNoiseModel m = upstreamCv1();
		m.gyro_noise_density = 0.02;
		m.accel_noise_density = 0.3;
		m.gyro_bias_anchor_sigma = 0.1;
		m.max_gyro_bias = 0.4;
		return m;
	}
};

enum class ImuBiasStateIndex : int
{
	AccelBiasX,
	AccelBiasY,
	AccelBiasZ,
	GyroBiasX,
	GyroBiasY,
	GyroBiasZ,
	AccelScaleX,
	AccelScaleY,
	AccelScaleZ,
	NumIndices,
};

typedef ceres::Jet<double, static_cast<int>(ImuBiasStateIndex::NumIndices)> ImuBiasJet;

//! Accelerometer bias, gyroscope bias and per-axis accelerometer scale of one IMU.
template <typename T> struct ImuBias
{
	static constexpr int kNumParameters = static_cast<int>(ImuBiasStateIndex::NumIndices);
	static constexpr int kAccelBiasIndex = static_cast<int>(ImuBiasStateIndex::AccelBiasX);
	static constexpr int kGyroBiasIndex = static_cast<int>(ImuBiasStateIndex::GyroBiasX);
	static constexpr int kAccelScaleIndex = static_cast<int>(ImuBiasStateIndex::AccelScaleX);

	Vector3<T> accel_bias;
	Vector3<T> gyro_bias;
	Vector3<T> accel_scale;

	ImuBias() : accel_bias(Vector3<T>::Zero()), gyro_bias(Vector3<T>::Zero()), accel_scale(Vector3<T>::Constant(T(1)))
	{}

	ImuBias(const Vector3<T> &accel_bias, const Vector3<T> &gyro_bias, const Vector3<T> &accel_scale)
	    : accel_bias(accel_bias), gyro_bias(gyro_bias), accel_scale(accel_scale)
	{}

	ImuBias(Eigen::Ref<const Vector<T, kNumParameters>> parameters)
	    : accel_bias(parameters.template segment<3>(kAccelBiasIndex)),
	      gyro_bias(parameters.template segment<3>(kGyroBiasIndex)),
	      accel_scale(parameters.template segment<3>(kAccelScaleIndex))
	{}

	void
	pack(Eigen::Ref<Vector<T, kNumParameters>> parameters) const
	{
		parameters.template segment<3>(kAccelBiasIndex) = this->accel_bias;
		parameters.template segment<3>(kGyroBiasIndex) = this->gyro_bias;
		parameters.template segment<3>(kAccelScaleIndex) = this->accel_scale;
	}

	ImuBias<ImuBiasJet>
	seed() const
	{
		ImuBias<ImuBiasJet> seeded;
		for (int i = 0; i < 3; i++) {
			seeded.accel_bias[i] = ImuBiasJet(this->accel_bias[i], kAccelBiasIndex + i);
			seeded.gyro_bias[i] = ImuBiasJet(this->gyro_bias[i], kGyroBiasIndex + i);
			seeded.accel_scale[i] = ImuBiasJet(this->accel_scale[i], kAccelScaleIndex + i);
		}
		return seeded;
	}
};

constexpr int kImuBiasRotCovIndex = 0;
constexpr int kImuBiasVelCovIndex = 3;
constexpr int kImuBiasPosCovIndex = 6;
constexpr int kNumImuBiasCovIndices = 9;

//! [rotation; velocity; position], the order the residual is written in.
typedef Matrix<double, kNumImuBiasCovIndices, kNumImuBiasCovIndices> PreintegrationCovarianceMatrix;

struct PreintegratedImuSamples
{
	static constexpr int kNumResiduals = 9;

	Quaterniond Q_start_end;
	//! Delta integrated specific force.
	Vector3d delta_velocity;
	//! Delta double-integrated specific force.
	Vector3d delta_position;

	//! Lower-triangular whitening matrix, L^-1 of the propagated covariance.
	PreintegrationCovarianceMatrix whitening;

	double dt;

	Vector3d accel_bias_at_integration;
	Vector3d gyro_bias_at_integration;
	Vector3d accel_scale_at_integration;

	Matrix3d J_R_bg;
	Matrix3d J_v_ba;
	Matrix3d J_v_sa;
	Matrix3d J_v_bg;
	Matrix3d J_p_ba;
	Matrix3d J_p_sa;
	Matrix3d J_p_bg;

	//! Samples integrated, including the partial ones at either end.
	uint32_t num_steps;

	static PreintegratedImuSamples
	Identity()
	{
		PreintegratedImuSamples p{};
		p.Q_start_end = Quaterniond::Identity();
		p.delta_velocity = Vector3d::Zero();
		p.delta_position = Vector3d::Zero();
		p.whitening = PreintegrationCovarianceMatrix::Zero();
		p.dt = 0;
		p.accel_bias_at_integration = Vector3d::Zero();
		p.gyro_bias_at_integration = Vector3d::Zero();
		p.accel_scale_at_integration = Vector3d::Constant(1.0);
		p.J_R_bg = p.J_v_ba = p.J_v_sa = p.J_v_bg = p.J_p_ba = p.J_p_sa = p.J_p_bg = Matrix3d::Zero();
		p.num_steps = 0;
		return p;
	}
};

/*!
 * Preintegrate @p imu_samples (all within [start, end], ascending) over [@p start_time_ns, @p end_time_ns]. The head is
 * integrated with the first sample and the tail with @p first_sample_after_end_time, as upstream: each sample's
 * reading is held over the interval that ends at it.
 */
PreintegratedImuSamples
preintegrate(std::span<const xrt_imu_sample> imu_samples,
             const xrt_imu_sample &first_sample_after_end_time,
             const ImuBias<ImuBiasJet> &imu_bias,
             int64_t start_time_ns,
             int64_t end_time_ns,
             const ImuNoiseModel &noise);

//! Upstream's preintegration residual, [rotation; velocity; position], not yet whitened.
template <typename T>
static void
computeImuResidual(const PreintegratedImuSamples &imu_preintegration,
                   const PoseWithVelocity<T> &start_T_world_device,
                   const PoseWithVelocity<T> &end_T_world_device,
                   const ImuBias<T> &imu_bias,
                   const WorldGravity<T> &world_gravity,
                   Vector<T, PreintegratedImuSamples::kNumResiduals> &residual)
{
	Quaternion<T> Q_world_device_start;
	start_T_world_device.getQuaternion(Q_world_device_start);
	const Quaternion<T> start_Q_device_world = Q_world_device_start.conjugate();

	Quaternion<T> end_Q_world_device;
	end_T_world_device.getQuaternion(end_Q_world_device);

	const Vector3<T> gravity_vector = world_gravity.toVector();
	const T dt = T(imu_preintegration.dt);

	const Quaternion<T> end_predicted_Q_start = start_Q_device_world * end_Q_world_device;
	const Vector3<T> predicted_start_delta_velocity =
	    start_Q_device_world * (end_T_world_device.velocity - start_T_world_device.velocity - gravity_vector * dt);
	const Vector3<T> predicted_start_delta_position =
	    start_Q_device_world * (end_T_world_device.translation - start_T_world_device.translation -
	                            start_T_world_device.velocity * dt - T(0.5) * gravity_vector * dt * dt);

	const Vector3<T> delta_accel_bias = imu_bias.accel_bias - imu_preintegration.accel_bias_at_integration.cast<T>();
	const Vector3<T> delta_gyro_bias = imu_bias.gyro_bias - imu_preintegration.gyro_bias_at_integration.cast<T>();
	const Vector3<T> delta_accel_scale =
	    imu_bias.accel_scale - imu_preintegration.accel_scale_at_integration.cast<T>();

	const Quaternion<T> corrected_Q_start_end =
	    imu_preintegration.Q_start_end.cast<T>() *
	    quat_exp_so3(Vector3<T>(imu_preintegration.J_R_bg.cast<T>() * delta_gyro_bias));

	const Vector3<T> corrected_start_delta_velocity = imu_preintegration.delta_velocity.cast<T>() +
	                                                  imu_preintegration.J_v_ba.cast<T>() * delta_accel_bias +
	                                                  imu_preintegration.J_v_bg.cast<T>() * delta_gyro_bias +
	                                                  imu_preintegration.J_v_sa.cast<T>() * delta_accel_scale;

	const Vector3<T> corrected_start_delta_position = imu_preintegration.delta_position.cast<T>() +
	                                                  imu_preintegration.J_p_ba.cast<T>() * delta_accel_bias +
	                                                  imu_preintegration.J_p_bg.cast<T>() * delta_gyro_bias +
	                                                  imu_preintegration.J_p_sa.cast<T>() * delta_accel_scale;

	Quaternion<T> delta_rotation_error = corrected_Q_start_end.conjugate() * end_predicted_Q_start;
	if (delta_rotation_error.w() < T(0)) {
		delta_rotation_error.coeffs() = -delta_rotation_error.coeffs();
	}

	residual.template segment<3>(0) = quat_ln_so3(delta_rotation_error);
	residual.template segment<3>(3) = corrected_start_delta_velocity - predicted_start_delta_velocity;
	residual.template segment<3>(6) = corrected_start_delta_position - predicted_start_delta_position;
}

//! IMU preintegration factor between two keyframes.
struct ImuCostFunctor
{
	PreintegratedImuSamples preintegration;

	template <typename T>
	bool
	operator()(const T *const world_gravity_parameters,
	           const T *const imu_bias_parameters,
	           const T *const start_keyframe_parameters,
	           const T *const end_keyframe_parameters,
	           T *residuals) const
	{
		const WorldGravity<T> world_gravity{
		    Map<const Vector<T, WorldGravity<T>::kNumParameters>>(world_gravity_parameters)};
		const ImuBias<T> imu_bias{Map<const Vector<T, ImuBias<T>::kNumParameters>>(imu_bias_parameters)};
		const PoseWithVelocity<T> start{
		    Map<const Vector<T, PoseWithVelocity<T>::kNumParameters>>(start_keyframe_parameters)};
		const PoseWithVelocity<T> end{
		    Map<const Vector<T, PoseWithVelocity<T>::kNumParameters>>(end_keyframe_parameters)};

		Vector<T, PreintegratedImuSamples::kNumResiduals> computed;
		computeImuResidual<T>(this->preintegration, start, end, imu_bias, world_gravity, computed);

		Map<Vector<T, PreintegratedImuSamples::kNumResiduals>> out(residuals);
		out = this->preintegration.whitening.cast<T>() * computed;
		return true;
	}
};

typedef ceres::AutoDiffCostFunction<ImuCostFunctor,
                                    PreintegratedImuSamples::kNumResiduals,
                                    WorldGravity<double>::kNumParameters,
                                    ImuBias<double>::kNumParameters,
                                    PoseWithVelocity<double>::kNumParameters,
                                    PoseWithVelocity<double>::kNumParameters>
    ImuCostFunction;

//! Bias random walk between consecutive keyframes.
struct ImuBiasRandomWalkCostFunctor
{
	static constexpr int kNumResiduals = ImuBias<double>::kNumParameters;

	double dt;
	ImuNoiseModel noise;

	template <typename T>
	bool
	operator()(const T *const start_bias_parameters, const T *const end_bias_parameters, T *residuals) const
	{
		const ImuBias<T> start{Map<const Vector<T, ImuBias<T>::kNumParameters>>(start_bias_parameters)};
		const ImuBias<T> end{Map<const Vector<T, ImuBias<T>::kNumParameters>>(end_bias_parameters)};
		const T sqrt_dt = T(std::sqrt(this->dt));

		Map<Vector3<T>>(residuals + 0) =
		    (end.accel_bias - start.accel_bias) / (T(this->noise.accel_bias_random_walk) * sqrt_dt);
		Map<Vector3<T>>(residuals + 3) =
		    (end.gyro_bias - start.gyro_bias) / (T(this->noise.gyro_bias_random_walk) * sqrt_dt);
		Map<Vector3<T>>(residuals + 6) =
		    (end.accel_scale - start.accel_scale) / (T(this->noise.accel_scale_random_walk) * sqrt_dt);
		return true;
	}
};

typedef ceres::AutoDiffCostFunction<ImuBiasRandomWalkCostFunctor,
                                    ImuBiasRandomWalkCostFunctor::kNumResiduals,
                                    ImuBias<double>::kNumParameters,
                                    ImuBias<double>::kNumParameters>
    ImuBiasRandomWalkCostFunction;

//! Holds the oldest bias near its seed when no marginalisation prior covers it.
struct ImuBiasAnchorCostFunctor
{
	static constexpr int kNumResiduals = ImuBias<double>::kNumParameters;

	ImuBias<double> anchor;
	ImuNoiseModel noise;

	template <typename T>
	bool
	operator()(const T *const bias_parameters, T *residuals) const
	{
		const ImuBias<T> bias{Map<const Vector<T, ImuBias<T>::kNumParameters>>(bias_parameters)};
		Map<Vector3<T>>(residuals + 0) =
		    (bias.accel_bias - this->anchor.accel_bias.cast<T>()) / T(this->noise.accel_bias_anchor_sigma);
		Map<Vector3<T>>(residuals + 3) =
		    (bias.gyro_bias - this->anchor.gyro_bias.cast<T>()) / T(this->noise.gyro_bias_anchor_sigma);
		Map<Vector3<T>>(residuals + 6) =
		    (bias.accel_scale - this->anchor.accel_scale.cast<T>()) / T(this->noise.accel_scale_anchor_sigma);
		return true;
	}
};

typedef ceres::AutoDiffCostFunction<ImuBiasAnchorCostFunctor,
                                    ImuBiasAnchorCostFunctor::kNumResiduals,
                                    ImuBias<double>::kNumParameters>
    ImuBiasAnchorCostFunction;

} // namespace xrt::tracking::constellation::fusion

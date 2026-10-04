// Copyright 2026, Beyley Cardellio
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  State blocks, manifolds, LED reprojection and marginalisation for the experimental sliding-window fusion.
 *
 * Adapted from Monado MR 3015 (head cd7174257), `src/xrt/tracking/constellation/optimizer/internal_math.{hpp,cpp}`,
 * by Beyley Cardellio. Changes for the PS VR2 evaluation:
 *  - the pose-state constants that lived in `pose_optimize.hpp`/`math.hpp` are defined here, since the MR 2940 solver
 *    is not on this branch;
 *  - the LED residual projects through this branch's templated `camera_models::project` (distorted KB4 pixels, as
 *    M1 uses) instead of the CV1 undistorted pinhole path, and the blob noise is a parameter;
 *  - the unused `PitchRoll` block and `evaluateFactorFitness` are left out.
 *
 * Everything is in the OpenCV convention the constellation tracker uses internally: x right, y down, z forward, so
 * world "up" is -y and gravity points along +y.
 *
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "tracking/t_constellation.h"
#include "tracking/t_camera_models.h"
#include "tracking/t_camera_models.hpp"

#include "math/m_eigen_interop.hpp"
#include "math/m_quatexpmap.hpp"
#include "math/m_quatexpmap_bigceres.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <ceres/ceres.h>
#include <ceres/crs_matrix.h>
#include <ceres/jet.h>
#include <ceres/autodiff_manifold.h>
#include <ceres/product_manifold.h>

#include <cassert>
#include <cmath>

namespace xrt::tracking::constellation::fusion {

using namespace xrt::auxiliary::math;

using Eigen::Map;
using Eigen::Matrix;
using Eigen::Matrix3d;
using Eigen::MatrixXd;
using Eigen::Quaternion;
using Eigen::Quaterniond;
using Eigen::Vector;
using Eigen::Vector2;
using Eigen::Vector3;
using Eigen::Vector3d;
using Eigen::VectorXd;

//! Standard gravity, m/s^2.
constexpr double kGravityMS2 = 9.80665;

//! Layout of a pose parameter block: translation first, then the quaternion (x, y, z, w).
constexpr int kPoseStatePosStart = 0;
constexpr int kPoseStateRotStart = 3;

/*!
 * A parameter block that may or may not hold a meaningful value yet, with the first-estimate anchor the
 * marginalisation prior linearises it at. Unchanged from upstream.
 */
template <typename T, int N> struct SeedableVector
{
	using VectorType = Vector<T, N>;

	VectorType vec;
	bool seeded{false};

	VectorType first_estimate;
	bool anchored{false};

	T *
	data()
	{
		assert(this->seeded);
		return this->vec.data();
	}

	VectorType &
	seedVec()
	{
		this->seeded = true;
		return this->vec;
	}

	void
	anchor()
	{
		assert(this->seeded);
		if (this->anchored) {
			return;
		}
		this->first_estimate = this->vec;
		this->anchored = true;
	}

	const T *
	anchorData() const
	{
		return this->anchored ? this->first_estimate.data() : nullptr;
	}

	void
	releaseAnchor()
	{
		this->anchored = false;
	}

	void
	unseed()
	{
		this->seeded = false;
		this->anchored = false;
	}
};

//! Gravity magnitude as a parameter block. Points along +y in the OpenCV-convention world.
template <typename T> struct WorldGravity
{
	static constexpr int kNumParameters = 1;
	static constexpr int kGravityMagIndex = 0;

	T gravity_mag;

	WorldGravity() : gravity_mag(T(kGravityMS2)) {}
	explicit WorldGravity(T gravity_mag) : gravity_mag(gravity_mag) {}
	WorldGravity(Eigen::Ref<const Vector<T, kNumParameters>> parameters) : gravity_mag(parameters[kGravityMagIndex])
	{}

	void
	pack(Eigen::Ref<Vector<T, kNumParameters>> parameters) const
	{
		parameters[kGravityMagIndex] = this->gravity_mag;
	}

	Vector3<T>
	toVector() const
	{
		return Vector3<T>(T(0), this->gravity_mag, T(0));
	}
};

//! Translation plus unit quaternion; 7 ambient, 6 tangent parameters.
template <typename T> struct Pose
{
	static constexpr int kNumParameters = 7;
	static constexpr int kNumTangentParameters = 6;

	Vector3<T> translation;
	Quaternion<T> rotation;

	Pose(Quaternion<T> quat) : translation(Vector3<T>::Zero()), rotation(quat) {}

	Pose(const xrt_pose &pose) : Pose(map_quat(pose.orientation).cast<T>().normalized())
	{
		this->translation = Vector3<T>(T(pose.position.x), T(pose.position.y), T(pose.position.z));
	}

	Pose(Eigen::Ref<const Vector<T, kNumParameters>> parameters)
	    : translation(parameters.template segment<3>(kPoseStatePosStart)),
	      rotation(Quaternion<T>(parameters.template segment<4>(kPoseStateRotStart)))
	{}

	void
	pack(Eigen::Ref<Vector<T, kNumParameters>> parameters) const
	{
		parameters.template segment<3>(kPoseStatePosStart) = this->translation;
		parameters.template segment<4>(kPoseStateRotStart) = this->rotation.coeffs();
	}

	Eigen::Transform<T, 3, Eigen::Isometry>
	toTransform() const
	{
		return Eigen::Translation<T, 3>(translation.x(), translation.y(), translation.z()) *
		       Eigen::Transform<T, 3, Eigen::Isometry>(this->rotation);
	}

	void
	getQuaternion(Quaternion<T> &q) const
	{
		q = this->rotation;
	}

	xrt_pose
	toXrtPose() const
	{
		xrt_pose pose;
		map_vec3(pose.position) = this->translation.template cast<float>();
		map_quat(pose.orientation) = this->rotation.normalized().coeffs().template cast<float>();
		return pose;
	}
};

//! Pose plus world-frame linear velocity; 10 ambient, 9 tangent parameters.
template <typename T> struct PoseWithVelocity : Pose<T>
{
	static constexpr int kNumParameters = Pose<T>::kNumParameters + 3;
	static constexpr int kNumTangentParameters = Pose<T>::kNumTangentParameters + 3;

	Vector3<T> velocity;

	PoseWithVelocity(const xrt_pose &p) : Pose<T>(p), velocity(Vector3<T>::Zero()) {}

	template <typename Derived>
	PoseWithVelocity(const xrt_pose &p, const Vector3<Derived> &velocity)
	    : Pose<T>(p), velocity(velocity.template cast<T>())
	{}

	PoseWithVelocity(Eigen::Ref<const Vector<T, kNumParameters>> parameters)
	    : Pose<T>(parameters.template segment<Pose<T>::kNumParameters>(0)),
	      velocity(Vector3<T>(parameters.template segment<3>(Pose<T>::kNumParameters)))
	{}

	void
	pack(Eigen::Ref<Vector<T, kNumParameters>> parameters) const
	{
		this->Pose<T>::pack(parameters.template segment<Pose<T>::kNumParameters>(0));
		parameters.template segment<3>(Pose<T>::kNumParameters) = this->velocity;
	}
};

/*!
 * The IMU -> LED model rotation: a point in the LED model frame is `Q_imu_model * p` in the IMU frame. Upstream
 * solves for rotation only (no lever arm), and so does this.
 */
template <typename T> struct ImuExtrinsics
{
	static constexpr int kNumParameters = 4;
	static constexpr int kNumTangentParameters = 3;

	Quaternion<T> Q_imu_model;

	template <typename Derived> ImuExtrinsics(Quaternion<Derived> quat) : Q_imu_model(quat.template cast<T>()) {}

	ImuExtrinsics(Eigen::Ref<const Vector<T, kNumParameters>> parameters)
	    : Q_imu_model(Quaternion<T>(parameters.template segment<4>(0)))
	{}

	void
	pack(Eigen::Ref<Vector<T, kNumParameters>> parameters) const
	{
		parameters.template segment<4>(0) = this->Q_imu_model.coeffs();
	}
};

//! Right-multiplying quaternion manifold (upstream's local convention).
struct QuaternionManifoldFunctor
{
	template <typename T>
	bool
	Plus(const T *x, const T *delta, T *x_plus_delta) const
	{
		const Map<const Quaternion<T>> q(x);
		const Map<const Vector3<T>> w(delta);
		Map<Quaternion<T>> result(x_plus_delta);
		result = q * quat_exp_so3(w);
		return true;
	}

	template <typename T>
	bool
	Minus(const T *y, const T *x, T *y_minus_x) const
	{
		const Map<const Quaternion<T>> q_y(y);
		const Map<const Quaternion<T>> q_x(x);
		Map<Vector3<T>> result(y_minus_x);
		result = quat_ln_so3(Quaternion<T>(q_x.conjugate() * q_y));
		return true;
	}
};

typedef ceres::AutoDiffManifold<QuaternionManifoldFunctor, 4, 3> QuaternionManifold;

typedef ceres::ProductManifold<ceres::EuclideanManifold<3>, QuaternionManifold, ceres::EuclideanManifold<3>>
    PoseWithVelocityManifold;

//! Converts a CRS matrix to a dense Eigen matrix.
void
matrixCrsToEigen(const ceres::CRSMatrix &mat_crs, MatrixXd &mat);

/*!
 * Marginalise the first @p num_base_states tangent states out of the linearised system (@p jacobian, @p residual),
 * returning the square-root linear prior `|J* dx + r*|^2` over the rest. Upstream's port of OKVIS'
 * MarginalizationError::marginalizeOut(), unchanged.
 */
void
marginalize(const VectorXd &residual, const MatrixXd &jacobian, int num_base_states, VectorXd &out_e0, MatrixXd &out_j);

//! X and Y in the image, for one LED.
constexpr int kNumLedResiduals = 2;

/*!
 * Reprojection residual of one LED (model frame point @p T_model_led) seen at @p blob_px, for a device whose model
 * frame sits at (@p Q_cam_model, @p T_cam_model) in the camera. In distorted pixels, like M1.
 */
template <typename T>
static void
computeLedResidual(const t_camera_model_params &params,
                   const Vector3<T> &T_cam_model,
                   const Quaternion<T> &Q_cam_model,
                   const Vector2<T> &blob_px,
                   const Vector3<T> &T_model_led,
                   T *residual)
{
	const Vector3<T> p = Q_cam_model * T_model_led + T_cam_model;
	T u = T(1e6);
	T v = T(1e6);
	if (p.z() > T(0)) {
		(void)xrt::auxiliary::tracking::camera_models::project(params, p.x(), p.y(), p.z(), u, v);
	}
	residual[0] = u - blob_px.x();
	residual[1] = v - blob_px.y();

	// Smooth penalty for points behind the camera, as upstream.
	if (p.z() < T(0.001)) {
		residual[0] += T(1000) * (T(0.001) - p.z());
		residual[1] += T(1000) * (T(0.001) - p.z());
	}
}

} // namespace xrt::tracking::constellation::fusion

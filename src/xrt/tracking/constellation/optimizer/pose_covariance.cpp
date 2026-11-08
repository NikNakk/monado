// Copyright 2020-2023, Jan Schmidt
// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pose covariance helpers that need only Eigen, not Ceres.
 * @author Jan Schmidt <jan@centricular.com>
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup tracking
 */

#include "util/u_logging.h"

#include "math/m_eigen_interop.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/Eigenvalues>

#include "pose_optimize.hpp"

using xrt::auxiliary::math::map_quat;
using xrt::auxiliary::math::map_vec3;

namespace xrt::tracking::constellation::optimizer {

void
covariancePositionRadius(const RawPoseCovarianceMatrix &raw_covariance, xrt_vec3 &radii, xrt_quat &Q_cam_cov)
{
	typedef Eigen::Matrix<double, kPoseCovarianceSize, kPoseCovarianceSize> PoseStateCovarianceMatrix;
	Eigen::Map<const PoseStateCovarianceMatrix> covariance(raw_covariance);

	const Eigen::Matrix3d &pos_covariance =
	    covariance.block<3, 3>(static_cast<int>(CovarianceIndex::PosX), static_cast<int>(CovarianceIndex::PosX));

	Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(pos_covariance);

	if (solver.info() != Eigen::Success) {
		// @todo handle failure better, if possible
		U_LOG_E("Failed to compute eigenvalues for position covariance matrix.");
		radii = {0.0f, 0.0f, 0.0f};
		Q_cam_cov = {0.0f, 0.0f, 0.0f, 1.0f};
		return;
	}

	const Eigen::Vector3d &eigenvalues = solver.eigenvalues();
	Eigen::Matrix3d eigenvectors = solver.eigenvectors();

	if (eigenvectors.determinant() < 0.0) {
		eigenvectors.col(2) *= -1.0;
	}

	Eigen::Quaterniond q(eigenvectors);
	map_quat(Q_cam_cov) = q.cast<float>();

	Eigen::Vector3d half_sizes = 3.0 * eigenvalues.cwiseMax(0.0).cwiseSqrt();
	map_vec3(radii) = half_sizes.cast<float>();
}

}; // namespace xrt::tracking::constellation::optimizer

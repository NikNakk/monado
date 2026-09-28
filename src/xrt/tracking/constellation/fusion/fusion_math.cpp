// Copyright 2026, Beyley Cardellio
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Marginalisation helpers for the experimental sliding-window fusion.
 *
 * Adapted from Monado MR 3015 (head cd7174257), `optimizer/internal_math.cpp`, by Beyley Cardellio. The functions
 * here are unchanged apart from namespaces.
 *
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "fusion_math.hpp"

#include <Eigen/Eigenvalues>
#include <Eigen/QR>

#include <algorithm>
#include <limits>

namespace xrt::tracking::constellation::fusion {

namespace {

/*!
 * Decompose a symmetric positive semi-definite @p a into a square root and the transposed pseudo-inverse of that
 * square root, dropping eigenvalues numerically indistinguishable from zero. As OKVIS'
 * MarginalizationError::updateErrorComputation(), minus its Jacobi preconditioner.
 */
void
robustComputeSqrtAndPinvSqrt(const MatrixXd &a, MatrixXd &out_j, MatrixXd &out_j_pinv_t)
{
	Eigen::SelfAdjointEigenSolver<MatrixXd> saes(a);
	const auto &eigenvectors = saes.eigenvectors();
	const auto &eigenvalues = saes.eigenvalues();

	constexpr double epsilon = std::numeric_limits<double>::epsilon();
	const double tolerance = epsilon * static_cast<double>(a.cols()) * std::max(eigenvalues.maxCoeff(), 0.0);
	const auto keep = eigenvalues.array() > tolerance;

	const VectorXd S = keep.select(eigenvalues.array(), 0);
	const VectorXd S_pinv = keep.select(eigenvalues.array().inverse(), 0);

	out_j = S.cwiseSqrt().asDiagonal() * eigenvectors.transpose();
	out_j_pinv_t = S_pinv.cwiseSqrt().asDiagonal() * eigenvectors.transpose();
}

} // namespace

void
matrixCrsToEigen(const ceres::CRSMatrix &mat_crs, MatrixXd &mat)
{
	mat.resize(mat_crs.num_rows, mat_crs.num_cols);
	mat.setZero();
	for (int row = 0; row < mat_crs.num_rows; row++) {
		for (int n = mat_crs.rows[row]; n < mat_crs.rows[row + 1]; n++) {
			mat(row, mat_crs.cols[n]) = mat_crs.values[n];
		}
	}
}

void
marginalize(const VectorXd &residual, const MatrixXd &jacobian, int num_base_states, VectorXd &out_e0, MatrixXd &out_j)
{
	const MatrixXd H = jacobian.transpose() * jacobian;
	const VectorXd b0 = -jacobian.transpose() * residual;

	const int num_states = static_cast<int>(H.cols());
	const int num_remain_states = num_states - num_base_states;

	const MatrixXd V = H.block(0, 0, num_base_states, num_base_states);
	const MatrixXd U = H.block(num_base_states, num_base_states, num_remain_states, num_remain_states);
	const MatrixXd W = H.block(num_base_states, 0, num_remain_states, num_base_states);

	const VectorXd b_b = b0.head(num_base_states);
	const VectorXd b_a = b0.tail(num_remain_states);

	// Pseudo-inverse, so a rank-deficient block does not produce an invalid inverse.
	Eigen::CompleteOrthogonalDecomposition<MatrixXd> V_decomp(V);
	const MatrixXd V_pinv = V_decomp.pseudoInverse();

	// Schur complement, symmetrised because SelfAdjointEigenSolver reads one triangle only.
	MatrixXd H_s = U - W * V_pinv * W.transpose();
	VectorXd b0_s = b_a - W * V_pinv * b_b;
	H_s = MatrixXd(0.5 * (H_s + H_s.transpose()));

	// J* = sqrt(H*), r* = -sqrt(H*)^-T b*.
	MatrixXd j_pinv_t;
	robustComputeSqrtAndPinvSqrt(H_s, out_j, j_pinv_t);
	out_e0 = -j_pinv_t * b0_s;
}

} // namespace xrt::tracking::constellation::fusion

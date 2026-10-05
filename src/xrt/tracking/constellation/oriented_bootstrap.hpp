// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Re-acquisition of a constellation-tracked device from one camera, given its orientation.
 *
 * @ref stereo_bootstrap needs the ring in two cameras. A lost device often reappears in only one: on 5 Oct, in
 * OpenBrush, Sense rings were lit in a single camera for about 25 s (left) and 15 s (right) of a 6.5-minute session
 * without being re-acquired. Its IMU still knows its orientation, carried into the optical world by the alignment
 * from the last solve, so only the position is unknown.
 *
 * With the orientation fixed, one blob-LED correspondence constrains the position to a ray, and two fix it: the
 * position is the least-squares solution of the linear collinearity constraints. Each blob is paired only with LEDs
 * that face along its ray at the known orientation. Hypotheses are scored by projecting the whole model into that
 * camera and counting blobs explained; the best few are refined against every camera with @ref joint_solve_refine,
 * with the orientation as a prior, whose acceptance tests decide the result.
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "joint_pose_solver.hpp"

#include <cstdint>
#include <vector>

namespace xrt::tracking::constellation {

struct OrientedBootstrapParams
{
	//! Free blobs a camera needs before it is searched.
	uint32_t min_blobs{4};
	//! Blobs a hypothesis must explain in its camera before refinement is attempted.
	uint32_t min_inliers{5};
	//! A projected LED explains a blob within this distance.
	float inlier_px{3.0f};
	//! LEDs paired with a blob must face its ray this far inside their visibility cone (allows orientation error).
	float facing_margin_deg{10.0f};
	//! Hypothesised LEDs must lie in this depth range from the camera.
	float min_depth_m{0.08f};
	float max_depth_m{1.5f};
	//! Stop after this many hypotheses (bounds the cost per exposure).
	uint32_t max_hypotheses{20000};
	//! Best hypotheses refined, in order, until one is accepted.
	uint32_t refine_candidates{3};
	/*!
	 * Refinement and acceptance, with the orientation as a prior. Strict on RMS as for stereo bootstrap: there is
	 * no position prior to fall back on.
	 */
	JointSolveParams refine = [] {
		JointSolveParams p;
		p.max_rms_px = 0.8f;
		p.orientation_prior_sigma_deg = 4.0f;
		// The orientation prior rules out a pose slipped round the ring, which coverage otherwise catches; a
		// partly occluded ring reaches 0.5-0.8.
		p.min_coverage = 0.5f;
		return p;
	}();
};

struct OrientedBootstrapResult
{
	bool ok{false};
	//! Index (into the cameras passed in) of the camera whose hypothesis was accepted, or -1.
	int camera{-1};
	//! Blobs explained by the best hypothesis in its camera.
	uint32_t inliers{0};
	uint32_t hypotheses{0};
	//! The accepted (or last attempted) refinement.
	JointSolveResult refined{};
};

/*!
 * Find @p model in the cameras' free blobs given its orientation @p Tcv_world_orientation (device to the
 * OpenCV-convention world), searching each camera on its own.
 *
 * @return true if a hypothesis passed the joint refinement's acceptance tests.
 */
bool
oriented_bootstrap(const std::vector<JointSolveCamera> &cameras,
                   const t_constellation_tracker_led_model &model,
                   const xrt_quat &Tcv_world_orientation,
                   const OrientedBootstrapParams &params,
                   OrientedBootstrapResult &out);

} // namespace xrt::tracking::constellation

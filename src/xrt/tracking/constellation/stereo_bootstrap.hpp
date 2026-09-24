// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pose bootstrap for constellation-tracked devices from multi-camera triangulation.
 *
 * Finds a device with no pose prior. Blobs from every pair of synchronised cameras are unprojected to rays and paired
 * where the rays nearly intersect; the resulting 3D points are merged when several cameras see the same LED. The
 * points are registered against the device's LED model with a three-point RANSAC: point triples whose pairwise
 * distances match a model triple give a rigid transform, scored by how many points it explains. The best hypothesis is
 * then refined against every camera's blobs with @ref joint_solve_refine, whose acceptance tests decide the result.
 *
 * A proper rotation cannot map a mirror-image model onto the points, so the left and right Sense rings do not
 * register against each other's models.
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "joint_pose_solver.hpp"

#include <cstdint>
#include <vector>

namespace xrt::tracking::constellation {

struct StereoBootstrapParams
{
	//! Two rays pair when they pass within this distance of each other.
	float max_ray_gap_m{0.004f};
	//! Triangulated points must lie within this range of both cameras.
	float min_depth_m{0.08f};
	float max_depth_m{2.0f};
	//! Points closer than this are the same LED seen by different camera pairs.
	float merge_radius_m{0.004f};
	//! Pairwise distance tolerance when matching point triples to model triples.
	float distance_tolerance_m{0.004f};
	//! A transformed LED explains a point within this distance.
	float inlier_radius_m{0.005f};
	//! Minimum points explained by the best hypothesis before refinement is attempted.
	uint32_t min_inliers{4};
	//! Stop after this many rigid hypotheses (bounds the cost per exposure).
	uint32_t max_hypotheses{20000};
	/*!
	 * Refinement and acceptance. Stricter on RMS than frame-to-frame tracking: with no prior to fall back on, a
	 * near-symmetric wrong fit (for example the other hand's ring) must not pass. Correct real solves are 0.3-0.5 px
	 * (p95 0.75 px while moving); a mirror-image fit on the synthetic ring reached 0.96 px.
	 */
	JointSolveParams refine = [] {
		JointSolveParams p;
		p.max_rms_px = 0.8f;
		return p;
	}();
};

struct StereoBootstrapResult
{
	bool ok{false};
	//! Triangulated (merged) points and the number explained by the best hypothesis.
	uint32_t points{0};
	uint32_t inliers{0};
	uint32_t hypotheses{0};
	//! The accepted (or last attempted) refinement.
	JointSolveResult refined{};
};

/*!
 * Find @p model in the cameras' free blobs (those without an owner) with no prior.
 *
 * @return true if a hypothesis was found and passed the joint refinement's acceptance tests.
 */
bool
stereo_bootstrap(const std::vector<JointSolveCamera> &cameras,
                 const t_constellation_tracker_led_model &model,
                 const StereoBootstrapParams &params,
                 StereoBootstrapResult &out);

} // namespace xrt::tracking::constellation

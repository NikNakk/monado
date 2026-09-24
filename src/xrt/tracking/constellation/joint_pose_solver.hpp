// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Joint multi-camera pose refinement for constellation-tracked devices.
 *
 * Refines one device pose against the blobs of every synchronised camera at once, instead of solving each camera
 * separately and averaging. From a prior pose the device's visible LEDs are projected into every camera, associated
 * with the nearest free blobs inside a gate, and a robust (Huber) Gauss-Newton step is taken on all correspondences
 * together. Association and optimisation alternate, with the gate shrinking.
 *
 * All poses are in the OpenCV convention the constellation tracker uses internally (x right, y down, z forward).
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "xrt/xrt_defines.h"

#include "tracking/t_constellation.h"
#include "tracking/t_camera_models.h"

#include <cstdint>
#include <vector>

namespace xrt::tracking::constellation {

struct JointSolveCamera
{
	//! Camera pose in the (OpenCV-convention) world.
	xrt_pose Tcv_world_cam;
	const t_camera_model_params *model;
	int width;
	int height;
	const t_blob *blobs;
	uint32_t blob_count;
	/*!
	 * Optional, blob_count long: device id that already owns each blob (another device solved first), or
	 * XRT_CONSTELLATION_INVALID_DEVICE_ID. Owned blobs are skipped.
	 */
	const t_constellation_device_id_t *blob_owner;
};

struct JointSolveParams
{
	//! Association gates in pixels, one per outer iteration (largest first).
	std::vector<float> gates_px{16.0f, 8.0f, 4.0f, 3.0f};
	//! Gauss-Newton steps per association.
	int steps_per_gate{3};
	//! Residuals beyond this many pixels are down-weighted (Huber).
	float huber_px{1.5f};
	//! Minimum correspondences across all cameras to accept a pose.
	uint32_t min_matches{5};
	//! Maximum RMS reprojection error to accept a pose. Correct live solves are 0.1-0.4 px; a solve one LED round an
	//! ambiguous ring can still reach ~1.5 px.
	float max_rms_px{1.0f};
	/*!
	 * Minimum fraction of the LEDs predicted visible at the solved pose that must have found a blob. A pose that has
	 * slipped round the ring predicts LEDs where there are none, which RMS alone does not catch.
	 */
	float min_coverage{0.8f};
	/*!
	 * A correspondence with a residual above @ref outlier_px counts as an outlier; more than @ref max_outlier_fraction
	 * of them rejects the pose. A wrong pose concentrates its error in a few correspondences, while calibration error
	 * across the rig (~0.5 px RMS, p95 0.93 px) spreads thinly.
	 */
	float outlier_px{2.0f};
	float max_outlier_fraction{0.1f};

	/*!
	 * Orientation prior (typically the IMU), as a standard deviation in degrees; 0 disables it. A ring at arm's length
	 * constrains its own tilt weakly, and without this the solve can settle several degrees off with a sub-pixel RMS.
	 * Its residual is scaled so one sigma costs as much as one pixel of reprojection error.
	 */
	float orientation_prior_sigma_deg{0.0f};
};

struct JointSolveMatch
{
	uint32_t camera;
	uint32_t blob;
	uint32_t led;
	float residual_px;
};

struct JointSolveResult
{
	bool ok{false};
	xrt_pose Tcv_world_device{};
	uint32_t matches{0};
	uint32_t cameras_used{0};
	float rms_px{0.0f};
	//! LEDs predicted visible (in any camera) at the solved pose, and matches / visible.
	uint32_t visible_leds{0};
	float coverage{0.0f};
	uint32_t outliers{0};
	std::vector<JointSolveMatch> correspondences;
};

/*!
 * Refine @p prior (device pose in the OpenCV-convention world) against every camera's blobs.
 *
 * @return true if the pose converged with at least @ref JointSolveParams::min_matches correspondences and an RMS
 * below @ref JointSolveParams::max_rms_px. @p out is filled either way, for diagnostics.
 */
bool
joint_solve_refine(const std::vector<JointSolveCamera> &cameras,
                   const t_constellation_tracker_led_model &model,
                   const xrt_pose &prior,
                   const JointSolveParams &params,
                   JointSolveResult &out);

/*!
 * As above, with a separate orientation prior (same world) used when @ref JointSolveParams::orientation_prior_sigma_deg
 * is set. @p prior still provides the starting pose.
 */
bool
joint_solve_refine(const std::vector<JointSolveCamera> &cameras,
                   const t_constellation_tracker_led_model &model,
                   const xrt_pose &prior,
                   const xrt_quat &orientation_prior,
                   const JointSolveParams &params,
                   JointSolveResult &out);

} // namespace xrt::tracking::constellation

// Copyright 2015, Philipp Zabel
// Copyright 2020-2023
// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  RANSAC PnP pose refinement
 * @author Philipp Zabel <philipp.zabel@gmail.com>
 * @author Jan Schmidt <jan@centricular.com>
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup tracking
 */

#pragma once

#include "xrt/xrt_defines.h"

#include "tracking/t_constellation.h"

#include "constellation/camera_model.h"


namespace xrt::tracking::constellation::optimizer {

enum class PoseStateIndex : int
{
	// Pos
	PosX = 0,
	PosY = 1,
	PosZ = 2,

	// Quat (must match xrt_quat/Eigen ordering)
	RotX = 3,
	RotY = 4,
	RotZ = 5,
	RotW = 6,

	NumIndices,
};

constexpr int kPoseStatePosStart = static_cast<int>(PoseStateIndex::PosX);
constexpr int kPoseStateRotStart = static_cast<int>(PoseStateIndex::RotX);
constexpr int kPoseStateSize = static_cast<int>(PoseStateIndex::NumIndices);

/*!
 * Does a pose optimization and RANSAC refinement on the given blobs and LED model, returning the optimized pose.
 *
 * @param log_level           The logging level to use for debug output.
 * @param params              The camera model parameters.
 * @param init_pose           The initial pose estimate to start the optimization from.
 * @param blobs               The array of blobs to use for the optimization, will write to here to unmark outliers.
 * @param num_blobs           The number of blobs in the array.
 * @param leds_model          The LED model to use for the optimization.
 * @param device_id           The device ID to optimize the pose for.
 * @param[out] out_pose       The optimized pose will be written to this variable.
 *
 * @return true if the optimization was successful, false otherwise.
 */
bool
optimizePose(u_logging_level log_level,
             bool deterministic,
             const camera_model &params,
             xrt_pose init_pose,
             t_blob *blobs,
             uint32_t num_blobs,
             t_constellation_tracker_led_model *leds_model,
             t_constellation_device_id_t device_id,
             xrt_pose &out_pose);

}; // namespace xrt::tracking::constellation::optimizer

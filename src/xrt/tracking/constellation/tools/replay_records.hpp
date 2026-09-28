// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  What the M1/M2 replay frontend solved at each exposure, kept so several backends can consume identical
 *         optical input.
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "tracking/t_constellation.h"

#include <cstdint>
#include <vector>

namespace xrt::tracking::constellation {

//! One blob-to-LED correspondence of an accepted joint solve.
struct FrontendMatch
{
	uint32_t camera_index;
	//! The camera's world pose at the exposure (CV), as recorded: T_world_HMD(t) * T_HMD_camera.
	xrt_pose Tcv_world_cam;
	//! Blob centre, distorted pixels.
	xrt_vec2 px;
	//! Index into the device's LED model.
	uint32_t led;
};

//! One accepted M1 (tracking) or M2 (bootstrap) solve.
struct FrontendRecord
{
	int64_t timestamp_ns;
	t_constellation_device_id_t device_id;
	//! LED model pose in the world (CV).
	xrt_pose Tcv_world_device;
	float rms_px;
	uint32_t cameras_used;
	uint32_t matches;
	bool bootstrapped;
	//! Frontend cost of this solve (or bootstrap attempt).
	double solve_us;
	std::vector<FrontendMatch> correspondences;
};

} // namespace xrt::tracking::constellation

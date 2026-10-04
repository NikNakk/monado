// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Offline comparison of the optical frontend, the branch's EKF and the experimental sliding-window fusion on
 *         identical recorded input (constellation_replay --fusion-compare).
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "t_constellation_tracker_dataset.hpp"
#include "replay_records.hpp"

#include <string>
#include <vector>

namespace xrt::tracking::constellation {

struct FusionCompareOptions
{
	//! The session's run.log, for LED-sync / clock / gyro-bias epochs. Null: one epoch for the whole run.
	const char *run_log{nullptr};
	//! Write per-exposure outputs to PREFIX-<scenario>.csv.
	const char *out_prefix{nullptr};
	//! The Sense IMU mounting rotation about x, degrees.
	double imu_angle_deg{50.27};
	//! Comma-separated: nominal, dropout, corrupt.
	std::string scenarios{"nominal,dropout,corrupt"};
};

/*!
 * @param exposure_times Every exposure of the recording (the evaluation timeline), ascending.
 * @param records        The M1/M2 frontend's accepted solves (replay_m1), the optical input every path shares.
 */
int
fusion_compare(const DatasetReader &dataset,
               const std::vector<int64_t> &exposure_times,
               const std::vector<FrontendRecord> &records,
               const FusionCompareOptions &options);

} // namespace xrt::tracking::constellation

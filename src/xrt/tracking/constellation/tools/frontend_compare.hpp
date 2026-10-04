// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Neutral scoring of optical front ends on one recording (constellation_replay --compare-frontend).
 *
 * Each front end's poses are judged by the same evaluator, independently of how the front end scored itself: its
 * LEDs are reprojected into every camera of the exposure and matched against the recorded blobs; consecutive poses
 * are checked against the gyro; stationary stretches give jitter; and, when the recording carries ground truth, the
 * pose error is measured directly.
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "t_constellation_tracker_dataset.hpp"
#include "replay_records.hpp"

#include <string>
#include <vector>

namespace xrt::tracking::constellation {

//! One front end's output on a recording.
struct FrontendRun
{
	std::string name;
	//! At most one per (exposure, device), each stamped with its exposure's time.
	std::vector<FrontendRecord> records;
	//! Front-end cost per exposure, all devices, microseconds. Empty if unknown.
	std::vector<double> exposure_us;
	//! The header comment of an imported records file.
	std::string source;
};

/*!
 * Import a records CSV (constellation_upstream_replay's "constellation frontend records v1"). A front end that
 * solves per camera yields one pose per camera; the exposure keeps the camera with the most matched blobs (then the
 * lowest RMS) for its pose, and the matches of every camera that produced a pose as its correspondences.
 *
 * @param exposure_times Every exposure of the recording, ascending.
 */
bool
load_frontend_records(const DatasetReader &dataset,
                      const std::vector<int64_t> &exposure_times,
                      const char *path,
                      FrontendRun &out);

//! Score each run and print a side-by-side report; with @p out_csv, also one row per scored pose.
int
frontend_compare(const DatasetReader &dataset,
                 const std::vector<int64_t> &exposure_times,
                 const std::vector<FrontendRun> &runs,
                 const char *out_csv);

} // namespace xrt::tracking::constellation

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-camera PnP optimisation when Monado is built without Ceres.
 *
 * The per-camera (outside-in) path needs the full Ceres solver. Builds without
 * it keep the rest of constellation tracking, including the joint multi-camera
 * path used by PS VR2 Sense tracking, which does its own Eigen-based solve.
 * Here the per-camera optimisation reports that it found no pose.
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "util/u_logging.h"

#include "pose_optimize.hpp"

#include <atomic>

namespace xrt::tracking::constellation::optimizer {

namespace {

void
warn_once()
{
	static std::atomic<bool> warned{false};
	if (!warned.exchange(true)) {
		U_LOG_W("Constellation per-camera pose optimisation needs Ceres, which this build does not include; "
		        "only the joint multi-camera path can produce poses.");
	}
}

} // namespace

bool
optimizePose(u_logging_level log_level,
             bool deterministic,
             const t_camera_model_params &params,
             xrt_pose init_pose,
             t_blob *blobs,
             uint32_t num_blobs,
             t_constellation_tracker_led_model *leds_model,
             t_constellation_device_id_t device_id,
             xrt_pose &out_pose,
             RawPoseCovarianceMatrix out_covariance,
             RawPoseCovarianceMatrix out_whitening)
{
	warn_once();
	return false;
}

void
computePoseCovariance(u_logging_level log_level,
                      const t_camera_model_params &params,
                      xrt_pose init_pose,
                      t_blob *blobs,
                      uint32_t num_blobs,
                      t_constellation_tracker_led_model *leds_model,
                      t_constellation_device_id_t device_id,
                      RawPoseCovarianceMatrix out_covariance,
                      RawPoseCovarianceMatrix out_whitening)
{
	warn_once();
	// Report no confidence rather than leaving the outputs undefined.
	for (int i = 0; i < kPoseCovarianceSize * kPoseCovarianceSize; i++) {
		out_covariance[i] = (i % (kPoseCovarianceSize + 1) == 0) ? 1e6 : 0.0;
		if (out_whitening != nullptr) {
			out_whitening[i] = (i % (kPoseCovarianceSize + 1) == 0) ? 1e-3 : 0.0;
		}
	}
}

}; // namespace xrt::tracking::constellation::optimizer

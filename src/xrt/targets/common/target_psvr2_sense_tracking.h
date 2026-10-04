// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PS Sense optical (6DoF) tracking with the PS VR2's cameras: the calibration loader and head-pose tracking
 *         origin shared with monado-cli, and the experimental, opt-in runtime pipeline (PSVR2_SENSE_6DOF=1).
 * @author Nick Kennedy
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "tracking/t_constellation.h"

#include "constellation/t_constellation_tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;

/*!
 * Tracking origin for the camera mosaic: the PS VR2 head pose at the requested time, times camera 0 in the head
 * frame. With it, controller poses are in the headset's tracking space rather than relative to the headset.
 */
struct psvr2_head_tracking_origin
{
	struct t_constellation_tracker_tracking_source base;
	struct xrt_device *head;
	struct xrt_pose head_from_camera0;
	//! Receives the head pose and its SLAM provenance for each query, when a dataset is being recorded.
	struct t_constellation_tracker *tracker;
};

//! Set up @p origin for @p head; head_from_camera0 is left for the caller (usually from the calibration).
void
psvr2_head_tracking_origin_init(struct psvr2_head_tracking_origin *origin, struct xrt_device *head);

/*!
 * Load a `psvr2-mode4-constellation-calibration-v1` file into @p out_params (one mosaic, four cameras) and its
 * optional head_from_camera0_xrt (identity when absent).
 */
bool
psvr2_constellation_load_calibration(const char *path,
                                     struct t_constellation_tracker_params *out_params,
                                     struct xrt_pose *out_head_from_camera0,
                                     bool *out_have_head_from_camera0);

/*!
 * Whether the experimental runtime pipeline is requested (PSVR2_SENSE_6DOF=1). When it is, this also sets defaults
 * for the headset and controller options it needs (mode-4 camera streams, the joint tracker, the IMU + optical
 * filter, the LED phase bootstrap, the LED model correction). Variables already set are left alone. Call it before
 * the headset and the controllers are created.
 */
bool
psvr2_sense_tracking_requested(void);

/*!
 * Start optical tracking of the Sense controllers with the headset's cameras, in the headset's tracking space. Needs
 * the headset in camera mode 4 and PSVR2_SENSE_6DOF_CALIBRATION naming a calibration file. Either controller may be
 * NULL. The pipeline stops itself first thing in the headset's destroy (psvr2_set_teardown_hook), while all the
 * devices still exist.
 *
 * @return true if it started; false (logged) leaves the controllers orientation-only.
 */
bool
psvr2_sense_tracking_start(struct xrt_device *head, struct xrt_device *left, struct xrt_device *right);

#ifdef __cplusplus
}
#endif

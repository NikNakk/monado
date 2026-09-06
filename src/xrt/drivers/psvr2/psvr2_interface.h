// Copyright 2020-2021, Collabora, Ltd.
// Copyright 2023, Jan Schmidt
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PSVR2 HMD device
 *
 * @author Jan Schmidt <jan@centricular.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup drv_psvr2
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @defgroup drv_psvr2 PS VR2 HMD driver
 * @ingroup drv
 *
 * @brief Driver for the Playstation VR2 headset
 *
 */

#define PSVR2_VID 0x054C
#define PSVR2_PID 0x0CDE

/*!
 * Minimal timing snapshot for diagnostics outside the private PSVR2 driver.
 *
 * The *_vts_ns fields are in the headset VTS clock domain. The corresponding
 * *_monotonic_ns fields are mapped into Monado's os_monotonic_get_ns() domain
 * using the driver's current VTS-to-monotonic clock offset.
 */
struct psvr2_slam_timing
{
	bool valid;
	int timestamp_samples;
	int64_t slam_vts_ns;
	int64_t slam_monotonic_ns;
	int64_t imu_vts_ns;
	int64_t imu_monotonic_ns;
	int64_t hw2mono_vts_ns;
};

/*!
 * Create the PS VR2 HMD device
 *
 * @ingroup drv_psvr2
 */
struct xrt_device *
psvr2_hmd_create(struct xrt_prober_device *xpdev);

/*!
 * Snapshot PSVR2 SLAM/IMU clock state for diagnostics.
 *
 * @param xdev A PSVR2 xrt_device.
 * @param out Timing snapshot to fill.
 * @return true when the arguments are valid and a snapshot was taken.
 *         Check out->valid to determine whether timestamp calibration and a
 *         SLAM sample are available yet.
 */
bool
psvr2_get_slam_timing(struct xrt_device *xdev, struct psvr2_slam_timing *out);

/*!
 * Probing function for PlayStation VR2 devices.
 *
 * @ingroup drv_psvr2
 * @see xrt_prober_found_func_t
 */
int
psvr2_found(struct xrt_prober *xp,
            struct xrt_prober_device **devices,
            size_t device_count,
            size_t index,
            cJSON *attached_data,
            struct xrt_device **out_xdevs);

/*!
 * @dir drivers/psvr2
 *
 * @brief @ref drv_psvr2 files.
 */


#ifdef __cplusplus
}
#endif

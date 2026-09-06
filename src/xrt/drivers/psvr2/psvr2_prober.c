// Copyright 2020-2021, Collabora, Ltd.
// Copyright 2023, Jan Schmidt
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PSVR2 HMD device prober
 *
 * @author Jan Schmidt <jan@centricular.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup drv_psvr2
 */

#include "xrt/xrt_prober.h"

#include "util/u_misc.h"
#include "util/u_debug.h"

#include "psvr2.h"
#include "psvr2_interface.h"

#include <string.h>


bool
psvr2_get_slam_timing(struct xrt_device *xdev, struct psvr2_slam_timing *out)
{
	if (xdev == NULL || out == NULL) {
		return false;
	}

	*out = (struct psvr2_slam_timing){0};

	/* Avoid treating an arbitrary driver's xrt_device as psvr2_hmd. */
	if (strstr(xdev->str, "PS VR2") == NULL) {
		return false;
	}

	struct psvr2_hmd *hmd = psvr2_hmd(xdev);

	os_mutex_lock(&hmd->data_lock);
	out->timestamp_samples = hmd->timestamp_samples;
	out->slam_vts_ns = (int64_t)hmd->last_slam_vts_ns;
	out->imu_vts_ns = (int64_t)hmd->last_imu_vts_ns;
	out->hw2mono_vts_ns = (int64_t)hmd->hw2mono_vts;
	out->slam_monotonic_ns = out->slam_vts_ns + out->hw2mono_vts_ns;
	out->imu_monotonic_ns = out->imu_vts_ns + out->hw2mono_vts_ns;
	out->valid = hmd->timestamp_samples >= TIMESTAMP_SAMPLES && hmd->last_slam_vts_ns != 0;
	os_mutex_unlock(&hmd->data_lock);

	return true;
}


int
psvr2_found(struct xrt_prober *xp,
            struct xrt_prober_device **devices,
            size_t device_count,
            size_t index,
            cJSON *attached_data,
            struct xrt_device **out_xdevs)
{
	struct xrt_device *hmd = psvr2_hmd_create(devices[index]);
	if (hmd == NULL) {
		return -1;
	}

	*out_xdevs = hmd;
	return 1;
}

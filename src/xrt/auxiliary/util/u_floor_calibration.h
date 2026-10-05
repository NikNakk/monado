// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Automatic floor calibration of the managed STAGE from eye height.
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_space_overseer;

//! The head must stay within this distance for the whole steady window.
#define U_FLOOR_CALIBRATION_MAX_MOTION_M 0.01f
//! How long the head must be steady, worn, tracked and level.
#define U_FLOOR_CALIBRATION_STEADY_NS (1000 * 1000 * 1000)
//! Largest head pitch accepted as looking ahead, in degrees.
#define U_FLOOR_CALIBRATION_MAX_PITCH_DEG 20.0f

/*!
 * One head observation, in the space overseer's root space.
 */
struct u_floor_calibration_sample
{
	int64_t timestamp_ns;
	bool worn;    //!< The head device reports it is worn, or cannot tell.
	bool tracked; //!< The head position is valid and tracked.
	struct xrt_pose head;
};

/*!
 * Waits for a worn, tracked, level and steady head, then derives the floor
 * height as head height minus the configured eye height.
 */
struct u_floor_calibration
{
	float eye_height_m;
	bool done;

	bool steady;
	int64_t steady_since_ns;
	struct xrt_vec3 anchor;
	double height_sum;
	uint32_t height_count;
};

void
u_floor_calibration_init(struct u_floor_calibration *fc, float eye_height_m);

/*!
 * Feed one head sample. Returns true, with the floor height in root, once the
 * head has been worn, tracked, level and steady for the whole window.
 */
bool
u_floor_calibration_push(struct u_floor_calibration *fc,
                         const struct u_floor_calibration_sample *sample,
                         float *out_floor_y);

/*!
 * Sample @p head through @p xso at @p now_ns and, once the floor is known, set
 * the managed STAGE height to it. Does nothing if the STAGE has already been
 * moved, for example by an explicit calibration. Returns true when finished,
 * whether the floor was applied or skipped.
 */
bool
u_floor_calibration_poll(struct u_floor_calibration *fc,
                         struct xrt_space_overseer *xso,
                         struct xrt_device *head,
                         int64_t now_ns);

#ifdef __cplusplus
}
#endif

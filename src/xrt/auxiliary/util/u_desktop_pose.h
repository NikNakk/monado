// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Head pose steered from desktop input, for a simulated HMD.
 * @ingroup aux_util
 *
 * A window (the macOS presenter today) feeds pointer and scroll deltas in;
 * the simulated HMD reads the resulting pose. Opt-in with SIMULATED_DESKTOP=1.
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//! True when SIMULATED_DESKTOP=1: simulate the HMD in a desktop window.
bool
u_desktop_pose_enabled(void);

//! Turn the head. Positive yaw turns left, positive pitch looks up (radians).
//! Pitch is clamped short of straight up and down.
void
u_desktop_pose_rotate(float yaw_rad, float pitch_rad);

//! Move along the floor relative to the current heading (metres).
void
u_desktop_pose_move(float right_m, float forward_m);

//! Back to the starting pose.
void
u_desktop_pose_reset(void);

//! Pose relative to the HMD's starting pose. Thread safe.
void
u_desktop_pose_get(struct xrt_pose *out_pose);

#ifdef __cplusplus
}
#endif

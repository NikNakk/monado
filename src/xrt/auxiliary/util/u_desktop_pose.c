// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Head pose steered from desktop input, for a simulated HMD.
 * @ingroup aux_util
 */

#include "util/u_desktop_pose.h"
#include "util/u_debug.h"

#include "math/m_mathinclude.h"
#include <pthread.h>

DEBUG_GET_ONCE_BOOL_OPTION(simulated_desktop, "SIMULATED_DESKTOP", false)

// Keep a little short of +-90 degrees so the view never flips.
#define PITCH_LIMIT_RAD 1.5533430f // 89 degrees

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static float g_yaw, g_pitch, g_x, g_z;

bool
u_desktop_pose_enabled(void)
{
	return debug_get_bool_option_simulated_desktop();
}

void
u_desktop_pose_rotate(float yaw_rad, float pitch_rad)
{
	pthread_mutex_lock(&g_mutex);
	g_yaw = remainderf(g_yaw + yaw_rad, 2.0f * (float)M_PI);
	g_pitch = fminf(fmaxf(g_pitch + pitch_rad, -PITCH_LIMIT_RAD), PITCH_LIMIT_RAD);
	pthread_mutex_unlock(&g_mutex);
}

void
u_desktop_pose_move(float right_m, float forward_m)
{
	pthread_mutex_lock(&g_mutex);
	// Heading -Z rotated by yaw about +Y: forward (-sin, 0, -cos), right (cos, 0, -sin).
	float s = sinf(g_yaw);
	float c = cosf(g_yaw);
	g_x += right_m * c - forward_m * s;
	g_z += -right_m * s - forward_m * c;
	pthread_mutex_unlock(&g_mutex);
}

void
u_desktop_pose_reset(void)
{
	pthread_mutex_lock(&g_mutex);
	g_yaw = g_pitch = g_x = g_z = 0.0f;
	pthread_mutex_unlock(&g_mutex);
}

void
u_desktop_pose_get(struct xrt_pose *out_pose)
{
	pthread_mutex_lock(&g_mutex);
	float yaw = g_yaw, pitch = g_pitch, x = g_x, z = g_z;
	pthread_mutex_unlock(&g_mutex);

	// Yaw about world +Y, then pitch about the head's +X: q = q_yaw * q_pitch.
	float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
	float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
	out_pose->orientation.w = cy * cp;
	out_pose->orientation.x = cy * sp;
	out_pose->orientation.y = sy * cp;
	out_pose->orientation.z = -sy * sp;
	out_pose->position.x = x;
	out_pose->position.y = 0.0f;
	out_pose->position.z = z;
}

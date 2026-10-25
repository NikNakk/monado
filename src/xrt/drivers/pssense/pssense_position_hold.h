// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_defines.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/*!
 * PSSENSE_HOLD_POSITION: once optical and inertial position validity have expired, keep reporting the last valid
 * (or, with PSSENSE_COAST_POSITION, the stillness-gated coasted)
 * position (valid, never tracked, zero linear velocity) while orientation stays valid. Without this, an
 * orientation-only relation reaches OpenXR as a position at the tracking origin, metres from the user.
 */
struct pssense_position_hold
{
	bool have;
	struct xrt_vec3 position;

	//! PSSENSE_COAST_POSITION state, advanced per IMU sample by pssense_position_coast_step.
	bool coasting;
	struct xrt_vec3 anchor;
	int64_t coast_start_ns;
	int64_t last_ns;
};

/*!
 * Stillness-gated coast limits. Offline (25 Oct survey replay) integrating the filter velocity only while the IMU shows
 * motion beat both holding and an unbounded coast at every gap length from 0.5 to 3 s.
 */
#define PSSENSE_COAST_MAX_NS (3 * 1000000000LL)
#define PSSENSE_COAST_MAX_DISPLACEMENT_M 0.8f
#define PSSENSE_COAST_MAX_SPEED_M_S 4.0f
#define PSSENSE_COAST_MOVING_GYRO_RAD_S 0.3f
#define PSSENSE_COAST_MOVING_ACCEL_M_S2 0.5f
#define PSSENSE_COAST_MAX_STEP_NS 50000000LL

static inline float
pssense_position_hold_len(struct xrt_vec3 v)
{
	return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

/*!
 * One IMU sample. While @p filter_rel has a valid position, track it. Afterwards, add the filter's linear velocity
 * times the sample interval only while the IMU shows motion, for at most PSSENSE_COAST_MAX_NS, at plausible speed, and
 * within PSSENSE_COAST_MAX_DISPLACEMENT_M of where the coast began. Otherwise the position stays put.
 */
static inline void
pssense_position_coast_step(struct pssense_position_hold *hold,
                            int64_t now_ns,
                            const struct xrt_space_relation *filter_rel,
                            struct xrt_vec3 gyro_rad_s,
                            struct xrt_vec3 accel_m_s2)
{
	if ((filter_rel->relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0) {
		hold->have = true;
		hold->position = filter_rel->pose.position;
		hold->coasting = false;
		hold->last_ns = now_ns;
		return;
	}
	if (!hold->have) {
		return;
	}
	if (!hold->coasting) {
		hold->coasting = true;
		hold->anchor = hold->position;
		hold->coast_start_ns = now_ns;
		hold->last_ns = now_ns;
		return;
	}
	int64_t dt_ns = now_ns - hold->last_ns;
	hold->last_ns = now_ns;
	if (dt_ns <= 0 || dt_ns > PSSENSE_COAST_MAX_STEP_NS || now_ns - hold->coast_start_ns > PSSENSE_COAST_MAX_NS ||
	    (filter_rel->relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) == 0) {
		return;
	}
	bool moving = pssense_position_hold_len(gyro_rad_s) > PSSENSE_COAST_MOVING_GYRO_RAD_S ||
	              fabsf(pssense_position_hold_len(accel_m_s2) - 9.80665f) > PSSENSE_COAST_MOVING_ACCEL_M_S2;
	struct xrt_vec3 v = filter_rel->linear_velocity;
	float speed = pssense_position_hold_len(v);
	if (!moving || !isfinite(speed) || speed > PSSENSE_COAST_MAX_SPEED_M_S) {
		return;
	}
	float dt = (float)dt_ns * 1e-9f;
	struct xrt_vec3 d = {hold->position.x + v.x * dt - hold->anchor.x, hold->position.y + v.y * dt - hold->anchor.y,
	                     hold->position.z + v.z * dt - hold->anchor.z};
	float dist = pssense_position_hold_len(d);
	if (dist > PSSENSE_COAST_MAX_DISPLACEMENT_M) {
		float s = PSSENSE_COAST_MAX_DISPLACEMENT_M / dist;
		d.x *= s;
		d.y *= s;
		d.z *= s;
	}
	hold->position.x = hold->anchor.x + d.x;
	hold->position.y = hold->anchor.y + d.y;
	hold->position.z = hold->anchor.z + d.z;
}

static inline void
pssense_position_hold_apply(struct pssense_position_hold *hold, struct xrt_space_relation *rel)
{
	if ((rel->relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0) {
		hold->have = true;
		hold->position = rel->pose.position;
		return;
	}
	if (!hold->have || (rel->relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) == 0) {
		return;
	}
	rel->pose.position = hold->position;
	rel->linear_velocity.x = 0;
	rel->linear_velocity.y = 0;
	rel->linear_velocity.z = 0;
	rel->relation_flags = (enum xrt_space_relation_flags)(
	    (rel->relation_flags | XRT_SPACE_RELATION_POSITION_VALID_BIT) &
	    ~(XRT_SPACE_RELATION_POSITION_TRACKED_BIT | XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT));
}

static inline void
pssense_position_hold_reset(struct pssense_position_hold *hold)
{
	hold->have = false;
	hold->coasting = false;
}

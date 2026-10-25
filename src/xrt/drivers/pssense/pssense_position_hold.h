// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>

/*!
 * PSSENSE_HOLD_POSITION: once optical and inertial position validity have expired, keep reporting the last valid
 * position (valid, never tracked, zero linear velocity) while orientation stays valid. Without this, an
 * orientation-only relation reaches OpenXR as a position at the tracking origin, metres from the user.
 */
struct pssense_position_hold
{
	bool have;
	struct xrt_vec3 position;
};

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
}

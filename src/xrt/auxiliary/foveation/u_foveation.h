// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Graphics-API-independent foveation policy profiles.
 *
 * Profiles express normalized rasterization-rate policy only. Graphics
 * backends translate these rates into their native mechanisms (for example,
 * Metal variable rasterization-rate maps). Keeping profile selection here
 * prevents application/runtime policy from being coupled to one graphics API.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum u_foveation_profile_index
{
	U_FOVEATION_PROFILE_REFERENCE = 0,
	U_FOVEATION_PROFILE_STRONG,
	U_FOVEATION_PROFILE_AGGRESSIVE,
	U_FOVEATION_PROFILE_AGGRESSIVE_PLUS,
	U_FOVEATION_PROFILE_NEAR_EXTREME,
	U_FOVEATION_PROFILE_EXTREME,
	U_FOVEATION_PROFILE_COUNT,
};

struct u_foveation_profile
{
	const char *name;

	//! Normalized rate for the gaze-centred region.
	float center_rate;

	//! Normalized rate for the transition region around the centre.
	float middle_rate;

	//! Normalized rate for the peripheral region.
	float peripheral_rate;

	//! Half-extent of the full-rate centre, normalized to one view dimension.
	float center_half_extent;

	//! Half-extent of the centre + transition region, normalized likewise.
	float middle_half_extent;
};

const struct u_foveation_profile *
u_foveation_profile_get(int profile_index);

int
u_foveation_profile_find(const char *name);

#ifdef __cplusplus
}
#endif

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

#include <stdbool.h>
#include <stdint.h>

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

/*!
 * API-independent coarse foveation levels. These mirror the common
 * NONE/LOW/MEDIUM/HIGH model used by registered OpenXR foveation extensions
 * without making the utility layer depend on OpenXR headers.
 */
enum u_foveation_level
{
	U_FOVEATION_LEVEL_NONE = 0,
	U_FOVEATION_LEVEL_LOW,
	U_FOVEATION_LEVEL_MEDIUM,
	U_FOVEATION_LEVEL_HIGH,
};

/*!
 * Runtime policy requested by a client-facing foveation API.
 *
 * profile_index is the maximum requested fixed profile when enabled.
 * dynamic allows the runtime to reduce foveation strength at run time.
 * eye_tracked means the runtime owns gaze selection; it does not imply that
 * gaze data is exposed to the requesting application.
 */
struct u_foveation_request
{
	bool enabled;
	int profile_index;
	bool dynamic;
	bool eye_tracked;
	float vertical_offset_degrees;
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

/*!
 * Convert a coarse standards-facing level into the internal profile family.
 * NONE disables foveation; LOW/MEDIUM/HIGH map to progressively stronger
 * profiles while retaining the finer experimental profiles for runtime use.
 */
bool
u_foveation_request_from_level(enum u_foveation_level level,
                               bool dynamic,
                               bool eye_tracked,
                               float vertical_offset_degrees,
                               struct u_foveation_request *out_request);


/*!
 * Return the per-axis normalized rasterization rate for an absolute normalized
 * distance from the foveation centre. A distance of 0 is at gaze; 0.5 is half
 * of one view dimension away.
 */
float
u_foveation_profile_rate_for_offset(const struct u_foveation_profile *profile, float normalized_offset);

/*!
 * Fill one axis of a discrete foveation grid using the generic profile policy.
 *
 * Samples are addressed by index so graphics backends can choose their native
 * grid resolution. This intentionally matches the established 16-zone Metal
 * behaviour when sample_count is 16.
 */
bool
u_foveation_build_axis_rates(const struct u_foveation_profile *profile,
                             uint32_t sample_count,
                             uint32_t center_index,
                             float *out_rates);

#ifdef __cplusplus
}
#endif

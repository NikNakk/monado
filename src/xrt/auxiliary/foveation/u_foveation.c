// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "foveation/u_foveation.h"

#include <stddef.h>
#include <string.h>

/*
 * The half-extents preserve the proven 16-zone layout while expressing the
 * policy independently of any graphics API: three full-rate zones span 3/16
 * of the view and the centre+transition region spans 7/16.
 */
static const struct u_foveation_profile k_profiles[U_FOVEATION_PROFILE_COUNT] = {
    {"reference", 1.00f, 0.70f, 0.45f, 0.09375f, 0.21875f},
    {"strong", 1.00f, 0.60f, 0.35f, 0.09375f, 0.21875f},
    {"aggressive", 1.00f, 0.50f, 0.25f, 0.09375f, 0.21875f},
    {"aggressive-plus", 1.00f, 0.46f, 0.23f, 0.09375f, 0.21875f},
    {"near-extreme", 1.00f, 0.43f, 0.21f, 0.09375f, 0.21875f},
    {"extreme", 1.00f, 0.40f, 0.20f, 0.09375f, 0.21875f},
};

const struct u_foveation_profile *
u_foveation_profile_get(int profile_index)
{
	if (profile_index < 0 || profile_index >= U_FOVEATION_PROFILE_COUNT) {
		return NULL;
	}
	return &k_profiles[profile_index];
}

int
u_foveation_profile_find(const char *name)
{
	if (name == NULL) {
		return -1;
	}
	for (int i = 0; i < U_FOVEATION_PROFILE_COUNT; ++i) {
		if (strcmp(name, k_profiles[i].name) == 0) {
			return i;
		}
	}
	return -1;
}

bool
u_foveation_request_from_level(enum u_foveation_level level,
                               bool dynamic,
                               bool eye_tracked,
                               float vertical_offset_degrees,
                               struct u_foveation_request *out_request)
{
	if (out_request == NULL) {
		return false;
	}

	struct u_foveation_request request = {
	    .enabled = true,
	    .profile_index = U_FOVEATION_PROFILE_REFERENCE,
	    .dynamic = dynamic,
	    .eye_tracked = eye_tracked,
	    .vertical_offset_degrees = vertical_offset_degrees,
	};

	switch (level) {
	case U_FOVEATION_LEVEL_NONE:
		request.enabled = false;
		request.profile_index = U_FOVEATION_PROFILE_REFERENCE;
		break;
	case U_FOVEATION_LEVEL_LOW: request.profile_index = U_FOVEATION_PROFILE_REFERENCE; break;
	case U_FOVEATION_LEVEL_MEDIUM: request.profile_index = U_FOVEATION_PROFILE_STRONG; break;
	case U_FOVEATION_LEVEL_HIGH: request.profile_index = U_FOVEATION_PROFILE_AGGRESSIVE; break;
	default: return false;
	}

	*out_request = request;
	return true;
}

float
u_foveation_profile_rate_for_offset(const struct u_foveation_profile *profile, float normalized_offset)
{
	if (profile == NULL) {
		return 1.0f;
	}

	if (normalized_offset < 0.0f) {
		normalized_offset = -normalized_offset;
	}

	if (normalized_offset <= profile->center_half_extent) {
		return profile->center_rate;
	}
	if (normalized_offset <= profile->middle_half_extent) {
		return profile->middle_rate;
	}
	return profile->peripheral_rate;
}

bool
u_foveation_build_axis_rates(const struct u_foveation_profile *profile,
                             uint32_t sample_count,
                             uint32_t center_index,
                             float *out_rates)
{
	if (profile == NULL || out_rates == NULL || sample_count == 0 || center_index >= sample_count) {
		return false;
	}

	for (uint32_t i = 0; i < sample_count; ++i) {
		const uint32_t delta = i > center_index ? i - center_index : center_index - i;
		const float normalized_offset = (float)delta / (float)sample_count;
		out_rates[i] = u_foveation_profile_rate_for_offset(profile, normalized_offset);
	}

	return true;
}

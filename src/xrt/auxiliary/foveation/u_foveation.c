// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "foveation/u_foveation.h"

#include <stddef.h>
#include <string.h>

static const struct u_foveation_profile k_profiles[U_FOVEATION_PROFILE_COUNT] = {
    {"reference", 1.00f, 0.70f, 0.45f},
    {"strong", 1.00f, 0.60f, 0.35f},
    {"aggressive", 1.00f, 0.50f, 0.25f},
    {"aggressive-plus", 1.00f, 0.46f, 0.23f},
    {"near-extreme", 1.00f, 0.43f, 0.21f},
    {"extreme", 1.00f, 0.40f, 0.20f},
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

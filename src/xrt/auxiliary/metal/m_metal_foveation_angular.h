// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include "foveation/u_foveation_angular.h"
#ifdef __cplusplus
extern "C" {
#endif
struct m_metal_foveation_angular;
struct m_metal_foveation_angular_result
{
	void *map;
	uint32_t width, height, revision, builds, cache_count;
	struct u_foveation_angular_history history;
	struct u_foveation_angular_recipe recipe;
};
struct m_metal_foveation_angular *
m_metal_foveation_angular_create(void *device, const struct u_foveation_angular_config *config);
void
m_metal_foveation_angular_destroy(struct m_metal_foveation_angular *cache);
/* On failure neither history nor the previous borrowed map changes. */
bool
m_metal_foveation_angular_lookup(struct m_metal_foveation_angular *cache,
                                 const struct u_foveation_angular_target *target,
                                 bool gaze_valid,
                                 float yaw,
                                 float pitch,
                                 bool diagnostics,
                                 struct m_metal_foveation_angular_result *out);
#ifdef __cplusplus
}
#endif

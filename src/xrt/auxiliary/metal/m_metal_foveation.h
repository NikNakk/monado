// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Reusable Metal variable-rate foveation profile and map helper.
 *
 * This interface deliberately exposes Metal objects as opaque pointers so
 * OpenXR clients can use it without pulling Objective-C types into C/C++
 * headers. A successfully-built rate map is retained and must eventually be
 * released with m_metal_foveation_map_release(), unless ownership is
 * transferred to an Objective-C owner which releases the MTL object itself.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define M_METAL_FOVEATION_ZONE_COUNT 16
#define M_METAL_FOVEATION_BOUNDARY_COUNT 129
#define M_METAL_FOVEATION_PROFILE_COUNT 6

struct m_metal_foveation_profile
{
	const char *name;
	float middle_rate;
	float peripheral_rate;
};

struct m_metal_foveation_map
{
	void *rate_map;
	size_t physical_width;
	size_t physical_height;
	float x[M_METAL_FOVEATION_BOUNDARY_COUNT];
	float y[M_METAL_FOVEATION_BOUNDARY_COUNT];
};

const struct m_metal_foveation_profile *
m_metal_foveation_profile_get(int profile_index);

int
m_metal_foveation_profile_find(const char *name);

bool
m_metal_foveation_map_build(void *metal_device,
                            uint32_t screen_width,
                            uint32_t screen_height,
                            int zone_x,
                            int zone_y,
                            int profile_index,
                            struct m_metal_foveation_map *out_map);

void
m_metal_foveation_map_release(struct m_metal_foveation_map *map);

#ifdef __cplusplus
}
#endif

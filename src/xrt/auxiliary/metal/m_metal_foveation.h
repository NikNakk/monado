// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Metal backend for graphics-API-independent foveation policy.
 *
 * This interface deliberately exposes Metal objects as opaque pointers so
 * OpenXR clients can use it without pulling Objective-C types into C/C++
 * headers. Generic profile selection lives in foveation/u_foveation.h; this
 * backend only translates a resolved policy profile into Metal VRR state.
 *
 * A successfully-built rate map is retained and must eventually be released
 * with m_metal_foveation_map_release(), unless ownership is transferred to an
 * Objective-C owner which releases the MTL object itself.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "foveation/u_foveation.h"

#ifdef __cplusplus
extern "C" {
#endif

#define M_METAL_FOVEATION_ZONE_COUNT 16
#define M_METAL_FOVEATION_BOUNDARY_COUNT 129
struct m_metal_foveation_map
{
	void *rate_map;
	size_t physical_width;
	size_t physical_height;
	uint32_t sample_count;
	float horizontal_rates[M_METAL_FOVEATION_ZONE_COUNT];
	float vertical_rates[M_METAL_FOVEATION_ZONE_COUNT];
	float x[M_METAL_FOVEATION_BOUNDARY_COUNT];
	float y[M_METAL_FOVEATION_BOUNDARY_COUNT];
};

bool
m_metal_foveation_map_build(void *metal_device,
                            uint32_t screen_width,
                            uint32_t screen_height,
                            int zone_x,
                            int zone_y,
                            const struct u_foveation_profile *profile,
                            struct m_metal_foveation_map *out_map);

/*!
 * Build one Metal map whose full target contains multiple foveal centres.
 * This is needed by packed-stereo clients such as Chromium, where two OpenXR
 * views occupy sub-rectangles of one 2D swapchain image.
 */
bool
m_metal_foveation_map_build_for_zones(void *metal_device,
                                      uint32_t screen_width,
                                      uint32_t screen_height,
                                      const uint32_t *zone_x,
                                      const uint32_t *zone_y,
                                      const float *scale_x,
                                      const float *scale_y,
                                      uint32_t center_count,
                                      const struct u_foveation_profile *profile,
                                      struct m_metal_foveation_map *out_map);

void
m_metal_foveation_map_release(struct m_metal_foveation_map *map);

#ifdef __cplusplus
}
#endif

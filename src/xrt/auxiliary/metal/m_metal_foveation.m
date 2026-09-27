// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import <Metal/Metal.h>

#include "metal/m_metal_foveation.h"

#include <stdlib.h>
#include <string.h>

static const struct m_metal_foveation_profile k_profiles[M_METAL_FOVEATION_PROFILE_COUNT] = {
    {"reference", 0.70f, 0.45f},
    {"strong", 0.60f, 0.35f},
    {"aggressive", 0.50f, 0.25f},
    {"aggressive-plus", 0.46f, 0.23f},
    {"near-extreme", 0.43f, 0.21f},
    {"extreme", 0.40f, 0.20f},
};

const struct m_metal_foveation_profile *
m_metal_foveation_profile_get(int profile_index)
{
	if (profile_index < 0 || profile_index >= M_METAL_FOVEATION_PROFILE_COUNT) {
		return NULL;
	}
	return &k_profiles[profile_index];
}

int
m_metal_foveation_profile_find(const char *name)
{
	if (name == NULL) {
		return -1;
	}
	for (int i = 0; i < M_METAL_FOVEATION_PROFILE_COUNT; ++i) {
		if (strcmp(name, k_profiles[i].name) == 0) {
			return i;
		}
	}
	return -1;
}

bool
m_metal_foveation_map_build(void *metal_device,
                            uint32_t screen_width,
                            uint32_t screen_height,
                            int zone_x,
                            int zone_y,
                            int profile_index,
                            struct m_metal_foveation_map *out_map)
{
	if (metal_device == NULL || out_map == NULL || screen_width == 0 || screen_height == 0 ||
	    zone_x < 0 || zone_x >= M_METAL_FOVEATION_ZONE_COUNT ||
	    zone_y < 0 || zone_y >= M_METAL_FOVEATION_ZONE_COUNT) {
		return false;
	}

	const struct m_metal_foveation_profile *profile = m_metal_foveation_profile_get(profile_index);
	if (profile == NULL) {
		return false;
	}

	id<MTLDevice> device = (id<MTLDevice>)metal_device;
	if (![device supportsRasterizationRateMapWithLayerCount:1]) {
		return false;
	}

	float horizontal[M_METAL_FOVEATION_ZONE_COUNT];
	float vertical[M_METAL_FOVEATION_ZONE_COUNT];
	for (int i = 0; i < M_METAL_FOVEATION_ZONE_COUNT; ++i) {
		const int dx = abs(i - zone_x);
		const int dy = abs(i - zone_y);
		horizontal[i] = dx <= 1 ? 1.0f : (dx <= 3 ? profile->middle_rate : profile->peripheral_rate);
		vertical[i] = dy <= 1 ? 1.0f : (dy <= 3 ? profile->middle_rate : profile->peripheral_rate);
	}

	MTLRasterizationRateLayerDescriptor *layer =
	    [[MTLRasterizationRateLayerDescriptor alloc]
	        initWithSampleCount:MTLSizeMake(M_METAL_FOVEATION_ZONE_COUNT, M_METAL_FOVEATION_ZONE_COUNT, 1)
	                 horizontal:horizontal
	                   vertical:vertical];
	if (layer == nil) {
		return false;
	}

	MTLRasterizationRateMapDescriptor *descriptor = [[MTLRasterizationRateMapDescriptor alloc] init];
	descriptor.screenSize = MTLSizeMake(screen_width, screen_height, 1);
	[descriptor setLayer:layer atIndex:0];
	id<MTLRasterizationRateMap> rate_map = [device newRasterizationRateMapWithDescriptor:descriptor];
	[layer release];
	[descriptor release];
	if (rate_map == nil) {
		return false;
	}

	const MTLSize physical_size = [rate_map physicalSizeForLayer:0];
	if (physical_size.width == 0 || physical_size.height == 0) {
		[rate_map release];
		return false;
	}

	out_map->rate_map = (void *)rate_map;
	out_map->physical_width = physical_size.width;
	out_map->physical_height = physical_size.height;
	for (uint32_t boundary = 0; boundary < M_METAL_FOVEATION_BOUNDARY_COUNT; ++boundary) {
		const float fraction = (float)boundary / (float)(M_METAL_FOVEATION_BOUNDARY_COUNT - 1);
		const float logical_x = (float)screen_width * fraction;
		const float logical_y = (float)screen_height * fraction;
		const MTLCoordinate2D px =
		    [rate_map mapScreenToPhysicalCoordinates:MTLCoordinate2DMake(logical_x, 0.0) forLayer:0];
		const MTLCoordinate2D py =
		    [rate_map mapScreenToPhysicalCoordinates:MTLCoordinate2DMake(0.0, logical_y) forLayer:0];
		out_map->x[boundary] = (float)px.x / (float)screen_width;
		out_map->y[boundary] = (float)py.y / (float)screen_height;
	}

	return true;
}

void
m_metal_foveation_map_release(struct m_metal_foveation_map *map)
{
	if (map == NULL) {
		return;
	}
	if (map->rate_map != NULL) {
		id<MTLRasterizationRateMap> rate_map = (id<MTLRasterizationRateMap>)map->rate_map;
		[rate_map release];
	}
	memset(map, 0, sizeof(*map));
}

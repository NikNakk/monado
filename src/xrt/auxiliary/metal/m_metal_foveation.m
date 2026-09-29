// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import <Metal/Metal.h>

#include "metal/m_metal_foveation.h"

#include <math.h>
#include <string.h>

bool
m_metal_foveation_map_build_for_zones(void *metal_device,
                                      uint32_t screen_width,
                                      uint32_t screen_height,
                                      const uint32_t *zone_x,
                                      const uint32_t *zone_y,
                                      uint32_t center_count,
                                      const struct u_foveation_profile *profile,
                                      struct m_metal_foveation_map *out_map)
{
	if (metal_device == NULL || profile == NULL || out_map == NULL || screen_width == 0 || screen_height == 0 ||
	    zone_x == NULL || zone_y == NULL || center_count == 0) {
		return false;
	}
	for (uint32_t center = 0; center < center_count; ++center) {
		if (zone_x[center] >= M_METAL_FOVEATION_ZONE_COUNT || zone_y[center] >= M_METAL_FOVEATION_ZONE_COUNT) {
			return false;
		}
	}

	id<MTLDevice> device = (id<MTLDevice>)metal_device;
	if (![device supportsRasterizationRateMapWithLayerCount:1]) {
		return false;
	}

	float horizontal[M_METAL_FOVEATION_ZONE_COUNT] = {0};
	float vertical[M_METAL_FOVEATION_ZONE_COUNT] = {0};
	for (uint32_t sample = 0; sample < M_METAL_FOVEATION_ZONE_COUNT; ++sample) {
		for (uint32_t center = 0; center < center_count; ++center) {
			const uint32_t dx = sample > zone_x[center] ? sample - zone_x[center] : zone_x[center] - sample;
			const uint32_t dy = sample > zone_y[center] ? sample - zone_y[center] : zone_y[center] - sample;
			const float x_rate = u_foveation_profile_rate_for_offset(
		    profile, (float)dx / (float)M_METAL_FOVEATION_ZONE_COUNT);
			const float y_rate = u_foveation_profile_rate_for_offset(
		    profile, (float)dy / (float)M_METAL_FOVEATION_ZONE_COUNT);
			horizontal[sample] = fmaxf(horizontal[sample], x_rate);
			vertical[sample] = fmaxf(vertical[sample], y_rate);
		}
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

	memset(out_map, 0, sizeof(*out_map));
	out_map->rate_map = (void *)rate_map;
	out_map->physical_width = physical_size.width;
	out_map->physical_height = physical_size.height;
	out_map->sample_count = M_METAL_FOVEATION_ZONE_COUNT;
	memcpy(out_map->horizontal_rates, horizontal, sizeof(horizontal));
	memcpy(out_map->vertical_rates, vertical, sizeof(vertical));
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

bool
m_metal_foveation_map_build(void *metal_device,
                            uint32_t screen_width,
                            uint32_t screen_height,
                            int zone_x,
                            int zone_y,
                            const struct u_foveation_profile *profile,
                            struct m_metal_foveation_map *out_map)
{
	if (zone_x < 0 || zone_y < 0) {
		return false;
	}
	const uint32_t zones_x[1] = {(uint32_t)zone_x};
	const uint32_t zones_y[1] = {(uint32_t)zone_y};
	return m_metal_foveation_map_build_for_zones(
	    metal_device, screen_width, screen_height, zones_x, zones_y, 1, profile, out_map);
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

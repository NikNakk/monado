// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import <Metal/Metal.h>

#include "comp_metal_foveation_cache.h"
#include "metal/m_metal_foveation.h"
#include "foveation/u_foveation.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>


struct comp_metal_foveation_cache_entry
{
	uint32_t revision;
	uint32_t view_index;
	uint32_t array_layer;
	struct m_metal_foveation_map map;
	struct comp_metal_foveation_cache_entry *next;
};


static float
clampf01(float value)
{
	if (value < 0.0f) {
		return 0.0f;
	}
	if (value > 1.0f) {
		return 1.0f;
	}
	return value;
}


static void
center_to_zone(const struct xrt_foveation_view_state *view, int *out_x, int *out_y)
{
	if (view == NULL || !view->center_valid) {
		*out_x = -1;
		*out_y = -1;
		return;
	}

	const float u = clampf01(0.5f * (view->center.x + 1.0f));
	const float v = clampf01(0.5f * (1.0f - view->center.y));
	const int zone_count = M_METAL_FOVEATION_ZONE_COUNT;
	*out_x = (int)fminf((float)(zone_count - 1), floorf(u * (float)zone_count));
	*out_y = (int)fminf((float)(zone_count - 1), floorf(v * (float)zone_count));
}

static bool
foveation_maps_equivalent(const struct xrt_foveation_state *a,
                          const struct xrt_foveation_state *b)
{
	if (a->enabled != b->enabled) {
		return false;
	}
	if (!a->enabled) {
		return true;
	}

	if (a->center_rate != b->center_rate ||
	    a->middle_rate != b->middle_rate ||
	    a->peripheral_rate != b->peripheral_rate ||
	    a->center_half_extent != b->center_half_extent ||
	    a->middle_half_extent != b->middle_half_extent ||
	    a->view_count != b->view_count) {
		return false;
	}

	for (uint32_t i = 0; i < a->view_count; ++i) {
		if (a->views[i].center_valid != b->views[i].center_valid) {
			return false;
		}
		int ax = -1, ay = -1, bx = -1, by = -1;
		center_to_zone(&a->views[i], &ax, &ay);
		center_to_zone(&b->views[i], &bx, &by);
		if (ax != bx || ay != by) {
			return false;
		}
	}

	return true;
}

static void
release_cached_entries(struct comp_metal_foveation_cache *cache)
{
	release_cached_entries(cache);
}

bool
comp_metal_foveation_cache_init(struct comp_metal_foveation_cache *cache,
                                void *metal_device,
                                uint32_t logical_width,
                                uint32_t logical_height,
                                uint32_t array_size)
{
	if (cache == NULL || metal_device == NULL || logical_width == 0 || logical_height == 0 || array_size == 0) {
		return false;
	}

	memset(cache, 0, sizeof(*cache));
	cache->metal_device = (void *)[(__bridge id<MTLDevice>)metal_device retain];
	cache->logical_width = logical_width;
	cache->logical_height = logical_height;
	cache->array_size = array_size;
	cache->revision = 1;

	if (os_mutex_init(&cache->mutex) != 0) {
		[(__bridge id<MTLDevice>)cache->metal_device release];
		memset(cache, 0, sizeof(*cache));
		return false;
	}

	return true;
}

void
comp_metal_foveation_cache_destroy(struct comp_metal_foveation_cache *cache)
{
	if (cache == NULL || cache->metal_device == NULL) {
		return;
	}

	os_mutex_lock(&cache->mutex);
	struct comp_metal_foveation_cache_entry *entry =
	    (struct comp_metal_foveation_cache_entry *)cache->entries;
	while (entry != NULL) {
		struct comp_metal_foveation_cache_entry *next = entry->next;
		m_metal_foveation_map_release(&entry->map);
		free(entry);
		entry = next;
	}
	cache->entries = NULL;
	void *device = cache->metal_device;
	cache->metal_device = NULL;
	os_mutex_unlock(&cache->mutex);
	os_mutex_destroy(&cache->mutex);

	[(__bridge id<MTLDevice>)device release];
	memset(cache, 0, sizeof(*cache));
}

xrt_result_t
comp_metal_foveation_cache_set(struct comp_metal_foveation_cache *cache,
                               const struct xrt_foveation_state *state)
{
	if (cache == NULL || cache->metal_device == NULL || state == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	os_mutex_lock(&cache->mutex);
	const bool same_map = foveation_maps_equivalent(&cache->state, state);
	cache->state = *state;
	if (!same_map) {
		/*
		 * Once an application accepts a new revision, pointers returned for
		 * the old revision are no longer part of the API state. Metal command
		 * encoders retain objects they have already encoded, so dropping the
		 * cache's ownership here does not invalidate submitted GPU work.
		 */
		release_cached_entries(cache);
		cache->revision++;
		if (cache->revision == 0) {
			cache->revision = 1;
		}
	}
	os_mutex_unlock(&cache->mutex);

	return XRT_SUCCESS;
}

xrt_result_t
comp_metal_foveation_cache_get(struct comp_metal_foveation_cache *cache,
                               uint32_t view_index,
                               uint32_t array_layer,
                               struct xrt_metal_foveation_state *out_state)
{
	if (cache == NULL || cache->metal_device == NULL || out_state == NULL || array_layer >= cache->array_size) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	os_mutex_lock(&cache->mutex);

	const uint32_t revision = cache->revision;
	const struct xrt_foveation_state state = cache->state;
	if (!state.enabled) {
		*out_state = (struct xrt_metal_foveation_state){
		    .enabled = false,
		    .rasterization_rate_map = NULL,
		    .physical_width = cache->logical_width,
		    .physical_height = cache->logical_height,
		    .revision = revision,
		};
		os_mutex_unlock(&cache->mutex);
		return XRT_SUCCESS;
	}

	if (state.view_count == 0 || view_index >= state.view_count || !state.views[view_index].center_valid) {
		os_mutex_unlock(&cache->mutex);
		return XRT_ERROR_NOT_IMPLEMENTED;
	}

	for (struct comp_metal_foveation_cache_entry *entry =
	         (struct comp_metal_foveation_cache_entry *)cache->entries;
	     entry != NULL;
	     entry = entry->next) {
		if (entry->revision == revision && entry->view_index == view_index && entry->array_layer == array_layer) {
			*out_state = (struct xrt_metal_foveation_state){
			    .enabled = true,
			    .rasterization_rate_map = entry->map.rate_map,
			    .physical_width = entry->map.physical_width,
			    .physical_height = entry->map.physical_height,
			    .revision = revision,
			};
			out_state->compositor_map.enabled = 1;
			out_state->compositor_map.boundary_count = M_METAL_FOVEATION_BOUNDARY_COUNT;
			memcpy(out_state->compositor_map.x, entry->map.x, sizeof(out_state->compositor_map.x));
			memcpy(out_state->compositor_map.y, entry->map.y, sizeof(out_state->compositor_map.y));
			os_mutex_unlock(&cache->mutex);
			return XRT_SUCCESS;
		}
	}

	const struct xrt_foveation_view_state *view = &state.views[view_index];
	int zone_x = -1;
	int zone_y = -1;
	center_to_zone(view, &zone_x, &zone_y);
	if (zone_x < 0 || zone_y < 0) {
		os_mutex_unlock(&cache->mutex);
		return XRT_ERROR_NOT_IMPLEMENTED;
	}

	struct u_foveation_profile profile = {
	    .name = "xrt",
	    .center_rate = state.center_rate,
	    .middle_rate = state.middle_rate,
	    .peripheral_rate = state.peripheral_rate,
	    .center_half_extent = state.center_half_extent,
	    .middle_half_extent = state.middle_half_extent,
	};

	struct comp_metal_foveation_cache_entry *entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		os_mutex_unlock(&cache->mutex);
		return XRT_ERROR_ALLOCATION;
	}

	if (!m_metal_foveation_map_build(cache->metal_device,
	                                  cache->logical_width,
	                                  cache->logical_height,
	                                  zone_x,
	                                  zone_y,
	                                  &profile,
	                                  &entry->map)) {
		free(entry);
		os_mutex_unlock(&cache->mutex);
		return XRT_ERROR_NOT_IMPLEMENTED;
	}

	entry->revision = revision;
	entry->view_index = view_index;
	entry->array_layer = array_layer;
	entry->next = (struct comp_metal_foveation_cache_entry *)cache->entries;
	cache->entries = entry;

	*out_state = (struct xrt_metal_foveation_state){
	    .enabled = true,
	    .rasterization_rate_map = entry->map.rate_map,
	    .physical_width = entry->map.physical_width,
	    .physical_height = entry->map.physical_height,
	    .revision = revision,
	};
	out_state->compositor_map.enabled = 1;
	out_state->compositor_map.boundary_count = M_METAL_FOVEATION_BOUNDARY_COUNT;
	memcpy(out_state->compositor_map.x, entry->map.x, sizeof(out_state->compositor_map.x));
	memcpy(out_state->compositor_map.y, entry->map.y, sizeof(out_state->compositor_map.y));

	os_mutex_unlock(&cache->mutex);
	return XRT_SUCCESS;
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import <Metal/Metal.h>

#include "comp_metal_foveation_cache.h"
#include "metal/m_metal_foveation.h"
#include "foveation/u_foveation.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(M_METAL_FOVEATION_ZONE_COUNT == XRT_METAL_FOVEATION_ZONE_COUNT,
               "Metal foveation sample counts must stay in sync");

struct comp_metal_foveation_cache_entry
{
	uint32_t revision;
	uint32_t view_index;
	uint32_t array_layer;
	bool packed;
	uint32_t packed_view_count;
	struct xrt_metal_foveation_view_layout packed_views[XRT_MAX_VIEWS];
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
clear_active_entries(struct comp_metal_foveation_cache *cache)
{
	if (cache->active_entries != NULL) {
		memset(cache->active_entries, 0, sizeof(void *) * cache->array_size);
	}
}

static void
release_cached_entries(struct comp_metal_foveation_cache *cache)
{
	struct comp_metal_foveation_cache_entry *entry =
	    (struct comp_metal_foveation_cache_entry *)cache->entries;
	while (entry != NULL) {
		struct comp_metal_foveation_cache_entry *next = entry->next;
		m_metal_foveation_map_release(&entry->map);
		free(entry);
		entry = next;
	}
	cache->entries = NULL;
	clear_active_entries(cache);
}

static void
fill_native_state(const struct comp_metal_foveation_cache_entry *entry,
                  struct xrt_metal_foveation_state *out_state)
{
	*out_state = (struct xrt_metal_foveation_state){
	    .enabled = true,
	    .rasterization_rate_map = entry->map.rate_map,
	    .physical_width = (uint32_t)entry->map.physical_width,
	    .physical_height = (uint32_t)entry->map.physical_height,
	    .revision = entry->revision,
	    .sample_count = entry->map.sample_count,
	};
	memcpy(out_state->horizontal_rates, entry->map.horizontal_rates,
	       sizeof(out_state->horizontal_rates));
	memcpy(out_state->vertical_rates, entry->map.vertical_rates,
	       sizeof(out_state->vertical_rates));
	out_state->compositor_map.enabled = 1;
	out_state->compositor_map.boundary_count = M_METAL_FOVEATION_BOUNDARY_COUNT;
	memcpy(out_state->compositor_map.x, entry->map.x, sizeof(out_state->compositor_map.x));
	memcpy(out_state->compositor_map.y, entry->map.y, sizeof(out_state->compositor_map.y));
}

static void
fill_disabled_state(const struct comp_metal_foveation_cache *cache,
                    struct xrt_metal_foveation_state *out_state)
{
	*out_state = (struct xrt_metal_foveation_state){
	    .enabled = false,
	    .rasterization_rate_map = NULL,
	    .physical_width = cache->logical_width,
	    .physical_height = cache->logical_height,
	    .revision = cache->revision,
	};
}

static struct u_foveation_profile
profile_from_state(const struct xrt_foveation_state *state)
{
	return (struct u_foveation_profile){
	    .name = "xrt",
	    .center_rate = state->center_rate,
	    .middle_rate = state->middle_rate,
	    .peripheral_rate = state->peripheral_rate,
	    .center_half_extent = state->center_half_extent,
	    .middle_half_extent = state->middle_half_extent,
	};
}

static bool
same_packed_layout(const struct comp_metal_foveation_cache_entry *entry,
                   const struct xrt_metal_foveation_view_layout *views,
                   uint32_t view_count)
{
	return entry->packed && entry->packed_view_count == view_count &&
	       memcmp(entry->packed_views, views, view_count * sizeof(*views)) == 0;
}

bool
comp_metal_foveation_cache_init(struct comp_metal_foveation_cache *cache,
                                void *metal_device,
                                uint32_t logical_width,
                                uint32_t logical_height,
                                uint32_t array_size)
{
	if (cache == NULL || metal_device == NULL || logical_width == 0 ||
	    logical_height == 0 || array_size == 0) {
		return false;
	}

	memset(cache, 0, sizeof(*cache));
	cache->metal_device = (void *)[(__bridge id<MTLDevice>)metal_device retain];
	cache->logical_width = logical_width;
	cache->logical_height = logical_height;
	cache->array_size = array_size;
	cache->revision = 1;
	cache->active_entries = calloc(array_size, sizeof(void *));
	if (cache->active_entries == NULL) {
		[(__bridge id<MTLDevice>)cache->metal_device release];
		memset(cache, 0, sizeof(*cache));
		return false;
	}

	if (os_mutex_init(&cache->mutex) != 0) {
		free(cache->active_entries);
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
	release_cached_entries(cache);
	free(cache->active_entries);
	cache->active_entries = NULL;
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
	if (cache == NULL || cache->metal_device == NULL || out_state == NULL ||
	    array_layer >= cache->array_size) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	os_mutex_lock(&cache->mutex);
	const uint32_t revision = cache->revision;
	const struct xrt_foveation_state state = cache->state;
	if (!state.enabled) {
		fill_disabled_state(cache, out_state);
		os_mutex_unlock(&cache->mutex);
		return XRT_SUCCESS;
	}
	if (state.view_count == 0 || view_index >= state.view_count ||
	    !state.views[view_index].center_valid) {
		os_mutex_unlock(&cache->mutex);
		return XRT_ERROR_NOT_IMPLEMENTED;
	}

	for (struct comp_metal_foveation_cache_entry *entry =
	         (struct comp_metal_foveation_cache_entry *)cache->entries;
	     entry != NULL; entry = entry->next) {
		if (!entry->packed && entry->revision == revision &&
		    entry->view_index == view_index && entry->array_layer == array_layer) {
			cache->active_entries[array_layer] = entry;
			fill_native_state(entry, out_state);
			os_mutex_unlock(&cache->mutex);
			return XRT_SUCCESS;
		}
	}

	int zone_x = -1, zone_y = -1;
	center_to_zone(&state.views[view_index], &zone_x, &zone_y);
	if (zone_x < 0 || zone_y < 0) {
		os_mutex_unlock(&cache->mutex);
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	const struct u_foveation_profile profile = profile_from_state(&state);
	struct comp_metal_foveation_cache_entry *entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		os_mutex_unlock(&cache->mutex);
		return XRT_ERROR_ALLOCATION;
	}
	if (!m_metal_foveation_map_build(cache->metal_device, cache->logical_width,
	                                  cache->logical_height, zone_x, zone_y,
	                                  &profile, &entry->map)) {
		free(entry);
		os_mutex_unlock(&cache->mutex);
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	entry->revision = revision;
	entry->view_index = view_index;
	entry->array_layer = array_layer;
	entry->next = (struct comp_metal_foveation_cache_entry *)cache->entries;
	cache->entries = entry;
	cache->active_entries[array_layer] = entry;
	fill_native_state(entry, out_state);
	os_mutex_unlock(&cache->mutex);
	return XRT_SUCCESS;
}

xrt_result_t
comp_metal_foveation_cache_get_packed(
    struct comp_metal_foveation_cache *cache,
    const struct xrt_metal_foveation_view_layout *views,
    uint32_t view_count,
    uint32_t array_layer,
    struct xrt_metal_foveation_state *out_state)
{
	if (cache == NULL || cache->metal_device == NULL || views == NULL ||
	    out_state == NULL || view_count == 0 || view_count > XRT_MAX_VIEWS ||
	    array_layer >= cache->array_size) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	os_mutex_lock(&cache->mutex);
	const uint32_t revision = cache->revision;
	const struct xrt_foveation_state state = cache->state;
	if (!state.enabled) {
		fill_disabled_state(cache, out_state);
		os_mutex_unlock(&cache->mutex);
		return XRT_SUCCESS;
	}

	for (uint32_t i = 0; i < view_count; ++i) {
		const struct xrt_metal_foveation_view_layout *layout = &views[i];
		if (layout->view_index >= state.view_count ||
		    !state.views[layout->view_index].center_valid ||
		    layout->width == 0 || layout->height == 0 || layout->offset_x < 0 ||
		    layout->offset_y < 0 ||
		    (uint64_t)layout->offset_x + layout->width > cache->logical_width ||
		    (uint64_t)layout->offset_y + layout->height > cache->logical_height) {
			os_mutex_unlock(&cache->mutex);
			return XRT_ERROR_INVALID_ARGUMENT;
		}
	}

	for (struct comp_metal_foveation_cache_entry *entry =
	         (struct comp_metal_foveation_cache_entry *)cache->entries;
	     entry != NULL; entry = entry->next) {
		if (entry->revision == revision && entry->array_layer == array_layer &&
		    same_packed_layout(entry, views, view_count)) {
			cache->active_entries[array_layer] = entry;
			fill_native_state(entry, out_state);
			os_mutex_unlock(&cache->mutex);
			return XRT_SUCCESS;
		}
	}

	uint32_t zones_x[XRT_MAX_VIEWS] = {0};
	uint32_t zones_y[XRT_MAX_VIEWS] = {0};
	float scales_x[XRT_MAX_VIEWS] = {0};
	float scales_y[XRT_MAX_VIEWS] = {0};
	for (uint32_t i = 0; i < view_count; ++i) {
		const struct xrt_metal_foveation_view_layout *layout = &views[i];
		const struct xrt_foveation_view_state *view = &state.views[layout->view_index];
		const float local_u = clampf01(0.5f * (view->center.x + 1.0f));
		const float local_v = clampf01(0.5f * (1.0f - view->center.y));
		const float target_u =
		    ((float)layout->offset_x + local_u * (float)layout->width) /
		    (float)cache->logical_width;
		const float target_v =
		    ((float)layout->offset_y + local_v * (float)layout->height) /
		    (float)cache->logical_height;
		zones_x[i] = (uint32_t)fminf(
		    (float)(M_METAL_FOVEATION_ZONE_COUNT - 1),
		    floorf(clampf01(target_u) * (float)M_METAL_FOVEATION_ZONE_COUNT));
		zones_y[i] = (uint32_t)fminf(
		    (float)(M_METAL_FOVEATION_ZONE_COUNT - 1),
		    floorf(clampf01(target_v) * (float)M_METAL_FOVEATION_ZONE_COUNT));
		scales_x[i] = (float)layout->width / (float)cache->logical_width;
		scales_y[i] = (float)layout->height / (float)cache->logical_height;
	}

	const struct u_foveation_profile profile = profile_from_state(&state);
	struct comp_metal_foveation_cache_entry *entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		os_mutex_unlock(&cache->mutex);
		return XRT_ERROR_ALLOCATION;
	}
	if (!m_metal_foveation_map_build_for_zones(
	        cache->metal_device, cache->logical_width, cache->logical_height,
	        zones_x, zones_y, scales_x, scales_y, view_count, &profile,
	        &entry->map)) {
		free(entry);
		os_mutex_unlock(&cache->mutex);
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	entry->revision = revision;
	entry->array_layer = array_layer;
	entry->packed = true;
	entry->packed_view_count = view_count;
	memcpy(entry->packed_views, views, view_count * sizeof(*views));
	entry->next = (struct comp_metal_foveation_cache_entry *)cache->entries;
	cache->entries = entry;
	cache->active_entries[array_layer] = entry;
	fill_native_state(entry, out_state);
	os_mutex_unlock(&cache->mutex);
	return XRT_SUCCESS;
}

xrt_result_t
comp_metal_foveation_cache_get_active(
    struct comp_metal_foveation_cache *cache,
    uint32_t array_layer,
    struct xrt_metal_foveation_state *out_state)
{
	if (cache == NULL || cache->metal_device == NULL || out_state == NULL ||
	    array_layer >= cache->array_size) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	os_mutex_lock(&cache->mutex);
	if (!cache->state.enabled) {
		fill_disabled_state(cache, out_state);
		os_mutex_unlock(&cache->mutex);
		return XRT_SUCCESS;
	}
	struct comp_metal_foveation_cache_entry *entry =
	    (struct comp_metal_foveation_cache_entry *)cache->active_entries[array_layer];
	if (entry == NULL || entry->revision != cache->revision) {
		os_mutex_unlock(&cache->mutex);
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	fill_native_state(entry, out_state);
	os_mutex_unlock(&cache->mutex);
	return XRT_SUCCESS;
}

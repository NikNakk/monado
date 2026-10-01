// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import <Metal/Metal.h>

#include "comp_metal_foveation_cache.h"
#include "metal/m_metal_foveation.h"
#include "foveation/u_foveation.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(M_METAL_FOVEATION_ZONE_COUNT == XRT_METAL_FOVEATION_ZONE_COUNT,
               "Metal foveation sample counts must stay in sync");

struct comp_metal_foveation_cache_entry
{
	//! Owners: lookup list, layer selections and image bindings.
	uint32_t refs;
	uint32_t revision;
	uint32_t view_index;
	bool packed;
	uint32_t packed_view_count;
	struct xrt_metal_foveation_view_layout packed_views[XRT_MAX_VIEWS];
	struct m_metal_foveation_map map;
	//! Lookup-list link, only meaningful while the list holds a reference.
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
foveation_maps_equivalent(const struct xrt_foveation_state *a, const struct xrt_foveation_state *b)
{
	if (a->enabled != b->enabled) {
		return false;
	}
	if (!a->enabled) {
		return true;
	}

	if (a->center_rate != b->center_rate || a->middle_rate != b->middle_rate ||
	    a->peripheral_rate != b->peripheral_rate || a->center_half_extent != b->center_half_extent ||
	    a->middle_half_extent != b->middle_half_extent || a->view_count != b->view_count) {
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

/*
 *
 * Entry ownership. All functions below require cache->mutex to be held.
 *
 */

static struct comp_metal_foveation_cache_entry *
entry_ref(struct comp_metal_foveation_cache_entry *entry)
{
	if (entry != NULL) {
		entry->refs++;
	}
	return entry;
}

static void
entry_unref(struct comp_metal_foveation_cache *cache, struct comp_metal_foveation_cache_entry *entry)
{
	if (entry == NULL) {
		return;
	}
	assert(entry->refs > 0);
	if (--entry->refs > 0) {
		return;
	}
	m_metal_foveation_map_release(&entry->map);
	free(entry);
	assert(cache->live_entry_count > 0);
	cache->live_entry_count--;
}

//! Replace one owned slot, taking a reference on @p entry first.
static void
slot_assign(struct comp_metal_foveation_cache *cache, void **slot, struct comp_metal_foveation_cache_entry *entry)
{
	struct comp_metal_foveation_cache_entry *old = (struct comp_metal_foveation_cache_entry *)*slot;
	if (old == entry) {
		return;
	}
	*slot = entry_ref(entry);
	entry_unref(cache, old);
}

static void
clear_lookup_entries(struct comp_metal_foveation_cache *cache)
{
	struct comp_metal_foveation_cache_entry *entry = (struct comp_metal_foveation_cache_entry *)cache->entries;
	cache->entries = NULL;
	cache->entry_count = 0;
	while (entry != NULL) {
		struct comp_metal_foveation_cache_entry *next = entry->next;
		entry->next = NULL;
		entry_unref(cache, entry);
		entry = next;
	}
}

static void
clear_selections(struct comp_metal_foveation_cache *cache)
{
	for (uint32_t layer = 0; layer < cache->array_size; ++layer) {
		slot_assign(cache, &cache->selected[layer], NULL);
	}
}

static void
clear_image_bindings(struct comp_metal_foveation_cache *cache)
{
	const size_t count = (size_t)cache->image_count * cache->array_size;
	for (size_t i = 0; i < count; ++i) {
		slot_assign(cache, &cache->image_entries[i], NULL);
	}
}

//! Insert a freshly built entry (refs == 0) into the lookup list.
static void
lookup_insert(struct comp_metal_foveation_cache *cache, struct comp_metal_foveation_cache_entry *entry)
{
	entry->next = (struct comp_metal_foveation_cache_entry *)cache->entries;
	cache->entries = entry_ref(entry);
	cache->entry_count++;

	if (cache->entry_count <= COMP_METAL_FOVEATION_CACHE_MAX_LOOKUP_ENTRIES) {
		return;
	}

	// Drop the oldest lookup entry; other owners keep it alive if needed.
	struct comp_metal_foveation_cache_entry *prev = entry;
	while (prev->next != NULL && prev->next->next != NULL) {
		prev = prev->next;
	}
	struct comp_metal_foveation_cache_entry *oldest = prev->next;
	prev->next = NULL;
	cache->entry_count--;
	entry_unref(cache, oldest);
}

static struct comp_metal_foveation_cache_entry *
entry_alloc(struct comp_metal_foveation_cache *cache)
{
	struct comp_metal_foveation_cache_entry *entry = calloc(1, sizeof(*entry));
	if (entry != NULL) {
		cache->live_entry_count++;
	}
	return entry;
}

static void
entry_free_unbuilt(struct comp_metal_foveation_cache *cache, struct comp_metal_foveation_cache_entry *entry)
{
	free(entry);
	cache->live_entry_count--;
}

static void
fill_native_state(const struct comp_metal_foveation_cache_entry *entry, struct xrt_metal_foveation_state *out_state)
{
	*out_state = (struct xrt_metal_foveation_state){
	    .enabled = true,
	    .rasterization_rate_map = entry->map.rate_map,
	    .physical_width = (uint32_t)entry->map.physical_width,
	    .physical_height = (uint32_t)entry->map.physical_height,
	    .revision = entry->revision,
	    .sample_count = entry->map.sample_count,
	};
	memcpy(out_state->horizontal_rates, entry->map.horizontal_rates, sizeof(out_state->horizontal_rates));
	memcpy(out_state->vertical_rates, entry->map.vertical_rates, sizeof(out_state->vertical_rates));
	out_state->compositor_map.enabled = 1;
	out_state->compositor_map.boundary_count = M_METAL_FOVEATION_BOUNDARY_COUNT;
	memcpy(out_state->compositor_map.x, entry->map.x, sizeof(out_state->compositor_map.x));
	memcpy(out_state->compositor_map.y, entry->map.y, sizeof(out_state->compositor_map.y));
}

static void
fill_disabled_state(const struct comp_metal_foveation_cache *cache, struct xrt_metal_foveation_state *out_state)
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

//! Select @p entry (may be NULL) for @p array_layer and report it.
static void
select_entry(struct comp_metal_foveation_cache *cache,
             uint32_t array_layer,
             struct comp_metal_foveation_cache_entry *entry,
             struct xrt_metal_foveation_state *out_state)
{
	slot_assign(cache, &cache->selected[array_layer], entry);
	if (out_state == NULL) {
		return;
	}
	if (entry != NULL) {
		fill_native_state(entry, out_state);
	} else {
		fill_disabled_state(cache, out_state);
	}
}

static xrt_result_t
select_failure(struct comp_metal_foveation_cache *cache, uint32_t array_layer, xrt_result_t xret)
{
	// A failed query must not leave an older map attached to future images.
	slot_assign(cache, &cache->selected[array_layer], NULL);
	return xret;
}


/*
 *
 * 'Exported' functions.
 *
 */

bool
comp_metal_foveation_cache_init(struct comp_metal_foveation_cache *cache,
                                void *metal_device,
                                uint32_t logical_width,
                                uint32_t logical_height,
                                uint32_t array_size,
                                uint32_t image_count)
{
	if (cache == NULL || metal_device == NULL || logical_width == 0 || logical_height == 0 || array_size == 0 ||
	    image_count == 0 || image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return false;
	}

	memset(cache, 0, sizeof(*cache));
	cache->logical_width = logical_width;
	cache->logical_height = logical_height;
	cache->array_size = array_size;
	cache->image_count = image_count;
	cache->revision = 1;
	cache->selected = calloc(array_size, sizeof(void *));
	cache->image_entries = calloc((size_t)image_count * array_size, sizeof(void *));
	if (cache->selected == NULL || cache->image_entries == NULL || os_mutex_init(&cache->mutex) != 0) {
		free(cache->selected);
		free(cache->image_entries);
		memset(cache, 0, sizeof(*cache));
		return false;
	}
	cache->metal_device = (void *)[(__bridge id<MTLDevice>)metal_device retain];

	return true;
}

void
comp_metal_foveation_cache_destroy(struct comp_metal_foveation_cache *cache)
{
	if (cache == NULL || cache->metal_device == NULL) {
		return;
	}

	os_mutex_lock(&cache->mutex);
	clear_image_bindings(cache);
	clear_selections(cache);
	clear_lookup_entries(cache);
	assert(cache->live_entry_count == 0);
	free(cache->image_entries);
	free(cache->selected);
	cache->image_entries = NULL;
	cache->selected = NULL;
	void *device = cache->metal_device;
	cache->metal_device = NULL;
	os_mutex_unlock(&cache->mutex);
	os_mutex_destroy(&cache->mutex);

	[(__bridge id<MTLDevice>)device release];
	memset(cache, 0, sizeof(*cache));
}

xrt_result_t
comp_metal_foveation_cache_set(struct comp_metal_foveation_cache *cache, const struct xrt_foveation_state *state)
{
	if (cache == NULL || cache->metal_device == NULL || state == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	os_mutex_lock(&cache->mutex);
	const bool same_map = foveation_maps_equivalent(&cache->state, state);
	cache->state = *state;
	if (!same_map) {
		/*
		 * Only the lookup list is revision-scoped. Selections keep the map
		 * last handed to the application alive until it queries again, and
		 * image bindings keep what each released image was rendered with.
		 */
		clear_lookup_entries(cache);
		cache->revision++;
		if (cache->revision == 0) {
			cache->revision = 1;
		}
	}
	if (!state->enabled) {
		clear_selections(cache);
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
		select_entry(cache, array_layer, NULL, out_state);
		os_mutex_unlock(&cache->mutex);
		return XRT_SUCCESS;
	}
	if (state.view_count == 0 || view_index >= state.view_count || !state.views[view_index].center_valid) {
		xrt_result_t xret = select_failure(cache, array_layer, XRT_ERROR_NOT_IMPLEMENTED);
		os_mutex_unlock(&cache->mutex);
		return xret;
	}

	for (struct comp_metal_foveation_cache_entry *entry = (struct comp_metal_foveation_cache_entry *)cache->entries;
	     entry != NULL; entry = entry->next) {
		if (!entry->packed && entry->view_index == view_index) {
			select_entry(cache, array_layer, entry, out_state);
			os_mutex_unlock(&cache->mutex);
			return XRT_SUCCESS;
		}
	}

	int zone_x = -1, zone_y = -1;
	center_to_zone(&state.views[view_index], &zone_x, &zone_y);
	if (zone_x < 0 || zone_y < 0) {
		xrt_result_t xret = select_failure(cache, array_layer, XRT_ERROR_NOT_IMPLEMENTED);
		os_mutex_unlock(&cache->mutex);
		return xret;
	}
	const struct u_foveation_profile profile = profile_from_state(&state);
	struct comp_metal_foveation_cache_entry *entry = entry_alloc(cache);
	if (entry == NULL) {
		xrt_result_t xret = select_failure(cache, array_layer, XRT_ERROR_ALLOCATION);
		os_mutex_unlock(&cache->mutex);
		return xret;
	}
	if (!m_metal_foveation_map_build(cache->metal_device, cache->logical_width, cache->logical_height, zone_x,
	                                 zone_y, &profile, &entry->map)) {
		entry_free_unbuilt(cache, entry);
		xrt_result_t xret = select_failure(cache, array_layer, XRT_ERROR_NOT_IMPLEMENTED);
		os_mutex_unlock(&cache->mutex);
		return xret;
	}
	entry->revision = revision;
	entry->view_index = view_index;
	lookup_insert(cache, entry);
	select_entry(cache, array_layer, entry, out_state);
	os_mutex_unlock(&cache->mutex);
	return XRT_SUCCESS;
}

xrt_result_t
comp_metal_foveation_cache_get_packed(struct comp_metal_foveation_cache *cache,
                                      const struct xrt_metal_foveation_view_layout *views,
                                      uint32_t view_count,
                                      uint32_t array_layer,
                                      struct xrt_metal_foveation_state *out_state)
{
	if (cache == NULL || cache->metal_device == NULL || views == NULL || out_state == NULL || view_count == 0 ||
	    view_count > XRT_MAX_VIEWS || array_layer >= cache->array_size) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	os_mutex_lock(&cache->mutex);
	const uint32_t revision = cache->revision;
	const struct xrt_foveation_state state = cache->state;
	if (!state.enabled) {
		select_entry(cache, array_layer, NULL, out_state);
		os_mutex_unlock(&cache->mutex);
		return XRT_SUCCESS;
	}

	for (uint32_t i = 0; i < view_count; ++i) {
		const struct xrt_metal_foveation_view_layout *layout = &views[i];
		if (layout->view_index >= state.view_count || !state.views[layout->view_index].center_valid ||
		    layout->width == 0 || layout->height == 0 || layout->offset_x < 0 || layout->offset_y < 0 ||
		    (layout->flags & ~XRT_METAL_FOVEATION_VIEW_VERTICAL_FLIP) != 0 ||
		    (uint64_t)layout->offset_x + layout->width > cache->logical_width ||
		    (uint64_t)layout->offset_y + layout->height > cache->logical_height) {
			xrt_result_t xret = select_failure(cache, array_layer, XRT_ERROR_INVALID_ARGUMENT);
			os_mutex_unlock(&cache->mutex);
			return xret;
		}
	}

	for (struct comp_metal_foveation_cache_entry *entry = (struct comp_metal_foveation_cache_entry *)cache->entries;
	     entry != NULL; entry = entry->next) {
		if (same_packed_layout(entry, views, view_count)) {
			select_entry(cache, array_layer, entry, out_state);
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
		float local_v = clampf01(0.5f * (1.0f - view->center.y));
		if ((layout->flags & XRT_METAL_FOVEATION_VIEW_VERTICAL_FLIP) != 0) {
			// The view's top is stored at the bottom of its rectangle.
			local_v = 1.0f - local_v;
		}
		const float target_u =
		    ((float)layout->offset_x + local_u * (float)layout->width) / (float)cache->logical_width;
		const float target_v =
		    ((float)layout->offset_y + local_v * (float)layout->height) / (float)cache->logical_height;
		zones_x[i] = (uint32_t)fminf((float)(M_METAL_FOVEATION_ZONE_COUNT - 1),
		                             floorf(clampf01(target_u) * (float)M_METAL_FOVEATION_ZONE_COUNT));
		zones_y[i] = (uint32_t)fminf((float)(M_METAL_FOVEATION_ZONE_COUNT - 1),
		                             floorf(clampf01(target_v) * (float)M_METAL_FOVEATION_ZONE_COUNT));
		scales_x[i] = (float)layout->width / (float)cache->logical_width;
		scales_y[i] = (float)layout->height / (float)cache->logical_height;
	}

	const struct u_foveation_profile profile = profile_from_state(&state);
	struct comp_metal_foveation_cache_entry *entry = entry_alloc(cache);
	if (entry == NULL) {
		xrt_result_t xret = select_failure(cache, array_layer, XRT_ERROR_ALLOCATION);
		os_mutex_unlock(&cache->mutex);
		return xret;
	}
	if (!m_metal_foveation_map_build_for_zones(cache->metal_device, cache->logical_width, cache->logical_height,
	                                           zones_x, zones_y, scales_x, scales_y, view_count, &profile,
	                                           &entry->map)) {
		entry_free_unbuilt(cache, entry);
		xrt_result_t xret = select_failure(cache, array_layer, XRT_ERROR_NOT_IMPLEMENTED);
		os_mutex_unlock(&cache->mutex);
		return xret;
	}
	entry->revision = revision;
	entry->packed = true;
	entry->packed_view_count = view_count;
	memcpy(entry->packed_views, views, view_count * sizeof(*views));
	lookup_insert(cache, entry);
	select_entry(cache, array_layer, entry, out_state);
	os_mutex_unlock(&cache->mutex);
	return XRT_SUCCESS;
}

xrt_result_t
comp_metal_foveation_cache_bind_released_image(struct comp_metal_foveation_cache *cache, uint32_t image_index)
{
	if (cache == NULL || cache->metal_device == NULL || image_index >= cache->image_count) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	os_mutex_lock(&cache->mutex);
	void **bindings = &cache->image_entries[(size_t)image_index * cache->array_size];
	for (uint32_t layer = 0; layer < cache->array_size; ++layer) {
		slot_assign(cache, &bindings[layer], (struct comp_metal_foveation_cache_entry *)cache->selected[layer]);
	}
	os_mutex_unlock(&cache->mutex);

	return XRT_SUCCESS;
}

xrt_result_t
comp_metal_foveation_cache_get_image(struct comp_metal_foveation_cache *cache,
                                     uint32_t image_index,
                                     uint32_t array_layer,
                                     struct xrt_metal_foveation_state *out_state)
{
	if (cache == NULL || cache->metal_device == NULL || out_state == NULL || image_index >= cache->image_count ||
	    array_layer >= cache->array_size) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	os_mutex_lock(&cache->mutex);
	const struct comp_metal_foveation_cache_entry *entry =
	    (const struct comp_metal_foveation_cache_entry *)
	        cache->image_entries[(size_t)image_index * cache->array_size + array_layer];
	if (entry != NULL) {
		fill_native_state(entry, out_state);
	} else {
		fill_disabled_state(cache, out_state);
	}
	os_mutex_unlock(&cache->mutex);

	return XRT_SUCCESS;
}

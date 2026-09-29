// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "xrt/xrt_compositor.h"
#include "os/os_threading.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Maximum number of distinct maps kept in the lookup list for the current
 * foveation revision. Entries still selected by the application or bound to a
 * released image outlive their lookup-list slot through their own reference.
 */
#define COMP_METAL_FOVEATION_CACHE_MAX_LOOKUP_ENTRIES 8

/*!
 * Per-swapchain Metal foveation map cache.
 *
 * Entries are immutable once built and reference counted. Three kinds of
 * owner hold references:
 *
 * - the lookup list, which only contains entries for the current revision;
 * - the per-array-layer selection, which is exactly the map most recently
 *   returned to the application for that layer;
 * - the per-(image, array layer) binding, which is the selection that was in
 *   effect when that image was successfully released.
 *
 * The compositor only ever consumes image bindings, so a submitted image is
 * always paired with the map it was rendered with, even if the foveation state
 * changed after the image was released and the image is re-submitted without
 * being rendered again.
 */
struct comp_metal_foveation_cache
{
	void *metal_device;
	uint32_t logical_width;
	uint32_t logical_height;
	uint32_t array_size;
	uint32_t image_count;
	uint32_t revision;
	struct xrt_foveation_state state;
	struct os_mutex mutex;

	//! Lookup list for the current revision, newest first.
	void *entries;
	uint32_t entry_count;

	//! [array_size] entries most recently returned to the application.
	void **selected;

	//! [image_count * array_size] entries bound on image release.
	void **image_entries;

	//! Number of live entries, for leak checks in tests.
	uint32_t live_entry_count;
};

bool
comp_metal_foveation_cache_init(struct comp_metal_foveation_cache *cache,
                                void *metal_device,
                                uint32_t logical_width,
                                uint32_t logical_height,
                                uint32_t array_size,
                                uint32_t image_count);

void
comp_metal_foveation_cache_destroy(struct comp_metal_foveation_cache *cache);

/*!
 * Apply a new resolved policy. A changed rasterization pattern starts a new
 * revision. Disabling foveation also clears every layer selection, so images
 * released afterwards are bound to no map until the application queries a map
 * again. Existing image bindings are never modified here.
 */
xrt_result_t
comp_metal_foveation_cache_set(struct comp_metal_foveation_cache *cache,
                               const struct xrt_foveation_state *state);

/*!
 * Return the map for one view and select it for @p array_layer. A disabled or
 * failed result clears the layer selection.
 */
xrt_result_t
comp_metal_foveation_cache_get(struct comp_metal_foveation_cache *cache,
                               uint32_t view_index,
                               uint32_t array_layer,
                               struct xrt_metal_foveation_state *out_state);

/*!
 * Return one map covering several packed views and select it for
 * @p array_layer. A disabled or failed result clears the layer selection.
 */
xrt_result_t
comp_metal_foveation_cache_get_packed(
    struct comp_metal_foveation_cache *cache,
    const struct xrt_metal_foveation_view_layout *views,
    uint32_t view_count,
    uint32_t array_layer,
    struct xrt_metal_foveation_state *out_state);

/*!
 * Bind the current selection of every array layer to @p image_index. Call
 * after the image has been successfully released to the compositor.
 */
xrt_result_t
comp_metal_foveation_cache_bind_released_image(struct comp_metal_foveation_cache *cache,
                                               uint32_t image_index);

/*!
 * Return the map bound to a released image and array layer. An image that was
 * released without a selected map reports a disabled state.
 */
xrt_result_t
comp_metal_foveation_cache_get_image(struct comp_metal_foveation_cache *cache,
                                     uint32_t image_index,
                                     uint32_t array_layer,
                                     struct xrt_metal_foveation_state *out_state);

#ifdef __cplusplus
}
#endif

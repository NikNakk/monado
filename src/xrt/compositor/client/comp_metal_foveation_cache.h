// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "xrt/xrt_compositor.h"
#include "os/os_threading.h"

#ifdef __cplusplus
extern "C" {
#endif

struct comp_metal_foveation_cache
{
	void *metal_device;
	uint32_t logical_width;
	uint32_t logical_height;
	uint32_t array_size;
	uint32_t revision;
	struct xrt_foveation_state state;
	struct os_mutex mutex;
	void *entries;
};

bool
comp_metal_foveation_cache_init(struct comp_metal_foveation_cache *cache,
                                void *metal_device,
                                uint32_t logical_width,
                                uint32_t logical_height,
                                uint32_t array_size);

void
comp_metal_foveation_cache_destroy(struct comp_metal_foveation_cache *cache);

xrt_result_t
comp_metal_foveation_cache_set(struct comp_metal_foveation_cache *cache,
                               const struct xrt_foveation_state *state);

xrt_result_t
comp_metal_foveation_cache_get(struct comp_metal_foveation_cache *cache,
                               uint32_t view_index,
                               uint32_t array_layer,
                               struct xrt_metal_foveation_state *out_state);

#ifdef __cplusplus
}
#endif

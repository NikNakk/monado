// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Lightweight thread-local handoff API for Metal-owned swapchains.
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_compositor.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start a synchronous Metal allocation request on the current thread. The
 * caller owns the MTLTexture objects and must keep them alive until end().
 */
bool
comp_metal_swapchain_import_begin(const struct xrt_swapchain_create_info *info,
                                  uint32_t image_count,
                                  void *const *metal_textures);

/* True after the normal compositor allocator has consumed the active request. */
bool
comp_metal_swapchain_import_was_consumed(void);

/* Clear the current thread's request after the native create call returns. */
void
comp_metal_swapchain_import_end(void);

#ifdef __cplusplus
}
#endif

// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Thread-local handoff for direct Metal-owned swapchain allocation.
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_handles.h"
#include "vk/vk_image_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start a synchronous in-process Metal allocation request. The caller owns the
 * MTLTexture objects and must keep them alive until end() is called.
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

/*
 * macOS-only wrappers used by comp_swapchain.c. They behave exactly like the
 * normal vk_image_allocator helpers unless a matching Metal request is active.
 */
VkResult
comp_metal_swapchain_import_allocate_or_default(struct vk_bundle *vk,
                                                const struct xrt_swapchain_create_info *info,
                                                uint32_t image_count,
                                                struct vk_image_collection *out_vkic);

VkResult
comp_metal_swapchain_import_get_handles_or_default(struct vk_bundle *vk,
                                                   struct vk_image_collection *vkic,
                                                   uint32_t max_handles,
                                                   xrt_graphics_buffer_handle_t *out_handles);

#ifdef __cplusplus
}
#endif

// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Thread-local allocator override for direct Metal-owned swapchains.
 * @ingroup comp_util
 */

#pragma once

#include "util/comp_metal_swapchain_handoff.h"
#include "xrt/xrt_handles.h"
#include "vk/vk_image_allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

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

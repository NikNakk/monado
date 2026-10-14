// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "vk/vk_helpers.h"
#include "xrt/xrt_compositor.h"
#ifdef __cplusplus
extern "C" {
#endif
struct client_vk_metal_texture_context;
bool
client_vk_metal_texture_enabled(void);
bool
client_vk_metal_texture_required(const struct xrt_swapchain_create_info *info);
struct client_vk_metal_texture_context *
client_vk_metal_texture_context_create(struct xrt_compositor_native *xcn, struct vk_bundle *vk);
void
client_vk_metal_texture_context_destroy(struct client_vk_metal_texture_context *context);
xrt_result_t
client_vk_metal_texture_swapchain_create(struct client_vk_metal_texture_context *context,
                                         const struct xrt_swapchain_create_info *info,
                                         struct xrt_swapchain_metal **out_swapchain,
                                         struct xrt_swapchain_native **out_native);
VkResult
client_vk_metal_texture_import(struct vk_bundle *vk,
                               const struct xrt_swapchain_create_info *info,
                               void *texture,
                               VkImage *out_image);
#ifdef __cplusplus
}
#endif

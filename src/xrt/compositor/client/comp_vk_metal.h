// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "vk/vk_helpers.h"
#include "xrt/xrt_compositor.h"
#ifdef __cplusplus
extern "C" {
#endif
struct client_vk_metal_context;
bool
client_vk_metal_enabled(void);
const char *
client_vk_metal_device_extensions(void);
bool
client_vk_metal_matches(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance, VkPhysicalDevice physical);
struct client_vk_metal_context *
client_vk_metal_create(struct xrt_compositor_native *xcn,
                       struct vk_bundle *vk,
                       struct xrt_compositor_metal **out_metal,
                       VkSemaphore *out_semaphore,
                       struct xrt_compositor_semaphore **out_native_semaphore);
VkResult
client_vk_metal_import_image(struct client_vk_metal_context *context,
                             const struct xrt_swapchain_create_info *info,
                             void *texture,
                             VkImage *out_image);
int64_t
client_vk_metal_format(int64_t vk_format);
void
client_vk_metal_destroy(struct client_vk_metal_context *context);
#ifdef __cplusplus
}
#endif

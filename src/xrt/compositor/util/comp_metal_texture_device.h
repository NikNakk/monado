// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Helpers for making shared Metal textures compatible with a Vulkan device.
 * @ingroup comp_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct vk_bundle;
struct xrt_swapchain_create_info;

/*
 * Return a Metal texture that belongs to the exact MTLDevice underlying @p vk.
 *
 * If @p source_texture already belongs to that device it is returned unchanged
 * and out_needs_release is false. Otherwise it is re-opened through an
 * MTLSharedTextureHandle on the Vulkan device and out_needs_release is true.
 */
bool
comp_metal_texture_prepare_for_vk_device(struct vk_bundle *vk,
                                         void *source_texture,
                                         void **out_texture,
                                         bool *out_needs_release);

bool
comp_metal_texture_create_from_iosurface_id_for_vk_device(struct vk_bundle *vk,
                                                          const struct xrt_swapchain_create_info *info,
                                                          uint32_t iosurface_id,
                                                          void **out_texture);

void
comp_metal_texture_release(void *texture);

/* Release a texture returned with out_needs_release=true. */
void
comp_metal_texture_finish_for_vk_device(void *texture, bool needs_release);

#ifdef __cplusplus
}
#endif

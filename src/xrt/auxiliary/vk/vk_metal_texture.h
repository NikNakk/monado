// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Spike: shareable Metal textures for Vulkan images on macOS.
 * @ingroup aux_vk
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Create a texture with the same description as @p metal_texture, on the same
 * Metal device, that can be shared with another process.
 *
 * Only a texture created as shareable can produce an MTLSharedTextureHandle;
 * a texture MoltenVK allocates for a VkImage cannot.
 *
 * @return A retained id<MTLTexture>, or NULL. Release with CFRelease.
 */
void *
vk_metal_texture_create_shareable_like(void *metal_texture);

/*! Can this id<MTLTexture> be shared with another process? */
bool
vk_metal_texture_is_shareable(void *metal_texture);

#ifdef __cplusplus
}
#endif

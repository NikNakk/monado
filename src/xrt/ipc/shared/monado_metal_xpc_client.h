// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal stable ABI for non-Monado processes that need to participate in the
// Metal XPC texture handoff. Metal/XPC objects stay private to the implementation.

/*
 * Publish one or more shared Metal textures under a token owned by this
 * process. The same process must use the token when creating the Monado
 * swapchain.
 */
__attribute__((visibility("default"))) int
monado_metal_xpc_publish_textures(void *const *metal_textures, uint32_t image_count, uint64_t *out_token);

__attribute__((visibility("default"))) int
monado_metal_xpc_publish_claimable_texture(void *metal_texture, uint64_t *out_token);

/*
 * Publish an existing MTLSharedEvent under a token owned by this process.
 * The same process must import the token into its Monado compositor session.
 */
__attribute__((visibility("default"))) int
monado_metal_xpc_publish_shared_event(void *metal_shared_event, uint64_t *out_token);

__attribute__((visibility("default"))) int
monado_metal_xpc_take_texture(uint64_t token, void **out_metal_texture);

__attribute__((visibility("default"))) int
monado_metal_xpc_take_texture_on_device(uint64_t token, void *metal_device, void **out_metal_texture);

__attribute__((visibility("default"))) void
monado_metal_xpc_release_texture(void *metal_texture);

#ifdef __cplusplus
}
#endif

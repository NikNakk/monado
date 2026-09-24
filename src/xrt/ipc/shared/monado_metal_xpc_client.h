// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Minimal stable ABI for non-Monado processes that need to participate in the
// Metal XPC texture handoff. Metal/XPC objects stay private to the implementation.
__attribute__((visibility("default"))) int
monado_metal_xpc_publish_claimable_texture(void *metal_texture,
                                           uint64_t *out_token);

__attribute__((visibility("default"))) int
monado_metal_xpc_take_texture(uint64_t token, void **out_metal_texture);

__attribute__((visibility("default"))) void
monado_metal_xpc_release_texture(void *metal_texture);

#ifdef __cplusplus
}
#endif

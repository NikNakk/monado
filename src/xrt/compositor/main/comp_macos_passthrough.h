// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#import <Metal/Metal.h>
#include <stdbool.h>
struct comp_compositor;
struct comp_macos_passthrough;
struct comp_macos_passthrough *
comp_macos_passthrough_create(struct comp_compositor *c, id<MTLDevice> device, MTLPixelFormat format, bool is_service);
void
comp_macos_passthrough_stop(struct comp_macos_passthrough *pt);
void
comp_macos_passthrough_destroy(struct comp_macos_passthrough **pt);
//! Poll camera availability before taking ownership of a WSI image.
bool
comp_macos_passthrough_ready(struct comp_macos_passthrough *pt);
bool
comp_macos_passthrough_encode(struct comp_macos_passthrough *pt,
                              id<MTLCommandBuffer> commands,
                              id<MTLTexture> target,
                              id<MTLTexture> app,
                              bool has_application_layers);

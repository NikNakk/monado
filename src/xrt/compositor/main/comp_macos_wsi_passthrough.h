// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "main/comp_target.h"
#import <Metal/Metal.h>
struct comp_macos_wsi_passthrough;
struct comp_macos_wsi_passthrough *
comp_macos_wsi_passthrough_create(struct comp_target *ct,
                                  struct vk_bundle_queue *queue,
                                  id<MTLDevice> device,
                                  bool is_service);
void
comp_macos_wsi_passthrough_destroy(struct comp_macos_wsi_passthrough **pt);
VkResult
//! On recoverable errors, restore PRESENT_SRC and render_complete before returning.
//! False present_ready means synchronization is uncertain: retire the target.
comp_macos_wsi_passthrough_compose(struct comp_macos_wsi_passthrough *pt,
                                   uint32_t index,
                                   bool has_app,
                                   bool *present_ready);

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "main/comp_target.h"
struct comp_vulkan_passthrough;
//! Reads the target's private images (ct->images, finished in ct->final_layout)
//! and renders into the separate presentation views, which finish in
//! destination_layout. The views must outlive this object.
struct comp_vulkan_passthrough *
comp_vulkan_passthrough_create(struct comp_target *ct,
                               struct vk_bundle_queue *queue,
                               bool is_service,
                               const VkImageView *destinations,
                               VkImageLayout destination_layout);
//! False retains pending GPU resources and the caller's target images.
bool
comp_vulkan_passthrough_destroy(struct comp_vulkan_passthrough **pt);
//! VK_SUCCESS means the presentation image was written; otherwise, while
//! *present_ready stays true, the caller must write it another way.
VkResult
comp_vulkan_passthrough_compose(struct comp_vulkan_passthrough *pt, uint32_t index, bool has_app, bool *present_ready);

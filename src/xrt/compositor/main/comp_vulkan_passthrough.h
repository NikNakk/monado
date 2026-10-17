// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "main/comp_target.h"
struct comp_vulkan_passthrough;
struct comp_vulkan_passthrough *
comp_vulkan_passthrough_create(struct comp_target *ct, struct vk_bundle_queue *queue, bool is_service);
//! False retains pending GPU resources and the caller's target images.
bool
comp_vulkan_passthrough_destroy(struct comp_vulkan_passthrough **pt);
VkResult
comp_vulkan_passthrough_compose(struct comp_vulkan_passthrough *pt, uint32_t index, bool has_app, bool *present_ready);

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

struct render_output;

//! Private render images followed by a GPU-ordered full-screen fragment pass
//! into caller-owned destination views. Destinations need only COLOR_ATTACHMENT
//! usage, with matching format and extent, and must outlive this object. No
//! shader writes, sampling or transfers touch the destination images. Calls are
//! serialized by the caller; rendering and output passes use the same queue.
//! vert/frag are the shared full-screen and output shaders.
struct render_output *
render_output_create(struct vk_bundle *vk,
                     struct vk_bundle_queue *queue,
                     VkShaderModule vert,
                     VkShaderModule frag,
                     VkFormat format,
                     VkExtent2D extent,
                     VkImageUsageFlags render_usage,
                     const VkImageView *destinations,
                     VkImageLayout destination_layout,
                     uint32_t count);

//! Borrowed images/views. The renderer must finish each image in GENERAL layout.
VkImage
render_output_image(struct render_output *output, uint32_t index);

VkImageView
render_output_view(struct render_output *output, uint32_t index);

//! Transfer the entire private image, alpha included, into the destination,
//! which finishes in destination_layout. Previous destination contents are
//! discarded. Consumes and re-signals the caller's binary completion semaphore
//! in one GPU batch. Slot reuse has a bounded wait; there is no explicit
//! current-frame GPU wait. On failure the caller must stop using this target.
VkResult
render_output_present(struct render_output *output, uint32_t index, VkSemaphore complete);

//! Bounded drain; retains the object and all resources if work is still pending.
VkResult
render_output_destroy(struct render_output **output);

#ifdef __cplusplus
}
#endif

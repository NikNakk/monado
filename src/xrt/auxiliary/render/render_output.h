// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

struct render_output;

//! Private render images followed by a GPU-ordered copy to caller-owned images.
//! Destination images require TRANSFER_DST usage, matching format and extent,
//! and must outlive this object. No shader writes touch the destination images.
//! Calls are serialized by the caller; rendering and copies use the same queue.
struct render_output *
render_output_create(struct vk_bundle *vk,
                     struct vk_bundle_queue *queue,
                     VkFormat format,
                     VkExtent2D extent,
                     VkImageUsageFlags render_usage,
                     const VkImage *destinations,
                     uint32_t count);

//! Borrowed images/views. The renderer must finish each image in GENERAL layout.
VkImage
render_output_image(struct render_output *output, uint32_t index);

VkImageView
render_output_view(struct render_output *output, uint32_t index);

//! Copy the entire private image, restoring it to GENERAL. Destination contents
//! are discarded, then copied and transitioned to destination_layout. Consumes
//! and re-signals the caller's binary completion semaphore in one GPU batch.
//! Slot reuse has a bounded wait; there is no explicit current-frame GPU wait.
//! On failure the caller must stop using/presenting this target.
VkResult
render_output_copy(struct render_output *output,
                   uint32_t index,
                   VkSemaphore complete,
                   VkImageLayout destination_layout);

//! Bounded drain; retains the object and all resources if work is still pending.
VkResult
render_output_destroy(struct render_output **output);

#ifdef __cplusplus
}
#endif

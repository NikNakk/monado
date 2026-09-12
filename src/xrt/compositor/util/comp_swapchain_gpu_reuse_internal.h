// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Internal hooks for compositor-to-client swapchain GPU reuse tracking.
 * @ingroup comp_util
 */

#pragma once

#include "util/comp_layer_accum.h"
#include "vk/vk_cmd.h"
#include "vk/vk_submit_helpers.h"

#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native comp_layer_accum bridge claims. */
void
comp_swapchain_gpu_reuse_native_accum_begin(struct comp_layer_accum *cla);

xrt_result_t
comp_swapchain_gpu_reuse_native_accum_claim_layer(struct comp_layer_accum *cla, const struct comp_layer *layer);

void
comp_swapchain_gpu_reuse_native_accum_release(struct comp_layer_accum *cla);

/* Scope the renderer translation unit's submit interception to one comp_renderer_draw(). */
void
comp_swapchain_gpu_reuse_renderer_enter(struct comp_layer_accum *cla);

void
comp_swapchain_gpu_reuse_renderer_leave(struct comp_layer_accum *cla);

/*
 * Source-local replacements used only while compiling comp_renderer.c. The
 * prepare hook arms tracking for the exact VkSubmitInfo built by
 * renderer_submit_queue(); the submit hook only augments/finalizes that armed
 * submission. Unrelated inline Vulkan submissions pass through unchanged.
 */
void
comp_swapchain_gpu_reuse_submit_info_builder_prepare(struct vk_submit_info_builder *builder,
                                                     const struct vk_semaphore_list_wait *wait_semaphores,
                                                     const VkCommandBuffer *command_buffers,
                                                     uint32_t command_buffer_count,
                                                     const struct vk_semaphore_list_signal *signal_semaphores,
                                                     const void *next);

VkResult
comp_swapchain_gpu_reuse_vk_cmd_submit_locked(struct vk_bundle *vk,
                                              struct vk_bundle_queue *queue,
                                              uint32_t count,
                                              const VkSubmitInfo *infos,
                                              VkFence fence);

#ifdef __cplusplus
}
#endif

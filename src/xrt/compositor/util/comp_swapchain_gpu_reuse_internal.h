// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Internal hooks for compositor-to-client swapchain GPU reuse tracking.
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_config_os.h"
#include "util/comp_layer_accum.h"
#include "vk/vk_cmd.h"
#include "vk/vk_submit_helpers.h"

#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX

/* Native comp_layer_accum bridge claims. */
void
comp_swapchain_gpu_reuse_native_accum_begin(struct comp_layer_accum *cla);

xrt_result_t
comp_swapchain_gpu_reuse_native_accum_claim_layer(struct comp_layer_accum *cla, const struct comp_layer *layer);

void
comp_swapchain_gpu_reuse_native_accum_release(struct comp_layer_accum *cla);

/* Scope the renderer's submit tracking to one comp_renderer_draw(). */
void
comp_swapchain_gpu_reuse_renderer_enter(struct comp_layer_accum *cla);

void
comp_swapchain_gpu_reuse_renderer_leave(struct comp_layer_accum *cla);

/*
 * Used by renderer_submit_queue() in place of vk_submit_info_builder_prepare()
 * and vk_cmd_submit_locked(). The prepare hook arms tracking for the exact
 * VkSubmitInfo it builds; the submit hook only augments/finalizes that armed
 * submission, and passes anything else straight through.
 */
void
comp_swapchain_gpu_reuse_submit_info_builder_prepare(struct vk_submit_info_builder *builder,
                                                     const struct vk_semaphore_list_wait *wait_semaphores,
                                                     const VkCommandBuffer *command_buffers,
                                                     uint32_t command_buffer_count,
                                                     const struct vk_semaphore_list_signal *signal_semaphores,
                                                     const void *next);

VkResult
comp_swapchain_gpu_reuse_vk_cmd_submit_locked(
    struct vk_bundle *vk, struct vk_bundle_queue *queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence);

#else

// Tracking is only used by the macOS service; the layer accumulator has nothing to claim elsewhere.
static inline void
comp_swapchain_gpu_reuse_native_accum_begin(struct comp_layer_accum *cla)
{
	(void)cla;
}

static inline xrt_result_t
comp_swapchain_gpu_reuse_native_accum_claim_layer(struct comp_layer_accum *cla, const struct comp_layer *layer)
{
	(void)cla;
	(void)layer;
	return XRT_SUCCESS;
}

static inline void
comp_swapchain_gpu_reuse_native_accum_release(struct comp_layer_accum *cla)
{
	(void)cla;
}

static inline void
comp_swapchain_gpu_reuse_renderer_enter(struct comp_layer_accum *cla)
{
	(void)cla;
}

static inline void
comp_swapchain_gpu_reuse_renderer_leave(struct comp_layer_accum *cla)
{
	(void)cla;
}

static inline void
comp_swapchain_gpu_reuse_submit_info_builder_prepare(struct vk_submit_info_builder *builder,
                                                     const struct vk_semaphore_list_wait *wait_semaphores,
                                                     const VkCommandBuffer *command_buffers,
                                                     uint32_t command_buffer_count,
                                                     const struct vk_semaphore_list_signal *signal_semaphores,
                                                     const void *next)
{
	vk_submit_info_builder_prepare(builder, wait_semaphores, command_buffers, command_buffer_count,
	                               signal_semaphores, next);
}

static inline VkResult
comp_swapchain_gpu_reuse_vk_cmd_submit_locked(
    struct vk_bundle *vk, struct vk_bundle_queue *queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence)
{
	return vk_cmd_submit_locked(vk, queue, count, infos, fence);
}

#endif

#ifdef __cplusplus
}
#endif

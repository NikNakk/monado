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

#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Native comp_layer_accum bridge claims.
 *
 * The multi compositor keeps a claim while a layer sits in one of its slots.
 * When that layer is copied into the native compositor, these helpers take an
 * overlapping claim so slot retirement can never expose the image before the
 * renderer has published the actual GPU-consumer submission value.
 */
void
comp_swapchain_gpu_reuse_native_accum_begin(struct comp_layer_accum *cla);

xrt_result_t
comp_swapchain_gpu_reuse_native_accum_claim_layer(struct comp_layer_accum *cla, const struct comp_layer *layer);

void
comp_swapchain_gpu_reuse_native_accum_release(struct comp_layer_accum *cla);

/*
 * Source-local replacement for comp_renderer.c's vk_cmd_submit_locked call.
 * It appends the reuse timeline signal to the exact VkSubmitInfo that samples
 * the current comp_layer_accum images, then performs the ordinary queue submit.
 */
VkResult
comp_swapchain_gpu_reuse_vk_cmd_submit_locked(struct comp_layer_accum *cla,
                                              struct vk_bundle *vk,
                                              struct vk_bundle_queue *queue,
                                              uint32_t count,
                                              const VkSubmitInfo *infos,
                                              VkFence fence);

#ifdef __cplusplus
}
#endif

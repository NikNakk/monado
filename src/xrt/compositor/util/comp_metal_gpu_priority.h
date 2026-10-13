// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experiment: Metal command-queue GPU priority for the macOS compositor.
 * @ingroup comp_util
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct vk_bundle;

/*!
 * Log the GPU priority of the compositor's MoltenVK queue and of
 * @p presenter_queue (an id<MTLCommandQueue>, may be NULL). With
 * XRT_MACOS_GPU_QUEUE_PRIORITY set to 0 (highest), 1 (the default) or 2
 * (lowest), also set both the foreground and background priority, and log what
 * reads back. Uses private Metal selectors, each checked before use.
 */
void
comp_metal_gpu_priority_apply(struct vk_bundle *vk, void *presenter_queue);

#ifdef __cplusplus
}
#endif

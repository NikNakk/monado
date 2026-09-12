// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Lightweight interface for compositor-to-client swapchain GPU reuse tracking.
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Enable fine-grained GPU reuse tracking on one native compositor swapchain.
 * Used by service-side Metal shared-texture swapchains, where client writes and
 * compositor Vulkan sampling access the same MTLTexture storage cross-process.
 */
xrt_result_t
comp_swapchain_gpu_reuse_enable(struct xrt_swapchain *xsc);

/*!
 * Hold/release a CPU-side pending-consumer claim for one image. These are
 * no-ops for untracked swapchains. A claim prevents wait_image() from taking
 * its final GPU-timeline snapshot while CPU bookkeeping still permits a future
 * compositor submission to consume the image.
 *
 * Returns true iff a claim was taken and therefore needs a matching release.
 */
bool
comp_swapchain_gpu_reuse_claim_image(struct xrt_swapchain *xsc, uint32_t image_index);

void
comp_swapchain_gpu_reuse_release_image(struct xrt_swapchain *xsc, uint32_t image_index);

#ifdef __cplusplus
}
#endif

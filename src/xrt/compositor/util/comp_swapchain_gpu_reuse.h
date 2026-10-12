// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Lightweight interface for compositor-to-client swapchain GPU reuse tracking.
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX

struct vk_bundle;
struct comp_gpu_client_budget;
struct comp_gpu_client_budget *
comp_gpu_client_budget_create(void);
uint64_t
comp_gpu_client_budget_trace_id(struct comp_gpu_client_budget *budget);
void
comp_gpu_client_budget_destroy(struct comp_gpu_client_budget **budget);
xrt_result_t
comp_gpu_client_budget_reserve(struct comp_gpu_client_budget *budget,
                               const struct xrt_swapchain_create_info *info,
                               uint32_t image_count,
                               const struct xrt_image_native *images);
void
comp_gpu_client_budget_finish(void);
xrt_result_t
comp_gpu_client_budget_adopt(struct xrt_swapchain *xsc);
bool
comp_swapchain_gpu_reuse_device_in_use(struct vk_bundle *vk);
/*!
 * Size the default GPU allocation budgets from @p vk's largest device-local
 * heap. Called once the compositor's device exists; swapchain tracking also
 * calls it, so this only makes the first client's limits accurate.
 */
void
comp_swapchain_gpu_reuse_note_device(struct vk_bundle *vk);
void
comp_swapchain_gpu_reuse_collect(struct vk_bundle *vk, bool device_teardown);

xrt_result_t
comp_swapchain_gpu_reuse_set_producer(struct xrt_swapchain *xsc,
                                      uint32_t index,
                                      struct xrt_compositor_semaphore *sem,
                                      uint64_t value);

/*!
 * Enable fine-grained GPU reuse tracking on one native compositor swapchain.
 * Used by local and service-side Metal shared-texture swapchains, where client
 * writes and compositor Vulkan sampling access the same storage on separate queues.
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

#else

// Tracking is only used on macOS; nothing is ever tracked on other platforms.
static inline bool
comp_swapchain_gpu_reuse_claim_image(struct xrt_swapchain *xsc, uint32_t image_index)
{
	(void)xsc;
	(void)image_index;
	return false;
}

static inline void
comp_swapchain_gpu_reuse_release_image(struct xrt_swapchain *xsc, uint32_t image_index)
{
	(void)xsc;
	(void)image_index;
}

#endif

#ifdef __cplusplus
}
#endif

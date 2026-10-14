// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Metal shared-event frame handoff for the Vulkan client compositor on macOS.
 * @ingroup comp_client
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Whether XRT_MACOS_VK_CLIENT_SHARED_EVENT asks for the shared-event handoff.
 */
bool
client_vk_shared_event_enabled(void);

/*!
 * Create a timeline semaphore on the application's VkDevice that imports a
 * Metal shared event the compositor waits on. Committing with it lets the
 * compositor wait for the application's GPU work, rather than the client
 * calling vkQueueWaitIdle on every frame, which is the only handoff macOS
 * otherwise has because it lacks opaque-FD timelines and sync-FD fences.
 *
 * On failure nothing is returned and the caller keeps its existing handoff.
 */
xrt_result_t
client_vk_shared_event_create(struct xrt_compositor_native *xcn,
                              struct vk_bundle *vk,
                              VkSemaphore *out_semaphore,
                              struct xrt_compositor_semaphore **out_xcsem,
                              uint64_t *out_value);

#ifdef __cplusplus
}
#endif

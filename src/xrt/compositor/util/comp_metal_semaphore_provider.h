// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Lightweight Metal shared-event semaphore provider API.
 * @ingroup comp_util
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_config_os.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX

/*!
 * Create a Vulkan timeline semaphore exported as an MTLSharedEvent.
 *
 * Available after the compositor's Metal semaphore probe has registered its
 * Vulkan bundle. The returned MTLSharedEvent pointer is borrowed; callers that
 * retain it beyond the call must take their own Objective-C reference.
 */
xrt_result_t
comp_metal_semaphore_create_client_pair(struct xrt_compositor_semaphore **out_xcsem, void **out_mtl_shared_event);

/*!
 * Import an existing MTLSharedEventHandle into the compositor timeline.
 * The handle remains owned by the caller.
 */
xrt_result_t
comp_metal_semaphore_import_shared_event_handle(void *mtl_shared_event_handle,
                                                struct xrt_compositor_semaphore **out_xcsem);

/*!
 * Experiment: make the compositor queue wait on @p xcsem at @p value once, and
 * wait up to @p timeout_ns for that submission to complete.
 *
 * Used to test whether the first GPU wait on a client-signalled shared event
 * stalls; see XRT_MACOS_PRIME_PRODUCER_EVENT.
 */
xrt_result_t
comp_metal_semaphore_prime_gpu_wait(struct xrt_compositor_semaphore *xcsem, uint64_t value, uint64_t timeout_ns);

#endif

#ifdef __cplusplus
}
#endif

// Copyright 2026, Collabora, Ltd.
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
 * Resolve a bootstrap-registered MTLSharedEvent Mach port (as used by DXMT
 * shared D3D11 fences) and import it into the compositor Vulkan timeline.
 */
xrt_result_t
comp_metal_semaphore_import_bootstrap_event(const char *bootstrap_name,
                                            struct xrt_compositor_semaphore **out_xcsem);

#endif

#ifdef __cplusplus
}
#endif

// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metal client side glue to compositor header.
 * @author OpenAI
 * @ingroup comp_client
 */

#pragma once

#include "xrt/xrt_gfx_metal.h"
#include "xrt/xrt_config_build.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * The plain Metal client compositor. With compositor util, the swapchain paths
 * below wrap it, and xrt_gfx_metal_provider_create() picks one.
 */
struct xrt_compositor_metal *
client_metal_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue);

#ifdef XRT_MODULE_COMPOSITOR_UTIL
/*!
 * Direct swapchains: Metal textures imported into the compositor's Vulkan in
 * this process. Used in-process, and by service clients hosted by the service.
 */
struct xrt_compositor_metal *
client_metal_direct_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue);
#endif

#if defined(XRT_MODULE_COMPOSITOR_UTIL) && defined(XRT_FEATURE_SERVICE)
/*!
 * Service swapchains: shared textures created by monado-service over XPC.
 */
struct xrt_compositor_metal *
client_metal_service_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue);
#endif

#ifdef __cplusplus
}
#endif

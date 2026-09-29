// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Glue code to Metal client side code.
 * @author OpenAI
 * @ingroup comp_client
 */

#include "xrt/xrt_config_build.h"
#include "client/comp_metal_client.h"
#ifdef XRT_MODULE_COMPOSITOR_UTIL
#include "client/comp_metal_release_wait_thread.h"
#ifdef XRT_FEATURE_SERVICE
#include "client/comp_metal_service_semaphore.h"
#include "util/u_macos_display_host.h"
#endif
#endif

#if defined(XRT_MODULE_COMPOSITOR_UTIL) && defined(XRT_FEATURE_SERVICE)
/* Both swapchain paths are built in service builds; see CMakeLists.txt. */
struct xrt_compositor_metal *
client_metal_service_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue);
struct xrt_compositor_metal *
client_metal_direct_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue);
#endif

struct xrt_compositor_metal *
xrt_gfx_metal_provider_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue)
{
#if defined(XRT_MODULE_COMPOSITOR_UTIL) && defined(XRT_FEATURE_SERVICE)
	/*
	 * A client hosted by the service composites in-process, so its native
	 * compositor is local: use the direct swapchain path. Otherwise the native
	 * compositor is the service's, reached over IPC and XPC.
	 */
	bool hosted = u_macos_hosted_client_available();
	struct xrt_compositor_metal *xcm = hosted ? client_metal_direct_compositor_create(xcn, metal_device, command_queue)
	                                          : client_metal_service_compositor_create(xcn, metal_device, command_queue);
	if (xcm != NULL && !hosted) {
		client_metal_service_semaphore_register_compositor(&xcm->base, metal_device);
	}
#else
	struct xrt_compositor_metal *xcm = client_metal_compositor_create(xcn, metal_device, command_queue);
#endif
#ifdef XRT_MODULE_COMPOSITOR_UTIL
	return client_metal_release_wait_thread_attach(xcm, command_queue);
#else
	return xcm;
#endif
}

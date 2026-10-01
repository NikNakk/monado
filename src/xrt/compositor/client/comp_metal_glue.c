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
#endif

struct xrt_compositor_metal *
xrt_gfx_metal_provider_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue)
{
#if defined(XRT_MODULE_COMPOSITOR_UTIL) && defined(XRT_FEATURE_SERVICE)
	struct xrt_compositor_metal *xcm =
	    xcn->is_remote ? client_metal_service_compositor_create(xcn, metal_device, command_queue)
	                   : client_metal_direct_compositor_create(xcn, metal_device, command_queue);
#elif defined(XRT_MODULE_COMPOSITOR_UTIL)
	struct xrt_compositor_metal *xcm = client_metal_direct_compositor_create(xcn, metal_device, command_queue);
#else
	struct xrt_compositor_metal *xcm = client_metal_compositor_create(xcn, metal_device, command_queue);
#endif
#ifdef XRT_MODULE_COMPOSITOR_UTIL
	return client_metal_release_wait_thread_attach(xcm, command_queue, xcn->is_remote);
#else
	return xcm;
#endif
}

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
#include "client/comp_metal_release_sync.h"
#endif

struct xrt_compositor_metal *
xrt_gfx_metal_provider_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue)
{
	struct xrt_compositor_metal *xcm = client_metal_compositor_create(xcn, metal_device, command_queue);
#ifdef XRT_MODULE_COMPOSITOR_UTIL
	return client_metal_release_sync_attach(xcm, command_queue);
#else
	return xcm;
#endif
}

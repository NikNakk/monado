// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Glue code to Metal client side code.
 * @author OpenAI
 * @ingroup comp_client
 */

#include "client/comp_metal_client.h"

struct xrt_compositor_metal *
xrt_gfx_metal_provider_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue)
{
	return client_metal_compositor_create(xcn, metal_device, command_queue);
}

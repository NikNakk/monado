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

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_compositor_metal *
client_metal_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue);

#ifdef __cplusplus
}
#endif

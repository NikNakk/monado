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

/*
 * macOS builds with compositor util wrap the ordinary Metal client constructor:
 * in-process builds use direct Metal/Vulkan imports, while service builds use
 * the XPC shared-texture transport. The underlying implementation is retained
 * as client_metal_compositor_create_vanilla().
 */
#if defined(__OBJC__) && defined(XRT_OS_OSX) && defined(XRT_MODULE_COMPOSITOR_UTIL)
#define client_metal_compositor_create client_metal_compositor_create_vanilla
#endif

struct xrt_compositor_metal *
client_metal_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue);

#ifdef __cplusplus
}
#endif

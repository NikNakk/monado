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
 * The in-process macOS build wraps the ordinary Metal client constructor with
 * the Metal-owned swapchain implementation. Service builds keep the ordinary
 * constructor: MTLTexture objects are process-local and service transport
 * remains a separate implementation step.
 */
#if defined(__OBJC__) && defined(XRT_OS_OSX) && defined(XRT_MODULE_COMPOSITOR_UTIL) && !defined(XRT_FEATURE_SERVICE)
#define client_metal_compositor_create client_metal_compositor_create_vanilla
#endif

struct xrt_compositor_metal *
client_metal_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue);

#ifdef __cplusplus
}
#endif

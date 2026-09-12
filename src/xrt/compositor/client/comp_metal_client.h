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

struct xrt_compositor_metal *
client_metal_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue);

/*
 * Experiment-only hook for the in-process macOS compositor. It intercepts the
 * one native swapchain creation in comp_metal_client.m so layered swapchains
 * can be created Metal-first and imported into Vulkan. Service builds are
 * deliberately excluded because the implementation casts the native swapchain
 * to comp_swapchain and is therefore intentionally in-process only.
 */
#if defined(__OBJC__) && defined(XRT_OS_OSX) && defined(XRT_MODULE_COMPOSITOR_UTIL) && !defined(XRT_FEATURE_SERVICE)
xrt_result_t
client_metal_array_import_experiment_create_swapchain(struct xrt_compositor_native *xcn,
                                                       const struct xrt_swapchain_create_info *info,
                                                       struct xrt_swapchain_native **out_xscn,
                                                       void *metal_device);

#define xrt_comp_native_create_swapchain(XCN, INFO, OUT_XSCN)                                                          \
	client_metal_array_import_experiment_create_swapchain((XCN), (INFO), (OUT_XSCN), (__bridge void *)c->device)
#endif

#ifdef __cplusplus
}
#endif

// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header defining a Metal graphics interface.
 * @author OpenAI
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_device.h"
#include "xrt/xrt_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(XRT_OS_OSX) || defined(XRT_DOXYGEN)

/*!
 * Create a Metal compositor client.
 *
 * @ingroup xrt_iface
 * @public @memberof xrt_compositor_native
 */
struct xrt_compositor_metal *
xrt_gfx_metal_provider_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue);

#endif // XRT_OS_OSX || XRT_DOXYGEN

#ifdef __cplusplus
}
#endif

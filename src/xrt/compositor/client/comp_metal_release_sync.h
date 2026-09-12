// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Experimental app-release Metal shared-event synchronization wrapper.
 * @ingroup comp_client
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "util/u_handles.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX

/*!
 * Optionally attach the Metal app-release shared-event synchronization
 * experiments to a Metal client compositor. Stage 2 keeps the blocking CPU
 * release barrier; Stage 3 replaces it with a Vulkan timeline GPU wait.
 * Returns @p xcm unchanged.
 */
struct xrt_compositor_metal *
client_metal_release_sync_attach(struct xrt_compositor_metal *xcm, void *command_queue);

#endif

#ifdef __cplusplus
}
#endif

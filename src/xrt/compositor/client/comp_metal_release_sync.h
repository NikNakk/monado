// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Diagnostic app-release Metal shared-event synchronization wrapper.
 * @ingroup comp_client
 */

#pragma once

#include "xrt/xrt_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX

/*!
 * Optionally attach the Stage-2 Metal app-release shared-event diagnostic to a
 * Metal client compositor. Returns @p xcm unchanged.
 */
struct xrt_compositor_metal *
client_metal_release_sync_attach(struct xrt_compositor_metal *xcm, void *command_queue);

#endif

#ifdef __cplusplus
}
#endif

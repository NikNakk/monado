// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Metal app-release handoff through Monado's compositor wait thread.
 * @ingroup comp_client
 */

#pragma once

#include "xrt/xrt_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX

bool
client_metal_release_wait_thread_enabled(void);

struct xrt_compositor_metal *
client_metal_release_wait_thread_attach(struct xrt_compositor_metal *xcm, void *command_queue);

#endif

#ifdef __cplusplus
}
#endif

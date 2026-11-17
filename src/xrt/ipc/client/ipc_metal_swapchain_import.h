// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS Metal IPC call shims.
 * @ingroup ipc_client
 */

#pragma once

#include "client/ipc_client.h"
#include "shared/ipc_protocol.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

xrt_result_t
ipc_metal_call_compositor_semaphore_create_or_default(struct ipc_connection *ipc_c,
                                                      uint32_t *out_id,
                                                      xrt_graphics_sync_handle_t *out_handles,
                                                      uint32_t max_handle_count);

#ifdef __cplusplus
}
#endif

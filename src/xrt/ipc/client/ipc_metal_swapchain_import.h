// Copyright 2026, Collabora, Ltd.
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
ipc_metal_call_swapchain_import_or_default(struct ipc_connection *ipc_c,
                                           const struct xrt_swapchain_create_info *info,
                                           const struct ipc_arg_swapchain_from_native *args,
                                           const xrt_graphics_buffer_handle_t *handles,
                                           uint32_t handle_count,
                                           uint32_t *out_id);

xrt_result_t
ipc_metal_call_compositor_semaphore_create_or_default(struct ipc_connection *ipc_c,
                                                      uint32_t *out_id,
                                                      xrt_graphics_sync_handle_t *out_handles,
                                                      uint32_t max_handle_count);

#ifdef __cplusplus
}
#endif

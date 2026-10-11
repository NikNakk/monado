// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Bounded swapchain image waits for IPC worker threads on macOS.
 * @ingroup ipc_server
 */

#pragma once

#include "xrt/xrt_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

//! Longest single swapchain wait an IPC worker makes before re-checking its client.
#define IPC_SERVER_SWAPCHAIN_WAIT_SLICE_NS (500 * INT64_C(1000000))

/*!
 * Wait for @p index of @p xsc for up to @p timeout_ns (INT64_MAX is infinite),
 * in slices of at most @ref IPC_SERVER_SWAPCHAIN_WAIT_SLICE_NS.
 *
 * Between slices, stop with XRT_TIMEOUT if @p running is false or the client on
 * @p client_fd has hung up. A client whose GPU work never completes, or that
 * dies mid-wait, then cannot pin the worker, while the application's own
 * timeout is still honoured.
 */
xrt_result_t
ipc_server_swapchain_wait_in_slices(struct xrt_swapchain *xsc,
                                    int64_t timeout_ns,
                                    uint32_t index,
                                    int client_fd,
                                    const volatile bool *running);

#ifdef __cplusplus
}
#endif

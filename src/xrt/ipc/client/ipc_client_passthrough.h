// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Passthrough camera frames from the service, for clients that
 *         composite in-process. See doc/macos-client-compositor-design.md.
 * @ingroup ipc_client
 */

#pragma once

#include "xrt/xrt_frame.h"
#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_connection;
struct ipc_client_passthrough;

struct ipc_client_passthrough *
ipc_client_passthrough_create(struct ipc_connection *ipc_c, uint32_t device_id);

/*!
 * Deliver the service's camera frames to @p left and @p right, or stop with
 * two NULL sinks. The first call maps the service's frame share and starts a
 * reader thread.
 */
xrt_result_t
ipc_client_passthrough_set_sinks(struct ipc_client_passthrough *icp,
                                 struct xrt_frame_sink *left,
                                 struct xrt_frame_sink *right);

void
ipc_client_passthrough_destroy(struct ipc_client_passthrough **icp_ptr);

#ifdef __cplusplus
}
#endif

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS clients that composite in-process and are hosted by the service.
 *
 * See doc/macos-client-compositor-design.md.
 *
 * @ingroup ipc_client
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_device.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_connection;

/*!
 * Try to create a system compositor in this process, presenting through the
 * service's headset window. Opt-in with XRT_MACOS_CLIENT_COMPOSITOR=1.
 *
 * @return XRT_SUCCESS with @p out_xsysc set, or an error with nothing left
 *         registered, in which case the caller uses the service's compositor.
 */
xrt_result_t
ipc_client_macos_hosted_create_system_compositor(struct ipc_connection *ipc_c,
                                                 struct xrt_device *head,
                                                 struct xrt_system_compositor **out_xsysc);

//! Undo the registration made by a successful create, after the compositor is gone.
void
ipc_client_macos_hosted_fini(struct ipc_connection *ipc_c);

#ifdef __cplusplus
}
#endif

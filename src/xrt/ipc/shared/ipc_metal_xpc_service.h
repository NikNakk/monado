// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Direct monado-service XPC endpoint for macOS Metal transport.
 * @ingroup ipc_shared
 */

#pragma once

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX

/*!
 * Start the launchd Mach-service listener inside monado-service.
 *
 * The listener exports the same Metal handle protocol previously hosted by the
 * standalone broker. It is intentionally started only after the ordinary Unix
 * IPC socket is ready, so a successful activation reply also means clients can
 * immediately connect to the normal Monado IPC transport.
 */
xrt_result_t
ipc_metal_xpc_service_start(void);

/*! Stop and release the in-process XPC listener. */
void
ipc_metal_xpc_service_stop(void);

/*!
 * Ask launchd for the Monado Metal Mach service and wait until monado-service
 * reports that its ordinary IPC socket is ready.
 *
 * If monado-service is already running under launchd this is a cheap readiness
 * handshake. If it is not running, opening the Mach service causes launchd to
 * start it on demand.
 */
xrt_result_t
ipc_metal_xpc_activate_service(void);

#endif // XRT_OS_OSX

#ifdef __cplusplus
}
#endif

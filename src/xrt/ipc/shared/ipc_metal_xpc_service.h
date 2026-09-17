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

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX

/*!
 * Diagnostic-only override for manually-started monado-service testing.
 *
 * When XRT_MACOS_METAL_XPC_EXTERNAL_BROKER=1, the service does not host the
 * Metal Mach service itself. Server-side Metal resource operations are routed
 * through the legacy standalone broker instead. This isolates launchd process
 * policy from the compositor while preserving cross-process Metal transport.
 */
bool
ipc_metal_xpc_external_broker_enabled(void);

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
 */
xrt_result_t
ipc_metal_xpc_activate_service(void);

/*!
 * Consume client-published texture handles directly from the registry hosted
 * by monado-service. The token must belong to the Unix IPC client's PID.
 */
xrt_result_t
ipc_metal_xpc_service_take_textures_for_pid(uint64_t token,
                                             uint32_t expected_count,
                                             void **out_metal_textures,
                                             pid_t owner_pid);

/*!
 * Publish a service-created MTLSharedEvent for one Unix IPC client. The XPC
 * peer that retrieves it must have the same PID.
 */
xrt_result_t
ipc_metal_xpc_service_publish_shared_event_for_pid(void *metal_shared_event,
                                                    uint64_t *out_token,
                                                    pid_t owner_pid);

/*! Drop a token only when it belongs to the supplied Unix IPC client PID. */
void
ipc_metal_xpc_service_discard_token_for_pid(uint64_t token, pid_t owner_pid);

/*!
 * Drop every still-pending registry entry owned by a process.
 *
 * Call this only after the last ordinary Monado IPC client for that PID has
 * disconnected. XPC connections themselves are deliberately short-lived and
 * are not a resource-lifetime signal.
 */
void
ipc_metal_xpc_service_discard_all_for_pid(pid_t owner_pid);

#endif // XRT_OS_OSX

#ifdef __cplusplus
}
#endif

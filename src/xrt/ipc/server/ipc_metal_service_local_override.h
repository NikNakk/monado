// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Route monado-service Metal transport calls to its in-process registry.
 * @ingroup ipc_server
 */

#pragma once

/*
 * Include the ordinary declarations before defining the source-local aliases.
 * This header is force-included by CMake, so defining the function-like macros
 * first would otherwise rewrite the declarations in ipc_metal_xpc.h itself.
 */
#include "shared/ipc_metal_xpc.h"
#include "shared/ipc_metal_xpc_service.h"

/*
 * ipc_server_metal.c runs inside monado-service itself. Once the service hosts
 * the Metal XPC registry there is no reason for it to open an XPC connection
 * back to its own Mach service. Keep the existing server implementation and
 * substitute only the resource-registry operations.
 *
 * The ordinary Unix IPC client state already records the application's PID.
 * Inject that PID here so the local registry can verify that a token published
 * over XPC belongs to the same process that is asking to import it over normal
 * Monado IPC.
 */
#define ipc_metal_xpc_take_textures(TOKEN, COUNT, OUT)                                                                \
	ipc_metal_xpc_service_take_textures_for_pid((TOKEN), (COUNT), (OUT), ics->client_state.pid)
#define ipc_metal_xpc_publish_shared_event(EVENT, OUT_TOKEN)                                                          \
	ipc_metal_xpc_service_publish_shared_event_for_pid((EVENT), (OUT_TOKEN), ics->client_state.pid)
#define ipc_metal_xpc_discard_token(TOKEN)                                                                            \
	ipc_metal_xpc_service_discard_token_for_pid((TOKEN), ics->client_state.pid)

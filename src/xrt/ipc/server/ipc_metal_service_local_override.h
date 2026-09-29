// Copyright 2026, Nick Kennedy
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
 * Service builds consume Metal resources directly from the registry hosted
 * inside monado-service, checked against the owning client's PID.
 */
static inline xrt_result_t
ipc_metal_server_take_textures(uint64_t token, uint32_t count, void **out, pid_t owner_pid)
{
	return ipc_metal_xpc_service_take_textures_for_pid(token, count, out, owner_pid);
}

static inline xrt_result_t
ipc_metal_server_publish_shared_event(void *event, uint64_t *out_token, pid_t owner_pid)
{
	return ipc_metal_xpc_service_publish_shared_event_for_pid(event, out_token, owner_pid);
}

static inline void
ipc_metal_server_discard_token(uint64_t token, pid_t owner_pid)
{
	ipc_metal_xpc_service_discard_token_for_pid(token, owner_pid);
}

/*
 * The ordinary Unix IPC client state records the application's PID. Inject it
 * into the direct-registry path.
 */
#define ipc_metal_xpc_take_textures(TOKEN, COUNT, OUT) \
	ipc_metal_server_take_textures((TOKEN), (COUNT), (OUT), ics->client_state.pid)
#define ipc_metal_xpc_publish_shared_event(EVENT, OUT_TOKEN) \
	ipc_metal_server_publish_shared_event((EVENT), (OUT_TOKEN), ics->client_state.pid)
#define ipc_metal_xpc_discard_token(TOKEN) \
	ipc_metal_server_discard_token((TOKEN), ics->client_state.pid)

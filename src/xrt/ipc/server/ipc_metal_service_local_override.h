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
 * Normal service builds consume Metal resources directly from the registry
 * hosted inside monado-service. For the diagnostic manual-service A/B,
 * XRT_MACOS_METAL_XPC_EXTERNAL_BROKER=1 routes the same operations through the
 * legacy standalone XPC broker instead. This lets monado-service run directly
 * from a terminal while preserving cross-process Metal handle transport.
 *
 * The external mode deliberately gives up the direct registry's PID ownership
 * check; it is for scheduler/process-policy diagnosis only and is not the
 * production architecture.
 */
static inline xrt_result_t
ipc_metal_server_take_textures(uint64_t token, uint32_t count, void **out, pid_t owner_pid)
{
	if (ipc_metal_xpc_external_broker_enabled()) {
		return ipc_metal_xpc_take_textures(token, count, out);
	}

	return ipc_metal_xpc_service_take_textures_for_pid(token, count, out, owner_pid);
}

static inline xrt_result_t
ipc_metal_server_publish_shared_event(void *event, uint64_t *out_token, pid_t owner_pid)
{
	if (ipc_metal_xpc_external_broker_enabled()) {
		return ipc_metal_xpc_publish_shared_event(event, out_token);
	}

	return ipc_metal_xpc_service_publish_shared_event_for_pid(event, out_token, owner_pid);
}

static inline void
ipc_metal_server_discard_token(uint64_t token, pid_t owner_pid)
{
	if (ipc_metal_xpc_external_broker_enabled()) {
		ipc_metal_xpc_discard_token(token);
		return;
	}

	ipc_metal_xpc_service_discard_token_for_pid(token, owner_pid);
}

/*
 * The ordinary Unix IPC client state records the application's PID. Inject it
 * into the direct-registry path; the external-broker diagnostic ignores it.
 */
#define ipc_metal_xpc_take_textures(TOKEN, COUNT, OUT) \
	ipc_metal_server_take_textures((TOKEN), (COUNT), (OUT), ics->client_state.pid)
#define ipc_metal_xpc_publish_shared_event(EVENT, OUT_TOKEN) \
	ipc_metal_server_publish_shared_event((EVENT), (OUT_TOKEN), ics->client_state.pid)
#define ipc_metal_xpc_discard_token(TOKEN) \
	ipc_metal_server_discard_token((TOKEN), ics->client_state.pid)

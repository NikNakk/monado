// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS service cleanup of per-process Metal XPC registry entries.
 * @ingroup ipc_server
 */

#include "server/ipc_server.h"
#include "shared/ipc_metal_xpc_service.h"

void
ipc_metal_client_cleanup(volatile struct ipc_client_state *ics)
{
	struct ipc_server *server = ics->server;
	pid_t owner_pid = ics->client_state.pid;

	if (server == NULL || owner_pid <= 0) {
		return;
	}

	/*
	 * common_shutdown() calls us while global_state.lock is already held and
	 * before client_state is cleared. If another live Monado IPC connection
	 * belongs to the same process, its Metal tokens are still potentially in
	 * use, so leave the registry untouched.
	 */
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		volatile struct ipc_client_state *other = &server->threads[i].ics;
		if (other == ics) {
			continue;
		}
		if (other->server_thread_index >= 0 && other->client_state.pid == owner_pid) {
			IPC_TRACE(server,
			          "Keeping Metal XPC tokens for pid=%d: another IPC client is still connected",
			          (int)owner_pid);
			return;
		}
	}

	ipc_metal_xpc_service_discard_all_for_pid(owner_pid);
}

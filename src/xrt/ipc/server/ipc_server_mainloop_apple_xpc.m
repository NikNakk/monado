// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Add direct Metal XPC lifecycle to the macOS IPC server main loop.
 * @ingroup ipc_server
 */

#include "server/ipc_server.h"
#include "shared/ipc_metal_xpc_service.h"
#include "util/u_logging.h"

/*
 * ipc_server_mainloop_apple.c is compiled with source-local symbol redirects
 * for init/deinit. Keep the established socket and signal implementation there
 * and add only the macOS XPC lifecycle here.
 */
int
ipc_server_mainloop_init_apple_vanilla(struct ipc_server_mainloop *ml, bool no_stdin);

void
ipc_server_mainloop_deinit_apple_vanilla(struct ipc_server_mainloop *ml);

int
ipc_server_mainloop_init(struct ipc_server_mainloop *ml, bool no_stdin)
{
	int ret = ipc_server_mainloop_init_apple_vanilla(ml, no_stdin);
	if (ret < 0) {
		return ret;
	}

	/*
	 * The Unix socket is already listening at this point. The XPC activation
	 * reply therefore acts as a readiness barrier before a client retries its
	 * ordinary Monado IPC connection.
	 *
	 * Keep failure non-fatal so manually-started development services can still
	 * use the legacy standalone broker while this direct path is being tested.
	 */
	xrt_result_t xret = ipc_metal_xpc_service_start();
	if (xret != XRT_SUCCESS) {
		U_LOG_W("Direct Metal XPC endpoint unavailable; continuing with ordinary Monado IPC");
	}

	return ret;
}

void
ipc_server_mainloop_deinit(struct ipc_server_mainloop *ml)
{
	ipc_metal_xpc_service_stop();
	ipc_server_mainloop_deinit_apple_vanilla(ml);
}

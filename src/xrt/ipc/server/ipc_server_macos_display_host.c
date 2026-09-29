// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Server handlers for clients that composite in their own process.
 *
 * Such a client presents into a CAContext and asks the service to show it in
 * the headset window. See doc/macos-client-compositor-design.md.
 *
 * @ingroup ipc_server
 */

#include "server/ipc_server.h"
#include "util/u_macos_display_host.h"
#include "util/u_trace_marker.h"

#include "ipc_server_generated.h"

xrt_result_t
ipc_handle_compositor_hosted_attach(volatile struct ipc_client_state *ics, uint32_t context_id)
{
	IPC_TRACE_MARKER();

	uint32_t client_id = ics->client_state.id;
	xrt_result_t xret = u_macos_display_host_attach(client_id, context_id);
	if (xret == XRT_SUCCESS) {
		ics->hosted_client_id = client_id;
		ics->hosted_attached = true;
	}
	return xret;
}

xrt_result_t
ipc_handle_compositor_hosted_set_visibility(volatile struct ipc_client_state *ics, uint32_t visibility)
{
	IPC_TRACE_MARKER();

	if (!ics->hosted_attached) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	if (visibility > U_MACOS_DISPLAY_HOST_EXCLUSIVE) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	return u_macos_display_host_set_visibility(ics->hosted_client_id,
	                                           (enum u_macos_display_host_visibility)visibility);
}

xrt_result_t
ipc_handle_compositor_hosted_detach(volatile struct ipc_client_state *ics)
{
	IPC_TRACE_MARKER();

	ipc_server_macos_display_host_client_gone(ics);
	return XRT_SUCCESS;
}

void
ipc_server_macos_display_host_client_gone(volatile struct ipc_client_state *ics)
{
	if (!ics->hosted_attached) {
		return;
	}
	u_macos_display_host_detach(ics->hosted_client_id);
	ics->hosted_attached = false;
	ics->hosted_client_id = 0;
}

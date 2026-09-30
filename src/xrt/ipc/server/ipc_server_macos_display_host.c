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
#include "server/ipc_server_objects.h"
#include "shared/ipc_shmem.h"
#include "util/u_frame_share.h"
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


/*
 *
 * Passthrough camera frames for clients compositing in-process.
 *
 */

//! Large enough for a PS VR2 BC4 frame (1024 x 1016) or an L8 640 x 640 region.
#define PASSTHROUGH_SHARE_MAX_FRAME_SIZE (1024 * 1024)

void
ipc_server_passthrough_share_init(struct ipc_server *s)
{
	os_mutex_init(&s->passthrough_share.lock);
	s->passthrough_share.handle = XRT_SHMEM_HANDLE_INVALID;
	s->passthrough_share.mem = NULL;
	s->passthrough_share.size = 0;
}

void
ipc_server_passthrough_share_fini(struct ipc_server *s)
{
	// Waits for a frame being published to finish.
	u_passthrough_share_set_target(NULL);

	os_mutex_lock(&s->passthrough_share.lock);
	if (s->passthrough_share.mem != NULL) {
		ipc_shmem_destroy(&s->passthrough_share.handle, &s->passthrough_share.mem, s->passthrough_share.size);
		s->passthrough_share.size = 0;
	}
	os_mutex_unlock(&s->passthrough_share.lock);
	os_mutex_destroy(&s->passthrough_share.lock);
}

xrt_result_t
ipc_handle_device_passthrough_share_get(volatile struct ipc_client_state *ics,
                                        uint32_t id,
                                        uint64_t *out_size,
                                        uint32_t max_handle_capacity,
                                        xrt_shmem_handle_t *out_handles,
                                        uint32_t *out_handle_count)
{
	IPC_TRACE_MARKER();

	struct ipc_server *s = ics->server;
	*out_size = 0;
	*out_handle_count = 0;

	struct xrt_device *xdev = NULL;
	xrt_result_t xret = ipc_server_objects_get_xdev_and_validate(ics, id, &xdev);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	if (xdev != s->xsysd->static_roles.head || !u_passthrough_share_source_available()) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (max_handle_capacity < 1) {
		return XRT_ERROR_IPC_FAILURE;
	}

	os_mutex_lock(&s->passthrough_share.lock);
	if (s->passthrough_share.mem == NULL) {
		size_t size = u_frame_share_size(2, PASSTHROUGH_SHARE_MAX_FRAME_SIZE);
		xrt_shmem_handle_t handle = XRT_SHMEM_HANDLE_INVALID;
		void *mem = NULL;
		xret = ipc_shmem_create_private("passthrough", size, &handle, &mem);
		if (xret == XRT_SUCCESS && !u_frame_share_init(mem, size, 2, PASSTHROUGH_SHARE_MAX_FRAME_SIZE)) {
			ipc_shmem_destroy(&handle, &mem, size);
			xret = XRT_ERROR_ALLOCATION;
		}
		if (xret == XRT_SUCCESS) {
			s->passthrough_share.handle = handle;
			s->passthrough_share.mem = mem;
			s->passthrough_share.size = size;
			u_passthrough_share_set_target(mem);
			IPC_INFO(s, "Sharing passthrough camera frames with clients (%zu bytes)", size);
		}
	}
	if (xret == XRT_SUCCESS) {
		out_handles[0] = s->passthrough_share.handle;
		*out_handle_count = 1;
		*out_size = s->passthrough_share.size;
	}
	os_mutex_unlock(&s->passthrough_share.lock);

	return xret;
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS Metal IPC call shims.
 * @ingroup ipc_client
 */

#include "client/ipc_metal_swapchain_import.h"
#include "ipc_client_generated.h"
#include "shared/ipc_metal_xpc.h"

xrt_result_t
ipc_metal_call_compositor_semaphore_create_or_default(struct ipc_connection *ipc_c,
                                                      uint32_t *out_id,
                                                      xrt_graphics_sync_handle_t *out_handles,
                                                      uint32_t max_handle_count)
{
#ifdef XRT_OS_OSX
	if (ipc_metal_xpc_shared_event_request_active()) {
		if (ipc_c == NULL || out_id == NULL || out_handles == NULL || max_handle_count == 0) {
			return XRT_ERROR_INVALID_ARGUMENT;
		}

		uint32_t id = 0;
		uint64_t token = 0;
		xrt_result_t xret = ipc_call_compositor_semaphore_create_metal(ipc_c, &id, &token);
		if (xret != XRT_SUCCESS) {
			return xret;
		}

		xret = ipc_metal_xpc_resolve_shared_event_request(token);
		if (xret != XRT_SUCCESS) {
			(void)ipc_call_compositor_semaphore_destroy(ipc_c, id);
			return xret;
		}

		*out_id = id;
		out_handles[0] = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
		return XRT_SUCCESS;
	}
#endif

	return ipc_call_compositor_semaphore_create(ipc_c, out_id, out_handles, max_handle_count);
}

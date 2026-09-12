// Copyright 2026, Collabora, Ltd.
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
ipc_metal_call_swapchain_import_or_default(struct ipc_connection *ipc_c,
                                           const struct xrt_swapchain_create_info *info,
                                           const struct ipc_arg_swapchain_from_native *args,
                                           const xrt_graphics_buffer_handle_t *handles,
                                           uint32_t handle_count,
                                           uint32_t *out_id)
{
#ifdef XRT_OS_OSX
	if (ipc_c != NULL && info != NULL && args != NULL && handles != NULL && out_id != NULL && handle_count > 0 &&
	    handle_count <= XRT_MAX_SWAPCHAIN_IMAGES) {
		struct xrt_image_native images[XRT_MAX_SWAPCHAIN_IMAGES] = XRT_STRUCT_INIT;
		for (uint32_t i = 0; i < handle_count; i++) {
			images[i].handle = handles[i];
			images[i].size = args->sizes[i];
		}

		uint64_t token = 0;
		if (ipc_metal_xpc_get_token_from_images(images, handle_count, &token)) {
			return ipc_call_swapchain_import_metal(ipc_c, info, token, handle_count, out_id);
		}
	}
#endif

	return ipc_call_swapchain_import(ipc_c, info, args, handles, handle_count, out_id);
}

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

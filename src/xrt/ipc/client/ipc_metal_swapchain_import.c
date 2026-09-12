// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS Metal-token swapchain import shim for IPC client.
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

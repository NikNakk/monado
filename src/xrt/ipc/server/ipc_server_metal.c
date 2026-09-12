// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS Metal shared-texture swapchain import for IPC server.
 * @ingroup ipc_server
 */

#include "server/ipc_server.h"
#include "shared/ipc_metal_xpc.h"
#include "util/u_trace_marker.h"

#ifdef XRT_OS_OSX
#include "util/comp_metal_swapchain_import.h"
#endif

static xrt_result_t
find_free_swapchain_index(volatile struct ipc_client_state *ics, uint32_t *out_index)
{
	for (uint32_t index = 0; index < IPC_MAX_CLIENT_SWAPCHAINS; index++) {
		if (!ics->swapchain_data[index].active) {
			*out_index = index;
			return XRT_SUCCESS;
		}
	}

	IPC_ERROR(ics->server, "Too many swapchains!");
	return XRT_ERROR_IPC_FAILURE;
}

xrt_result_t
ipc_handle_swapchain_import_metal(volatile struct ipc_client_state *ics,
                                  const struct xrt_swapchain_create_info *info,
                                  uint64_t token,
                                  uint32_t image_count,
                                  uint32_t *out_id)
{
	IPC_TRACE_MARKER();

#ifndef XRT_OS_OSX
	(void)ics;
	(void)info;
	(void)token;
	(void)image_count;
	(void)out_id;
	return XRT_ERROR_NOT_IMPLEMENTED;
#else
	if (ics == NULL || info == NULL || out_id == NULL || ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}
	if (image_count == 0 || image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	uint32_t index = 0;
	xrt_result_t xret = find_free_swapchain_index(ics, &index);
	if (xret != XRT_SUCCESS) {
		ipc_metal_xpc_discard_token(token);
		return xret;
	}

	void *textures[XRT_MAX_SWAPCHAIN_IMAGES] = {0};
	xret = ipc_metal_xpc_take_textures(token, image_count, textures);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server,
		          "Failed to retrieve Metal XPC swapchain textures token=0x%016llx count=%u",
		          (unsigned long long)token,
		          image_count);
		return xret;
	}

	if (!comp_metal_swapchain_import_begin(info, image_count, textures)) {
		ipc_metal_xpc_release_textures(textures, image_count);
		return XRT_ERROR_ALLOCATION;
	}

	struct xrt_swapchain *xsc = NULL;
	xret = xrt_comp_create_swapchain(ics->xc, info, &xsc);
	bool consumed = comp_metal_swapchain_import_was_consumed();
	comp_metal_swapchain_import_end();

	/*
	 * VkImportMetalTextureInfoEXT has retained/consumed the Metal backing it
	 * needs by the time the synchronous create call returns. The broker-side
	 * reconstruction references are no longer needed here.
	 */
	ipc_metal_xpc_release_textures(textures, image_count);

	if (xret != XRT_SUCCESS) {
		return xret;
	}
	if (!consumed || xsc == NULL || xsc->image_count != image_count) {
		IPC_ERROR(ics->server,
		          "Metal IPC swapchain allocator mismatch: consumed=%s expected=%u actual=%u",
		          consumed ? "true" : "false",
		          image_count,
		          xsc != NULL ? xsc->image_count : 0);
		xrt_swapchain_reference(&xsc, NULL);
		return XRT_ERROR_VULKAN;
	}

	ics->swapchain_count++;
	ics->xscs[index] = xsc;
	ics->swapchain_data[index].active = true;
	ics->swapchain_data[index].width = info->width;
	ics->swapchain_data[index].height = info->height;
	ics->swapchain_data[index].format = info->format;
	ics->swapchain_data[index].image_count = xsc->image_count;
	*out_id = index;

	IPC_INFO(ics->server,
	         "Metal IPC swapchain active: id=%u images=%u size=%ux%u array_size=%u token=0x%016llx",
	         index,
	         image_count,
	         info->width,
	         info->height,
	         info->array_size,
	         (unsigned long long)token);

	return XRT_SUCCESS;
#endif
}

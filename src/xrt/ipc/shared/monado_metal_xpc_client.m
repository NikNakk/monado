// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0

#include "shared/monado_metal_xpc_client.h"

#include "shared/ipc_metal_xpc.h"

int
monado_metal_xpc_publish_claimable_texture(void *metal_texture,
                                           uint64_t *out_token)
{
	if (metal_texture == NULL || out_token == NULL) {
		return (int)XRT_ERROR_INVALID_ARGUMENT;
	}
	void *textures[1] = {metal_texture};
	return (int)ipc_metal_xpc_publish_claimable_textures(textures, 1, out_token);
}

int
monado_metal_xpc_take_texture(uint64_t token, void **out_metal_texture)
{
	if (out_metal_texture == NULL) {
		return (int)XRT_ERROR_INVALID_ARGUMENT;
	}
	void *textures[1] = {NULL};
	xrt_result_t xret = ipc_metal_xpc_take_textures(token, 1, textures);
	if (xret == XRT_SUCCESS) {
		*out_metal_texture = textures[0];
	} else {
		*out_metal_texture = NULL;
	}
	return (int)xret;
}

void
monado_metal_xpc_release_texture(void *metal_texture)
{
	void *textures[1] = {metal_texture};
	ipc_metal_xpc_release_textures(textures, 1);
}

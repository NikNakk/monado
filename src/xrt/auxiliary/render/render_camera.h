// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "vk/vk_helpers.h"
#include "xrt/xrt_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

//! Stereo ray-to-camera projection, matching the shader's std140 layout.
struct render_camera_params
{
	float brightness;
	uint32_t has_app, map_size, rotate;
	struct
	{
		float intrinsics[4], k[4], rows[3][4];
	} cameras[2];
};
struct render_camera;

//! All images belong to the caller and must outlive this renderer. The maps
//! contain static UVs in xy and optical tangent rays in zw, one float4 per texel.
struct render_camera *
render_camera_create(struct vk_bundle *vk,
                     struct vk_bundle_queue *queue,
                     VkShaderModule shader,
                     VkFormat format,
                     VkExtent2D extent,
                     VkExtent2D camera_extent,
                     uint32_t map_size,
                     const float *maps[2],
                     const VkImage *images,
                     const VkImageView *views,
                     uint32_t count);

//! Upload a CPU snapshot and queue camera composition after render_complete.
//! The binary semaphore is consumed and re-signalled on the GPU, preserving the
//! caller's existing present wait. Per-image storage is reused only after its
//! previous submission completes; normal submissions never wait for this frame.
//! The image starts and finishes in layout. The caller serializes these calls.
//! OOM leaves synchronization unchanged. Other submission failures require
//! target retirement because device state may be uncertain.
VkResult
render_camera_submit(struct render_camera *rc,
                     uint32_t index,
                     VkImageLayout layout,
                     VkSemaphore render_complete,
                     struct xrt_frame *frames[2],
                     const struct render_camera_params *params,
                     bool *submitted);

//! Bounded drain. On timeout retain *rc and all GPU-visible storage; retry after
//! completion or retain until device destruction. Never free pending resources.
VkResult
render_camera_destroy(struct render_camera **rc);

#ifdef __cplusplus
}
#endif

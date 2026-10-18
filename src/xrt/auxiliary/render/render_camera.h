// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "vk/vk_helpers.h"
#include "xrt/xrt_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

//! What the pass draws behind the application.
enum render_camera_mode
{
	//! Camera image through the static UVs in the maps.
	RENDER_CAMERA_MODE_STATIC = 0,
	//! Camera image through the calibrated projection in cameras[].
	RENDER_CAMERA_MODE_ROTATE = 1,
	//! A simulated room seen from views[]; no camera frames are used.
	RENDER_CAMERA_MODE_ROOM = 2,
};

//! Stereo ray-to-camera projection, matching the shader's std140 layout.
struct render_camera_params
{
	float brightness;
	uint32_t has_app, map_size;
	uint32_t mode; //!< enum render_camera_mode
	struct
	{
		float intrinsics[4], k[4], rows[3][4];
	} cameras[2];
	//! RENDER_CAMERA_MODE_ROOM: each eye's world pose; orientation x, y, z, w.
	struct
	{
		float orientation[4], position[4];
	} views[2];
};
struct render_camera;

//! All images belong to the caller and must outlive this renderer. Sources hold
//! the composited application (premultiplied alpha), are sampled in
//! source_layout and need SAMPLED usage. Destinations are only rendered to
//! (COLOR_ATTACHMENT usage) and finish in destination_layout. Sources and
//! destinations must not alias. The maps contain static UVs in xy and optical
//! tangent rays in zw, one float4 per texel. vert/frag are the shared
//! full-screen and camera shaders.
struct render_camera *
render_camera_create(struct vk_bundle *vk,
                     struct vk_bundle_queue *queue,
                     VkShaderModule vert,
                     VkShaderModule frag,
                     VkFormat format,
                     VkExtent2D extent,
                     VkExtent2D camera_extent,
                     uint32_t map_size,
                     const float *maps[2],
                     const VkImage *sources,
                     const VkImageView *source_views,
                     VkImageLayout source_layout,
                     const VkImageView *destinations,
                     VkImageLayout destination_layout,
                     uint32_t count);

//! Upload a CPU snapshot and queue camera composition after render_complete.
//! In RENDER_CAMERA_MODE_ROOM frames may be NULL and are ignored.
//! The binary semaphore is consumed and re-signalled on the GPU, preserving the
//! caller's existing present wait. Per-image storage is reused only after its
//! previous submission completes; normal submissions never wait for this frame.
//! The source stays in its layout; previous destination contents are discarded.
//! The caller serializes these calls. OOM leaves synchronization unchanged.
//! Other submission failures require target retirement because device state
//! may be uncertain.
VkResult
render_camera_submit(struct render_camera *rc,
                     uint32_t index,
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

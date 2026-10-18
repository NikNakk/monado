// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulated passthrough: a room drawn at the display pose, no cameras.
 * @ingroup comp_main
 *
 * The room is drawn at exactly the eye poses the compositor timewarped this
 * image to. Application content and room should therefore never slip against
 * each other; if they do, the fault is in composition or presentation rather
 * than in camera handling.
 */

#include "main/comp_sim_room_passthrough.h"
#include "main/comp_compositor.h"

#include "math/m_mathinclude.h"

#include <stdlib.h>
#include <string.h>

struct comp_sim_room
{
	struct comp_compositor *c;
	float *maps[2];
};

static bool
create_map(struct comp_sim_room *room, uint32_t eye)
{
	struct xrt_device *xdev = room->c->xdev;
	const struct xrt_fov fov = xdev->hmd->distortion.fov[eye];
	const float left = tanf(fov.angle_left), right = tanf(fov.angle_right);
	const float up = tanf(fov.angle_up), down = tanf(fov.angle_down);
	const uint32_t size = COMP_SIM_ROOM_MAP_SIZE;

	float *map = calloc((size_t)size * size * 4, sizeof(float));
	if (map == NULL) {
		return false;
	}
	// Without lenses the source UV is the target UV. This also avoids one IPC
	// round trip per texel when a client composites in-process.
	bool no_lens = xdev->hmd->distortion.preferred == XRT_DISTORTION_MODEL_NONE;
	for (uint32_t y = 0; y < size; y++) {
		float v = ((float)y + 0.5f) / (float)size;
		for (uint32_t x = 0; x < size; x++) {
			float u = ((float)x + 0.5f) / (float)size;
			float *texel = map + ((size_t)y * size + x) * 4;
			texel[0] = texel[1] = -1.0f; // No camera image.
			struct xrt_uv_triplet distortion = {.g = {u, v}};
			if (!no_lens && xrt_device_compute_distortion(xdev, eye, u, v, &distortion) != XRT_SUCCESS) {
				texel[2] = texel[3] = NAN;
				continue;
			}
			// The green-channel source UV spans the view's field of view; v grows downwards.
			texel[2] = left + distortion.g.x * (right - left);
			texel[3] = -(up + distortion.g.y * (down - up));
		}
	}
	room->maps[eye] = map;
	return true;
}

struct comp_sim_room *
comp_sim_room_create(struct comp_compositor *c)
{
	struct xrt_device *xdev = c->xdev;
	if (xdev == NULL || xdev->hmd == NULL || xdev->hmd->view_count != 2 ||
	    (xdev->compute_distortion == NULL && xdev->hmd->distortion.preferred != XRT_DISTORTION_MODEL_NONE)) {
		COMP_ERROR(c, "Simulated room passthrough needs a stereo HMD with a distortion function");
		return NULL;
	}
	struct comp_sim_room *room = calloc(1, sizeof(*room));
	if (room == NULL) {
		return NULL;
	}
	room->c = c;
	for (uint32_t eye = 0; eye < 2; eye++) {
		if (!create_map(room, eye)) {
			comp_sim_room_destroy(&room);
			return NULL;
		}
	}
	COMP_INFO(c, "Simulated room passthrough: drawn at the timewarp eye poses");
	return room;
}

void
comp_sim_room_destroy(struct comp_sim_room **ptr)
{
	struct comp_sim_room *room = *ptr;
	if (room == NULL) {
		return;
	}
	free(room->maps[0]);
	free(room->maps[1]);
	free(room);
	*ptr = NULL;
}

const float *
comp_sim_room_map(struct comp_sim_room *room, uint32_t eye)
{
	return eye < 2 ? room->maps[eye] : NULL;
}

bool
comp_sim_room_sample(struct comp_sim_room *room,
                     const struct comp_target_image *image,
                     bool has_app,
                     struct render_camera_params *out_params)
{
	(void)room;
	if (!image->display_view_poses_valid) {
		return false;
	}
	memset(out_params, 0, sizeof(*out_params));
	out_params->brightness = 1.0f;
	out_params->has_app = has_app;
	out_params->map_size = COMP_SIM_ROOM_MAP_SIZE;
	out_params->mode = RENDER_CAMERA_MODE_ROOM;
	for (uint32_t eye = 0; eye < 2; eye++) {
		const struct xrt_pose *pose = &image->display_view_poses[eye];
		out_params->views[eye].orientation[0] = pose->orientation.x;
		out_params->views[eye].orientation[1] = pose->orientation.y;
		out_params->views[eye].orientation[2] = pose->orientation.z;
		out_params->views[eye].orientation[3] = pose->orientation.w;
		out_params->views[eye].position[0] = pose->position.x;
		out_params->views[eye].position[1] = pose->position.y;
		out_params->views[eye].position[2] = pose->position.z;
	}
	return true;
}

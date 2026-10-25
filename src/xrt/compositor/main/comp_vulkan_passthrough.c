// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "main/comp_vulkan_passthrough.h"
#include "main/comp_psvr2_passthrough.h"
#include "main/comp_sim_room_passthrough.h"
#include "util/u_desktop_pose.h"
#include "util/u_frame_share.h"
#include "main/comp_compositor.h"
#include "render/render_camera.h"
#include "util/u_timing_trace.h"
#include "os/os_time.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

struct comp_vulkan_passthrough
{
	struct comp_target *ct;
	//! Exactly one source: the PS VR2 cameras or, with SIMULATED_DESKTOP, a room.
	struct comp_psvr2_passthrough *camera;
	struct comp_sim_room *room;
	//! The room told clients (through IPC) that passthrough is available.
	bool room_advertised;
	struct render_camera *renderer;
	FILE *timing;
};

struct comp_vulkan_passthrough *
comp_vulkan_passthrough_create(struct comp_target *ct,
                               struct vk_bundle_queue *queue,
                               bool is_service,
                               const VkImageView *destinations,
                               VkImageLayout destination_layout)
{
	if (ct->semaphores.render_complete_is_timeline || ct->image_count > XRT_MAX_SWAPCHAIN_IMAGES ||
	    destinations == NULL)
		return NULL;
	struct comp_vulkan_passthrough *pt = calloc(1, sizeof(*pt));
	if (pt == NULL)
		return NULL;
	pt->ct = ct;
	if (u_desktop_pose_enabled()) {
		pt->room = comp_sim_room_create(ct->c);
		if (pt->room == NULL)
			goto fail;
		if (is_service) {
			// Lets clients report XR_FB_passthrough support; they draw their own room.
			u_passthrough_share_set_source_available(true);
			pt->room_advertised = true;
		}
	} else {
		pt->camera = comp_psvr2_passthrough_create(ct->c, is_service);
		if (pt->camera == NULL)
			goto fail;
	}
	VkImage images[XRT_MAX_SWAPCHAIN_IMAGES];
	VkImageView views[XRT_MAX_SWAPCHAIN_IMAGES];
	for (uint32_t i = 0; i < ct->image_count; i++) {
		images[i] = ct->images[i].handle;
		views[i] = ct->images[i].view;
	}
	const float *maps[2];
	VkExtent2D camera_extent;
	uint32_t map_size;
	if (pt->room != NULL) {
		maps[0] = comp_sim_room_map(pt->room, 0);
		maps[1] = comp_sim_room_map(pt->room, 1);
		camera_extent = (VkExtent2D){4, 4}; // Unused by the room; keep the images tiny.
		map_size = COMP_SIM_ROOM_MAP_SIZE;
	} else {
		maps[0] = comp_psvr2_passthrough_map(pt->camera, 0);
		maps[1] = comp_psvr2_passthrough_map(pt->camera, 1);
		camera_extent = (VkExtent2D){1024, 1016};
		map_size = 512;
	}
	pt->renderer =
	    render_camera_create(&ct->c->base.vk, queue, ct->c->shaders.fullscreen_vert, ct->c->shaders.camera_frag,
	                         ct->format, (VkExtent2D){ct->width, ct->height}, camera_extent, map_size, maps, images,
	                         views, ct->final_layout, destinations, destination_layout, ct->image_count);
	if (pt->renderer == NULL)
		goto fail;
	if (u_timing_trace_enabled()) {
		pt->timing = u_timing_trace_open("wsi_passthrough", 65536);
		if (pt->timing)
			fprintf(pt->timing, "frame_id,image_index,begin_ns,snapshot_ns,queued_ns,result\n");
	}
	COMP_INFO(ct->c, "Vulkan camera passthrough initialized: %u independent output slots", ct->image_count);
	return pt;
fail:
	(void)comp_vulkan_passthrough_destroy(&pt);
	return NULL;
}

VkResult
comp_vulkan_passthrough_compose(struct comp_vulkan_passthrough *pt, uint32_t index, bool has_app, bool *present_ready)
{
	*present_ready = true; // Before submission the renderer's semaphore is untouched.
	if (index >= pt->ct->image_count)
		return VK_ERROR_INITIALIZATION_FAILED;
	uint64_t begin = pt->timing ? os_monotonic_get_ns() : 0;
	struct comp_target_image *image = &pt->ct->images[index];
	struct comp_psvr2_passthrough_sample sample = {0};
	if (pt->room != NULL) {
		if (!comp_sim_room_sample(pt->room, image, has_app, &sample.params))
			return VK_NOT_READY;
	} else if (!comp_psvr2_passthrough_sample(
	               pt->camera, image->display_head_orientation_valid ? &image->display_head_orientation : NULL,
	               has_app, &sample)) {
		return VK_NOT_READY;
	}
	uint64_t snapshot = pt->timing ? os_monotonic_get_ns() : 0;
	bool submitted = false;
	VkResult ret = render_camera_submit(pt->renderer, index, pt->ct->semaphores.render_complete, sample.frames,
	                                    &sample.params, &submitted);
	comp_psvr2_passthrough_sample_release(&sample);
	if (ret != VK_SUCCESS && (submitted || ret == VK_TIMEOUT || ret == VK_ERROR_DEVICE_LOST))
		*present_ready = false;
	if (pt->timing) {
		fprintf(pt->timing, "%" PRIi64 ",%u,%llu,%llu,%llu,%d\n", pt->ct->c->frame.rendering.id, index,
		        (unsigned long long)begin, (unsigned long long)snapshot,
		        (unsigned long long)os_monotonic_get_ns(), ret);
	}
	return ret;
}

bool
comp_vulkan_passthrough_destroy(struct comp_vulkan_passthrough **ptr)
{
	struct comp_vulkan_passthrough *pt = *ptr;
	if (pt == NULL)
		return true;
	comp_psvr2_passthrough_destroy(&pt->camera); // Detach the source before draining GPU storage.
	comp_sim_room_destroy(&pt->room);
	if (pt->room_advertised) {
		u_passthrough_share_set_source_available(false);
		pt->room_advertised = false;
	}
	VkResult ret = render_camera_destroy(&pt->renderer);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(pt->ct->c, "Camera GPU drain failed (%d); retaining camera storage and output target", ret);
		return false;
	}
	if (pt->timing)
		fclose(pt->timing);
	free(pt);
	*ptr = NULL;
	return true;
}

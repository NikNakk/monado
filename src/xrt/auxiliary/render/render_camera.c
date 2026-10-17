// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/* Stereo camera composition using only Vulkan. No presentation or device policy. */
#include "render/render_camera.h"
#include "render/render_interface.h"
#include "vk/vk_cmd_pool.h"
#include <stdlib.h>
#include <string.h>

#define CAMERA_WAIT_NS 500000000ull
static const VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
static const VkMemoryPropertyFlags host_memory =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

struct camera_image
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
};
struct camera_slot
{
	struct camera_image application, cameras[2];
	struct render_buffer upload[2], params;
	VkImage target;
	VkDescriptorSet descriptors;
	VkCommandBuffer commands;
	VkFence fence;
	bool pending;
};
struct render_camera
{
	struct vk_bundle *vk;
	struct vk_cmd_pool pool;
	bool pool_ready, maps_initialized, failed;
	VkExtent2D extent, camera_extent;
	uint32_t count, map_size;
	struct camera_slot *slots;
	struct camera_image maps[2];
	struct render_buffer map_upload;
	VkSampler sampler;
	VkDescriptorSetLayout set_layout;
	VkDescriptorPool descriptors;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
};

static bool
create_image(
    struct render_camera *rc, struct camera_image *image, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage)
{
	struct vk_bundle *vk = rc->vk;
	return vk_create_image_simple(vk, extent, format, usage, &image->memory, &image->image) == VK_SUCCESS &&
	       vk_create_view(vk, image->image, VK_IMAGE_VIEW_TYPE_2D, format, color_range, &image->view) == VK_SUCCESS;
}

static bool
create_buffer(struct render_camera *rc, struct render_buffer *buffer, VkDeviceSize size, VkBufferUsageFlags usage)
{
	return render_buffer_init(rc->vk, buffer, usage, host_memory, size) == VK_SUCCESS &&
	       render_buffer_map(rc->vk, buffer) == VK_SUCCESS;
}

static void
barrier(struct render_camera *rc,
        VkCommandBuffer cmd,
        VkImage image,
        VkImageLayout old_layout,
        VkImageLayout new_layout,
        VkAccessFlags src,
        VkAccessFlags dst)
{
	vk_cmd_image_barrier_locked(rc->vk, cmd, image, src, dst, old_layout, new_layout,
	                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                            color_range);
}

static void
upload(struct render_camera *rc,
       VkCommandBuffer cmd,
       VkBuffer buffer,
       VkDeviceSize offset,
       VkImage image,
       VkExtent2D extent)
{
	barrier(rc, cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
	        VK_ACCESS_TRANSFER_WRITE_BIT);
	VkBufferImageCopy copy = {
	    .bufferOffset = offset,
	    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .imageExtent = {extent.width, extent.height, 1},
	};
	rc->vk->vkCmdCopyBufferToImage(cmd, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	barrier(rc, cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
}

static bool
supports(struct vk_bundle *vk, VkFormat format, VkFormatFeatureFlags features)
{
	VkFormatProperties properties = {0};
	vk->vkGetPhysicalDeviceFormatProperties(vk->physical_device, format, &properties);
	return (properties.optimalTilingFeatures & features) == features;
}

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
                     uint32_t count)
{
	// These are linear UNORM views, matching the compositor output. SRGB storage
	// images are not required by Vulkan and must not be silently reinterpreted.
	if ((format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_B8G8R8A8_UNORM) || count == 0 ||
	    count > XRT_MAX_SWAPCHAIN_IMAGES || extent.width == 0 || extent.height == 0 || camera_extent.width == 0 ||
	    camera_extent.height == 0 || map_size == 0 || map_size > 4096 || !maps[0] || !maps[1] ||
	    !supports(vk, format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
	    !supports(vk, VK_FORMAT_BC4_UNORM_BLOCK,
	              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
		return NULL;
	}
	struct render_camera *rc = calloc(1, sizeof(*rc));
	if (rc == NULL)
		return NULL;
	rc->vk = vk;
	rc->extent = extent;
	rc->camera_extent = camera_extent;
	rc->count = count;
	rc->map_size = map_size;
	rc->slots = calloc(count, sizeof(*rc->slots));
	if (rc->slots == NULL)
		goto fail;
	if (vk_cmd_pool_init_for_queue(vk, &rc->pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, queue) !=
	    VK_SUCCESS)
		goto fail;
	rc->pool_ready = true;
	if (vk_create_sampler(vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, &rc->sampler) != VK_SUCCESS)
		goto fail;

	VkDescriptorSetLayoutBinding bindings[] = {
	    {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	    {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	    {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	    {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	    {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
	};
	VkDescriptorSetLayoutCreateInfo set_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 5,
	    .pBindings = bindings,
	};
	if (vk->vkCreateDescriptorSetLayout(vk->device, &set_info, NULL, &rc->set_layout) != VK_SUCCESS)
		goto fail;
	VkPipelineLayoutCreateInfo layout_info = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &rc->set_layout,
	};
	if (vk->vkCreatePipelineLayout(vk->device, &layout_info, NULL, &rc->pipeline_layout) != VK_SUCCESS)
		goto fail;
	if (vk_create_compute_pipeline(vk, VK_NULL_HANDLE, shader, rc->pipeline_layout, NULL, &rc->pipeline) !=
	    VK_SUCCESS)
		goto fail;
	VkDescriptorPoolSize sizes[] = {
	    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * count},
	    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, count},
	    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, count},
	};
	if (!vk_init_descriptor_pool(vk, sizes, 3, count, &rc->descriptors))
		goto fail;
	VkDeviceSize map_bytes = (VkDeviceSize)map_size * map_size * 4 * sizeof(float);
	if (!create_buffer(rc, &rc->map_upload, 2 * map_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
		goto fail;
	for (uint32_t eye = 0; eye < 2; eye++) {
		memcpy((char *)rc->map_upload.mapped + eye * map_bytes, maps[eye], map_bytes);
		if (!create_image(rc, &rc->maps[eye], (VkExtent2D){map_size, map_size}, VK_FORMAT_R32G32B32A32_SFLOAT,
		                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
			goto fail;
	}
	VkDeviceSize camera_bytes =
	    (VkDeviceSize)((camera_extent.width + 3) / 4) * ((camera_extent.height + 3) / 4) * 8;
	for (uint32_t i = 0; i < count; i++) {
		struct camera_slot *slot = &rc->slots[i];
		slot->target = images[i];
		if (!create_image(rc, &slot->application, extent, format,
		                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
			goto fail;
		for (uint32_t eye = 0; eye < 2; eye++) {
			if (!create_image(rc, &slot->cameras[eye], camera_extent, VK_FORMAT_BC4_UNORM_BLOCK,
			                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
			    !create_buffer(rc, &slot->upload[eye], camera_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
				goto fail;
		}
		if (!create_buffer(rc, &slot->params, sizeof(struct render_camera_params),
		                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
			goto fail;
		VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (vk->vkCreateFence(vk->device, &fence_info, NULL, &slot->fence) != VK_SUCCESS ||
		    vk_cmd_pool_create_cmd_buffer(vk, &rc->pool, &slot->commands) != VK_SUCCESS ||
		    !vk_allocate_descriptor_sets(vk, rc->descriptors, 1, &rc->set_layout, &slot->descriptors))
			goto fail;
		VkDescriptorImageInfo app = {rc->sampler, slot->application.view,
		                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorImageInfo camera[2], mapping[2];
		for (uint32_t eye = 0; eye < 2; eye++) {
			camera[eye] = (VkDescriptorImageInfo){rc->sampler, slot->cameras[eye].view,
			                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			mapping[eye] = (VkDescriptorImageInfo){rc->sampler, rc->maps[eye].view,
			                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		}
		VkDescriptorImageInfo target = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
		VkDescriptorBufferInfo params = {slot->params.buffer, 0, sizeof(struct render_camera_params)};
		VkWriteDescriptorSet writes[] = {
		    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		     .dstSet = slot->descriptors,
		     .dstBinding = 0,
		     .descriptorCount = 1,
		     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     .pImageInfo = &app},
		    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		     .dstSet = slot->descriptors,
		     .dstBinding = 1,
		     .descriptorCount = 2,
		     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     .pImageInfo = camera},
		    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		     .dstSet = slot->descriptors,
		     .dstBinding = 2,
		     .descriptorCount = 2,
		     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		     .pImageInfo = mapping},
		    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		     .dstSet = slot->descriptors,
		     .dstBinding = 3,
		     .descriptorCount = 1,
		     .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
		     .pImageInfo = &target},
		    {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		     .dstSet = slot->descriptors,
		     .dstBinding = 4,
		     .descriptorCount = 1,
		     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
		     .pBufferInfo = &params},
		};
		vk->vkUpdateDescriptorSets(vk->device, 5, writes, 0, NULL);
	}
	return rc;
fail:
	(void)render_camera_destroy(&rc);
	return NULL;
}

VkResult
render_camera_submit(struct render_camera *rc,
                     uint32_t index,
                     VkImageLayout layout,
                     VkSemaphore render_complete,
                     struct xrt_frame *frames[2],
                     const struct render_camera_params *params,
                     bool *submitted)
{
	*submitted = false;
	if (index >= rc->count || rc->failed || params->map_size != rc->map_size)
		return VK_ERROR_INITIALIZATION_FAILED;
	struct vk_bundle *vk = rc->vk;
	struct camera_slot *slot = &rc->slots[index];
	if (slot->pending) {
		VkResult ret = vk->vkWaitForFences(vk->device, 1, &slot->fence, VK_TRUE, CAMERA_WAIT_NS);
		if (ret != VK_SUCCESS)
			return ret;
		slot->pending = false;
	}
	uint32_t rows = (rc->camera_extent.height + 3) / 4;
	size_t row_bytes = ((rc->camera_extent.width + 3) / 4) * 8;
	for (uint32_t eye = 0; eye < 2; eye++) {
		struct xrt_frame *frame = frames[eye];
		if (frame == NULL || frame->data == NULL || frame->format != XRT_FORMAT_BC4 ||
		    frame->width != rc->camera_extent.width || frame->height != rc->camera_extent.height ||
		    frame->stride < row_bytes || frame->size < (rows - 1) * frame->stride + row_bytes)
			return VK_ERROR_INITIALIZATION_FAILED;
	}
	for (uint32_t eye = 0; eye < 2; eye++) {
		for (uint32_t row = 0; row < rows; row++) {
			memcpy((char *)slot->upload[eye].mapped + row * row_bytes,
			       frames[eye]->data + row * frames[eye]->stride, row_bytes);
		}
	}
	memcpy(slot->params.mapped, params, sizeof(*params));
	vk_cmd_pool_lock(&rc->pool);
	// The reset-capable pool permits implicit reset at begin, after the slot
	// fence above has proved that the previous recording is no longer pending.
	VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	                                  .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	VkResult ret = vk->vkBeginCommandBuffer(slot->commands, &begin);
	if (ret != VK_SUCCESS)
		goto done;
	VkCommandBuffer cmd = slot->commands;
	if (!rc->maps_initialized) {
		VkDeviceSize bytes = (VkDeviceSize)rc->map_size * rc->map_size * 4 * sizeof(float);
		for (uint32_t eye = 0; eye < 2; eye++)
			upload(rc, cmd, rc->map_upload.buffer, eye * bytes, rc->maps[eye].image,
			       (VkExtent2D){rc->map_size, rc->map_size});
	}
	for (uint32_t eye = 0; eye < 2; eye++)
		upload(rc, cmd, slot->upload[eye].buffer, 0, slot->cameras[eye].image, rc->camera_extent);
	// Copy before overwriting: the application image and camera output must
	// never alias in a shader invocation. The copy belongs to this output slot.
	barrier(rc, cmd, slot->target, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
	        VK_ACCESS_TRANSFER_READ_BIT);
	barrier(rc, cmd, slot->application.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
	        VK_ACCESS_TRANSFER_WRITE_BIT);
	VkImageCopy copy = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .extent = {rc->extent.width, rc->extent.height, 1},
	};
	vk->vkCmdCopyImage(cmd, slot->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot->application.image,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	barrier(rc, cmd, slot->application.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	barrier(rc, cmd, slot->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
	        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT);
	vk->vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rc->pipeline);
	vk->vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, rc->pipeline_layout, 0, 1, &slot->descriptors,
	                            0, NULL);
	vk->vkCmdDispatch(cmd, (rc->extent.width + 7) / 8, (rc->extent.height + 7) / 8, 1);
	barrier(rc, cmd, slot->target, VK_IMAGE_LAYOUT_GENERAL, layout, VK_ACCESS_SHADER_WRITE_BIT,
	        VK_ACCESS_MEMORY_READ_BIT);
	ret = vk->vkEndCommandBuffer(cmd);
	if (ret != VK_SUCCESS)
		goto done;
	ret = vk->vkResetFences(vk->device, 1, &slot->fence);
	if (ret != VK_SUCCESS)
		goto done;
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	// ALL_COMMANDS consumes the incoming binary signal before this batch
	// writes the image; the outgoing signal happens after all of its commands.
	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = 1,
	    .pWaitSemaphores = &render_complete,
	    .pWaitDstStageMask = &stage,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	    .signalSemaphoreCount = 1,
	    .pSignalSemaphores = &render_complete,
	};
	vk_queue_lock(rc->pool.queue);
	ret = vk->vkQueueSubmit(rc->pool.queue->queue, 1, &submit, slot->fence);
	vk_queue_unlock(rc->pool.queue);
	if (ret == VK_SUCCESS) {
		slot->pending = true;
		*submitted = true;
		rc->maps_initialized = true;
	} else if (ret != VK_ERROR_OUT_OF_HOST_MEMORY && ret != VK_ERROR_OUT_OF_DEVICE_MEMORY) {
		// OOM submissions leave resources and synchronization unchanged by
		// Vulkan's contract. Other failures are terminal: retain uncertain work.
		slot->pending = true;
		*submitted = true;
		rc->failed = true;
	}
done:
	vk_cmd_pool_unlock(&rc->pool);
	return ret;
}

static void
free_image(struct vk_bundle *vk, struct camera_image *image)
{
	if (image->view)
		vk->vkDestroyImageView(vk->device, image->view, NULL);
	if (image->image)
		vk->vkDestroyImage(vk->device, image->image, NULL);
	if (image->memory)
		vk->vkFreeMemory(vk->device, image->memory, NULL);
}

VkResult
render_camera_destroy(struct render_camera **ptr)
{
	struct render_camera *rc = *ptr;
	if (rc == NULL)
		return VK_SUCCESS;
	struct vk_bundle *vk = rc->vk;
	// One bounded wait for every in-flight image, not a fresh timeout per image.
	VkFence fences[XRT_MAX_SWAPCHAIN_IMAGES];
	uint32_t count = 0;
	for (uint32_t i = 0; rc->slots != NULL && i < rc->count; i++)
		if (rc->slots[i].pending)
			fences[count++] = rc->slots[i].fence;
	if (count != 0) {
		VkResult ret = vk->vkWaitForFences(vk->device, count, fences, VK_TRUE, CAMERA_WAIT_NS);
		if (ret != VK_SUCCESS)
			return ret;
	}
	for (uint32_t i = 0; rc->slots != NULL && i < rc->count; i++) {
		struct camera_slot *slot = &rc->slots[i];
		if (slot->fence)
			vk->vkDestroyFence(vk->device, slot->fence, NULL);
		free_image(vk, &slot->application);
		for (uint32_t eye = 0; eye < 2; eye++) {
			free_image(vk, &slot->cameras[eye]);
			render_buffer_fini(vk, &slot->upload[eye]);
		}
		render_buffer_fini(vk, &slot->params);
	}
	if (rc->pool_ready)
		vk_cmd_pool_destroy(vk, &rc->pool);
	if (rc->pipeline)
		vk->vkDestroyPipeline(vk->device, rc->pipeline, NULL);
	if (rc->pipeline_layout)
		vk->vkDestroyPipelineLayout(vk->device, rc->pipeline_layout, NULL);
	if (rc->descriptors)
		vk->vkDestroyDescriptorPool(vk->device, rc->descriptors, NULL);
	if (rc->set_layout)
		vk->vkDestroyDescriptorSetLayout(vk->device, rc->set_layout, NULL);
	if (rc->sampler)
		vk->vkDestroySampler(vk->device, rc->sampler, NULL);
	for (uint32_t eye = 0; eye < 2; eye++)
		free_image(vk, &rc->maps[eye]);
	render_buffer_fini(vk, &rc->map_upload);
	free(rc->slots);
	free(rc);
	*ptr = NULL;
	return VK_SUCCESS;
}

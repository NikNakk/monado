// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/* Keep shader-write usage off presentation images, using only Vulkan. */
#include "render_output.h"
#include "vk/vk_cmd_pool.h"
#include <stdlib.h>

#define OUTPUT_WAIT_NS 500000000ull
static const VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

struct output_slot
{
	VkImage image, destination;
	VkDeviceMemory memory;
	VkImageView view;
	VkCommandBuffer commands;
	VkFence fence;
	bool pending;
};

struct render_output
{
	struct vk_bundle *vk;
	struct vk_cmd_pool pool;
	bool pool_ready, failed;
	VkExtent2D extent;
	uint32_t count;
	struct output_slot *slots;
};

struct render_output *
render_output_create(struct vk_bundle *vk,
                     struct vk_bundle_queue *queue,
                     VkFormat format,
                     VkExtent2D extent,
                     VkImageUsageFlags render_usage,
                     const VkImage *destinations,
                     uint32_t count)
{
	if (count == 0 || count > XRT_MAX_SWAPCHAIN_IMAGES || extent.width == 0 || extent.height == 0 ||
	    destinations == NULL)
		return NULL;
	struct render_output *output = calloc(1, sizeof(*output));
	if (output == NULL)
		return NULL;
	output->vk = vk;
	output->extent = extent;
	output->count = count;
	output->slots = calloc(count, sizeof(*output->slots));
	if (output->slots == NULL)
		goto fail;
	if (vk_cmd_pool_init_for_queue(vk, &output->pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, queue) !=
	    VK_SUCCESS)
		goto fail;
	output->pool_ready = true;
	for (uint32_t i = 0; i < count; i++) {
		struct output_slot *slot = &output->slots[i];
		slot->destination = destinations[i];
		if (slot->destination == VK_NULL_HANDLE ||
		    vk_create_image_simple(vk, extent, format, render_usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		                           &slot->memory, &slot->image) != VK_SUCCESS ||
		    vk_create_view(vk, slot->image, VK_IMAGE_VIEW_TYPE_2D, format, color_range, &slot->view) !=
		        VK_SUCCESS)
			goto fail;
		VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (vk->vkCreateFence(vk->device, &fence, NULL, &slot->fence) != VK_SUCCESS)
			goto fail;
		vk_cmd_pool_lock(&output->pool);
		VkResult ret = vk_cmd_pool_create_and_begin_cmd_buffer_locked(
		    vk, &output->pool, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &slot->commands);
		if (ret == VK_SUCCESS)
			ret = vk->vkEndCommandBuffer(slot->commands);
		vk_cmd_pool_unlock(&output->pool);
		if (ret != VK_SUCCESS)
			goto fail;
	}
	return output;
fail:
	(void)render_output_destroy(&output);
	return NULL;
}

VkImage
render_output_image(struct render_output *output, uint32_t index)
{
	return index < output->count ? output->slots[index].image : VK_NULL_HANDLE;
}

VkImageView
render_output_view(struct render_output *output, uint32_t index)
{
	return index < output->count ? output->slots[index].view : VK_NULL_HANDLE;
}

static void
barrier(struct render_output *output,
        VkCommandBuffer cmd,
        VkImage image,
        VkImageLayout before,
        VkImageLayout after,
        VkAccessFlags src,
        VkAccessFlags dst)
{
	vk_cmd_image_barrier_locked(output->vk, cmd, image, src, dst, before, after, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
	                            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, color_range);
}

VkResult
render_output_copy(struct render_output *output, uint32_t index, VkSemaphore complete, VkImageLayout destination_layout)
{
	if (index >= output->count || complete == VK_NULL_HANDLE || output->failed)
		return VK_ERROR_INITIALIZATION_FAILED;
	struct vk_bundle *vk = output->vk;
	struct output_slot *slot = &output->slots[index];
	if (slot->pending) {
		VkResult ret = vk->vkWaitForFences(vk->device, 1, &slot->fence, VK_TRUE, OUTPUT_WAIT_NS);
		if (ret != VK_SUCCESS)
			return ret;
		slot->pending = false;
	}
	VkCommandBuffer cmd = slot->commands;
	VkCommandBufferBeginInfo begin = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk_cmd_pool_lock(&output->pool);
	VkResult ret = vk->vkBeginCommandBuffer(cmd, &begin);
	if (ret != VK_SUCCESS)
		goto done;
	barrier(output, cmd, slot->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	barrier(output, cmd, slot->destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
	        VK_ACCESS_TRANSFER_WRITE_BIT);
	VkImageCopy copy = {
	    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
	    .extent = {output->extent.width, output->extent.height, 1},
	};
	vk->vkCmdCopyImage(cmd, slot->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot->destination,
	                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	barrier(output, cmd, slot->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
	        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	barrier(output, cmd, slot->destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, destination_layout,
	        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
	ret = vk->vkEndCommandBuffer(cmd);
	if (ret != VK_SUCCESS)
		goto done;
	ret = vk->vkResetFences(vk->device, 1, &slot->fence);
	if (ret != VK_SUCCESS)
		goto done;
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = 1,
	    .pWaitSemaphores = &complete,
	    .pWaitDstStageMask = &stage,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	    .signalSemaphoreCount = 1,
	    .pSignalSemaphores = &complete,
	};
	vk_queue_lock(output->pool.queue);
	ret = vk->vkQueueSubmit(output->pool.queue->queue, 1, &submit, slot->fence);
	vk_queue_unlock(output->pool.queue);
	if (ret == VK_SUCCESS) {
		slot->pending = true;
	} else {
		output->failed = true;
		// OOM leaves synchronization unchanged. Retain uncertain submissions.
		slot->pending = ret != VK_ERROR_OUT_OF_HOST_MEMORY && ret != VK_ERROR_OUT_OF_DEVICE_MEMORY;
	}
done:
	vk_cmd_pool_unlock(&output->pool);
	return ret;
}

VkResult
render_output_destroy(struct render_output **ptr)
{
	struct render_output *output = *ptr;
	if (output == NULL)
		return VK_SUCCESS;
	struct vk_bundle *vk = output->vk;
	for (uint32_t i = 0; output->slots != NULL && i < output->count; i++) {
		struct output_slot *slot = &output->slots[i];
		if (!slot->pending)
			continue;
		VkResult ret = vk->vkWaitForFences(vk->device, 1, &slot->fence, VK_TRUE, OUTPUT_WAIT_NS);
		if (ret != VK_SUCCESS)
			return ret;
		slot->pending = false;
	}
	for (uint32_t i = 0; output->slots != NULL && i < output->count; i++) {
		struct output_slot *slot = &output->slots[i];
		if (slot->fence)
			vk->vkDestroyFence(vk->device, slot->fence, NULL);
		if (slot->view)
			vk->vkDestroyImageView(vk->device, slot->view, NULL);
		if (slot->image)
			vk->vkDestroyImage(vk->device, slot->image, NULL);
		if (slot->memory)
			vk->vkFreeMemory(vk->device, slot->memory, NULL);
	}
	if (output->pool_ready)
		vk_cmd_pool_destroy(vk, &output->pool);
	free(output->slots);
	free(output);
	*ptr = NULL;
	return VK_SUCCESS;
}

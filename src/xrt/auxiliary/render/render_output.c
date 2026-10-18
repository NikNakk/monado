// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/* Keep shader-write and transfer usage off presentation images, using only Vulkan. */
#include "render_output.h"
#include "render/render_fullscreen.h"
#include "vk/vk_cmd_pool.h"
#include <stdlib.h>

#define OUTPUT_WAIT_NS 500000000ull
static const VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

struct output_slot
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkDescriptorSet descriptors;
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
	struct render_fullscreen pass;
	VkSampler sampler;
	VkDescriptorSetLayout set_layout;
	VkDescriptorPool descriptors;
};

struct render_output *
render_output_create(struct vk_bundle *vk,
                     struct vk_bundle_queue *queue,
                     VkShaderModule vert,
                     VkShaderModule frag,
                     VkFormat format,
                     VkExtent2D extent,
                     VkImageUsageFlags render_usage,
                     const VkImageView *destinations,
                     VkImageLayout destination_layout,
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
	if (vk_create_sampler(vk, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, &output->sampler) != VK_SUCCESS)
		goto fail;
	VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
	                                        VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
	VkDescriptorSetLayoutCreateInfo set_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 1,
	    .pBindings = &binding,
	};
	if (vk->vkCreateDescriptorSetLayout(vk->device, &set_info, NULL, &output->set_layout) != VK_SUCCESS)
		goto fail;
	if (render_fullscreen_init(vk, &output->pass, vert, frag, output->set_layout, format, destination_layout) !=
	    VK_SUCCESS)
		goto fail;
	VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count};
	if (!vk_init_descriptor_pool(vk, &size, 1, count, &output->descriptors))
		goto fail;
	for (uint32_t i = 0; i < count; i++) {
		struct output_slot *slot = &output->slots[i];
		if (destinations[i] == VK_NULL_HANDLE ||
		    vk_create_image_simple(vk, extent, format, render_usage | VK_IMAGE_USAGE_SAMPLED_BIT, &slot->memory,
		                           &slot->image) != VK_SUCCESS ||
		    vk_create_view(vk, slot->image, VK_IMAGE_VIEW_TYPE_2D, format, color_range, &slot->view) !=
		        VK_SUCCESS ||
		    render_fullscreen_framebuffer(vk, &output->pass, destinations[i], extent, &slot->framebuffer) !=
		        VK_SUCCESS ||
		    !vk_allocate_descriptor_sets(vk, output->descriptors, 1, &output->set_layout, &slot->descriptors))
			goto fail;
		VkDescriptorImageInfo source = {output->sampler, slot->view, VK_IMAGE_LAYOUT_GENERAL};
		VkWriteDescriptorSet write = {
		    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		    .dstSet = slot->descriptors,
		    .dstBinding = 0,
		    .descriptorCount = 1,
		    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		    .pImageInfo = &source,
		};
		vk->vkUpdateDescriptorSets(vk->device, 1, &write, 0, NULL);
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
render_output_present(struct render_output *output, uint32_t index, VkSemaphore complete)
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
	// The private image stays in GENERAL; order earlier shader/transfer writes
	// before sampling, and this read before the next frame's writes.
	barrier(output, cmd, slot->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_WRITE_BIT,
	        VK_ACCESS_SHADER_READ_BIT);
	render_fullscreen_draw(vk, &output->pass, cmd, slot->framebuffer, output->extent, slot->descriptors);
	barrier(output, cmd, slot->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
	        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
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
		if (slot->framebuffer)
			vk->vkDestroyFramebuffer(vk->device, slot->framebuffer, NULL);
		if (slot->view)
			vk->vkDestroyImageView(vk->device, slot->view, NULL);
		if (slot->image)
			vk->vkDestroyImage(vk->device, slot->image, NULL);
		if (slot->memory)
			vk->vkFreeMemory(vk->device, slot->memory, NULL);
	}
	if (output->pool_ready)
		vk_cmd_pool_destroy(vk, &output->pool);
	render_fullscreen_fini(vk, &output->pass);
	if (output->descriptors)
		vk->vkDestroyDescriptorPool(vk->device, output->descriptors, NULL);
	if (output->set_layout)
		vk->vkDestroyDescriptorSetLayout(vk->device, output->set_layout, NULL);
	if (output->sampler)
		vk->vkDestroySampler(vk->device, output->sampler, NULL);
	free(output->slots);
	free(output);
	*ptr = NULL;
	return VK_SUCCESS;
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "main/comp_macos_wsi_passthrough.h"
#include "main/comp_macos_passthrough.h"
#include "main/comp_compositor.h"
#include "vk/vk_cmd_pool.h"
#include <stdlib.h>
#include "util/u_debug.h"

DEBUG_GET_ONCE_BOOL_OPTION(wsi_pt_verify, "XRT_MACOS_WSI_PASSTHROUGH_VERIFY", false)

struct comp_macos_wsi_passthrough
{
	struct comp_target *ct;
	struct comp_macos_passthrough *camera;
	struct vk_cmd_pool pool;
	VkImageView *views;
	uint32_t count;
	id<MTLCommandQueue> metal_queue;
	id<MTLTexture> app_copy;
	bool verified[2];
};

static MTLPixelFormat
metal_format(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
	case VK_FORMAT_B8G8R8A8_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
	case VK_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
	case VK_FORMAT_R8G8B8A8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
	default: return MTLPixelFormatInvalid;
	}
}

// Consume rendering completion before Metal access; re-signal after Metal and
// layout restoration so the standard Vulkan presenter retains its normal wait.
static VkResult
transition(struct comp_macos_wsi_passthrough *pt, uint32_t index, bool to_metal)
{
	struct comp_target *ct = pt->ct;
	struct vk_bundle *vk = &ct->c->base.vk;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	vk_cmd_pool_lock(&pt->pool);
	VkResult ret = vk_cmd_pool_create_and_begin_cmd_buffer_locked(
	    vk, &pt->pool, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &cmd);
	if (ret != VK_SUCCESS) {
		goto done;
	}
	VkImageMemoryBarrier barrier = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
	    .dstAccessMask = to_metal ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0,
	    .oldLayout = to_metal ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL,
	    .newLayout = to_metal ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = ct->images[index].handle,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	};
	vk->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
	                         NULL, 0, NULL, 1, &barrier);
	ret = vk->vkEndCommandBuffer(cmd);
	if (ret != VK_SUCCESS) {
		goto done;
	}
	VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	ret = vk->vkCreateFence(vk->device, &fence_info, NULL, &fence);
	if (ret != VK_SUCCESS) {
		goto done;
	}
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .waitSemaphoreCount = to_metal ? 1 : 0,
	    .pWaitSemaphores = &ct->semaphores.render_complete,
	    .pWaitDstStageMask = &stage,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd,
	    .signalSemaphoreCount = to_metal ? 0 : 1,
	    .pSignalSemaphores = &ct->semaphores.render_complete,
	};
	vk_queue_lock(pt->pool.queue);
	ret = vk->vkQueueSubmit(pt->pool.queue->queue, 1, &submit, fence);
	vk_queue_unlock(pt->pool.queue);
	if (ret == VK_SUCCESS) {
		ret = vk->vkWaitForFences(vk->device, 1, &fence, VK_TRUE, UINT64_MAX);
	}
done:
	if (fence != VK_NULL_HANDLE) {
		vk->vkDestroyFence(vk->device, fence, NULL);
	}
	if (cmd != VK_NULL_HANDLE) {
		vk->vkFreeCommandBuffers(vk->device, pt->pool.pool, 1, &cmd);
	}
	vk_cmd_pool_unlock(&pt->pool);
	return ret;
}

struct comp_macos_wsi_passthrough *
comp_macos_wsi_passthrough_create(struct comp_target *ct,
                                  struct vk_bundle_queue *queue,
                                  id<MTLDevice> device,
                                  bool is_service)
{
	struct vk_bundle *vk = &ct->c->base.vk;
	MTLPixelFormat format = metal_format(ct->format);
	if (!vk->has_EXT_metal_objects || vk->vkExportMetalObjectsEXT == NULL || format == MTLPixelFormatInvalid ||
	    ct->semaphores.render_complete_is_timeline) {
		return NULL;
	}
	struct comp_macos_wsi_passthrough *pt = calloc(1, sizeof(*pt));
	if (pt == NULL) {
		return NULL;
	}
	pt->ct = ct;
	pt->camera = comp_macos_passthrough_create(ct->c, device, format, is_service);
	if (pt->camera == NULL) {
		goto fail;
	}
	if (vk_cmd_pool_init_for_queue(vk, &pt->pool, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, queue) != VK_SUCCESS) {
		goto fail;
	}
	pt->metal_queue = [device newCommandQueue];
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
	                                                                                width:ct->width
	                                                                               height:ct->height
	                                                                            mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModePrivate;
	pt->app_copy = [device newTextureWithDescriptor:desc];
	pt->count = ct->image_count;
	pt->views = calloc(pt->count, sizeof(*pt->views));
	if (pt->metal_queue == nil || pt->app_copy == nil || pt->views == NULL) {
		goto fail;
	}
	for (uint32_t i = 0; i < pt->count; ++i) {
		// Export from an explicitly exportable view: swapchain images themselves
		// cannot carry VkExportMetalObjectCreateInfoEXT at image creation.
		VkExportMetalObjectCreateInfoEXT export = {
		    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
		    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
		};
		VkImageViewCreateInfo view = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .pNext = &export,
		    .image = ct->images[i].handle,
		    .viewType = VK_IMAGE_VIEW_TYPE_2D,
		    .format = ct->format,
		    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
		};
		if (vk->vkCreateImageView(vk->device, &view, NULL, &pt->views[i]) != VK_SUCCESS) {
			goto fail;
		}
	}
	return pt;
fail:
	comp_macos_wsi_passthrough_destroy(&pt);
	return NULL;
}

static bool
verify_pixels(struct comp_macos_wsi_passthrough *pt, id<MTLTexture> target, bool has_app)
{
	NSUInteger stride = (pt->ct->width * 4 + 255) & ~(NSUInteger)255;
	NSUInteger size = stride * pt->ct->height;
	id<MTLDevice> device = [target device];
	id<MTLBuffer> output = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
	id<MTLBuffer> source = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
	if (output == nil || source == nil) {
		[output release];
		[source release];
		return false;
	}
	id<MTLCommandBuffer> commands = [pt->metal_queue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
	if (blit == nil) {
		[output release];
		[source release];
		return false;
	}
	MTLSize extent = MTLSizeMake(pt->ct->width, pt->ct->height, 1);
	[blit copyFromTexture:target
	                 sourceSlice:0
	                 sourceLevel:0
	                sourceOrigin:MTLOriginMake(0, 0, 0)
	                  sourceSize:extent
	                    toBuffer:output
	           destinationOffset:0
	      destinationBytesPerRow:stride
	    destinationBytesPerImage:size];
	[blit copyFromTexture:pt->app_copy
	                 sourceSlice:0
	                 sourceLevel:0
	                sourceOrigin:MTLOriginMake(0, 0, 0)
	                  sourceSize:extent
	                    toBuffer:source
	           destinationOffset:0
	      destinationBytesPerRow:stride
	    destinationBytesPerImage:size];
	[blit endEncoding];
	[commands commit];
	[commands waitUntilCompleted];
	uint64_t sampled = 0, bad_alpha = 0, bad_opaque = 0, opaque = 0, bad_gray = 0, nonblack = 0;
	bool completed = commands.status == MTLCommandBufferStatusCompleted;
	if (completed) {
		const uint8_t *out = [output contents], *in = [source contents];
		for (uint32_t y = 0; y < pt->ct->height; y += 37) {
			for (uint32_t x = 0; x < pt->ct->width; x += 37) {
				NSUInteger i = y * stride + x * 4;
				sampled++;
				bad_alpha += out[i + 3] != 255;
				nonblack += out[i] != 0 || out[i + 1] != 0 || out[i + 2] != 0;
				if (!has_app) {
					bad_gray += out[i] != out[i + 1] || out[i] != out[i + 2];
				} else if (in[i + 3] == 255) {
					opaque++;
					for (uint32_t c = 0; c < 3; ++c) {
						bad_opaque += abs((int)out[i + c] - (int)in[i + c]) > 1;
					}
				}
			}
		}
	}
	COMP_INFO(pt->ct->c,
	          "WSI passthrough readback: samples=%llu opaque=%llu nonblack=%llu bad_alpha=%llu bad_opaque=%llu "
	          "bad_gray=%llu completed=%d",
	          (unsigned long long)sampled, (unsigned long long)opaque, (unsigned long long)nonblack,
	          (unsigned long long)bad_alpha, (unsigned long long)bad_opaque, (unsigned long long)bad_gray,
	          completed);
	[output release];
	[source release];
	return completed && sampled > 0 && (!has_app || opaque > 0) && bad_alpha == 0 && bad_opaque == 0 &&
	       bad_gray == 0;
}

VkResult
comp_macos_wsi_passthrough_compose(struct comp_macos_wsi_passthrough *pt,
                                   uint32_t index,
                                   bool has_app,
                                   bool *present_ready)
{
	*present_ready = false;
	if (pt == NULL || index >= pt->count) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (!comp_macos_passthrough_ready(pt->camera)) {
		*present_ready = true;
		return VK_NOT_READY;
	}
	struct vk_bundle *vk = &pt->ct->c->base.vk;
	VkResult ret = transition(pt, index, true);
	if (ret != VK_SUCCESS) {
		return ret;
	}
	VkExportMetalTextureInfoEXT texture = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
	    .imageView = pt->views[index],
	    .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
	};
	VkExportMetalObjectsInfoEXT export = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	                                      .pNext = &texture};
	vk->vkExportMetalObjectsEXT(vk->device, &export);
	id<MTLTexture> target = texture.mtlTexture;
	id<MTLCommandBuffer> commands = nil;
	id<MTLBlitCommandEncoder> blit = nil;
	bool encoded = false;
	VkResult restored;
	if (target == nil) {
		ret = VK_ERROR_INITIALIZATION_FAILED;
		goto restore;
	}
	commands = [pt->metal_queue commandBuffer];
	blit = [commands blitCommandEncoder];
	if (commands == nil || blit == nil) {
		ret = VK_ERROR_OUT_OF_HOST_MEMORY;
		goto restore;
	}
	[blit copyFromTexture:target
	          sourceSlice:0
	          sourceLevel:0
	         sourceOrigin:MTLOriginMake(0, 0, 0)
	           sourceSize:MTLSizeMake(pt->ct->width, pt->ct->height, 1)
	            toTexture:pt->app_copy
	     destinationSlice:0
	     destinationLevel:0
	    destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];
	encoded = comp_macos_passthrough_encode(pt->camera, commands, target, pt->app_copy, has_app);
	[commands commit];
	[commands waitUntilCompleted];
	if (commands.status != MTLCommandBufferStatusCompleted) {
		COMP_ERROR(pt->ct->c, "WSI passthrough Metal command failed: %s",
		           [[commands.error localizedDescription] UTF8String]);
		ret = VK_ERROR_DEVICE_LOST;
		goto restore;
	}
	if (!encoded) {
		ret = VK_ERROR_INITIALIZATION_FAILED;
		goto restore;
	}
	if (!pt->verified[has_app ? 1 : 0] && debug_get_bool_option_wsi_pt_verify()) {
		if (!verify_pixels(pt, target, has_app)) {
			COMP_ERROR(pt->ct->c, "WSI passthrough pixel verification failed");
			ret = VK_ERROR_VALIDATION_FAILED_EXT;
			goto restore;
		}
		pt->verified[has_app ? 1 : 0] = true;
	}
restore:
	// Even a failed export, Metal command or verification owns an acquired image.
	// Re-establish the present layout and signal the semaphore consumed above.
	restored = transition(pt, index, false);
	*present_ready = restored == VK_SUCCESS;
	return restored == VK_SUCCESS ? ret : restored;
}

void
comp_macos_wsi_passthrough_destroy(struct comp_macos_wsi_passthrough **ptr)
{
	struct comp_macos_wsi_passthrough *pt = *ptr;
	if (pt == NULL) {
		return;
	}
	struct vk_bundle *vk = &pt->ct->c->base.vk;
	comp_macos_passthrough_destroy(&pt->camera);
	if (pt->views != NULL) {
		for (uint32_t i = 0; i < pt->count; ++i) {
			if (pt->views[i] != VK_NULL_HANDLE) {
				vk->vkDestroyImageView(vk->device, pt->views[i], NULL);
			}
		}
		free(pt->views);
	}
	if (pt->pool.pool != VK_NULL_HANDLE) {
		vk_cmd_pool_destroy(vk, &pt->pool);
	}
	[pt->app_copy release];
	[pt->metal_queue release];
	free(pt);
	*ptr = NULL;
}

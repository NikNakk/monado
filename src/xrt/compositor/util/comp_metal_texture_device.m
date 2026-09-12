// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Helpers for making shared Metal textures compatible with a Vulkan device.
 * @ingroup comp_util
 */

#import <Metal/Metal.h>

#include "util/comp_metal_texture_device.h"
#include "util/u_logging.h"
#include "vk/vk_helpers.h"

bool
comp_metal_texture_prepare_for_vk_device(struct vk_bundle *vk,
                                         void *source_texture,
                                         void **out_texture,
                                         bool *out_needs_release)
{
	if (vk == NULL || source_texture == NULL || out_texture == NULL || out_needs_release == NULL ||
	    vk->vkExportMetalObjectsEXT == NULL) {
		return false;
	}

	*out_texture = NULL;
	*out_needs_release = false;

	VkExportMetalDeviceInfoEXT device_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT,
	};
	VkExportMetalObjectsInfoEXT objects_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &device_info,
	};
	vk->vkExportMetalObjectsEXT(vk->device, &objects_info);

	id<MTLDevice> vk_device = (__bridge id<MTLDevice>)device_info.mtlDevice;
	id<MTLTexture> source = (__bridge id<MTLTexture>)source_texture;
	if (vk_device == nil || source == nil) {
		U_LOG_E("Metal texture device normalization failed: vk_device=%p source=%p",
		        (__bridge void *)vk_device,
		        source_texture);
		return false;
	}

	if (source.device == vk_device) {
		*out_texture = source_texture;
		return true;
	}

	@autoreleasepool {
		MTLSharedTextureHandle *handle = [source newSharedTextureHandle];
		if (handle == nil) {
			U_LOG_E("Metal texture device normalization could not create shared handle: source=%p source_device=%p vk_device=%p",
			        source_texture,
			        (__bridge void *)source.device,
			        (__bridge void *)vk_device);
			return false;
		}

		id<MTLTexture> rebound = [vk_device newSharedTextureWithHandle:handle];
		[handle release];
		if (rebound == nil) {
			U_LOG_E("Metal texture device normalization could not reopen shared texture on Vulkan MTLDevice: source=%p source_device=%p vk_device=%p",
			        source_texture,
			        (__bridge void *)source.device,
			        (__bridge void *)vk_device);
			return false;
		}

		if (rebound.width != source.width || rebound.height != source.height ||
		    rebound.arrayLength != source.arrayLength || rebound.mipmapLevelCount != source.mipmapLevelCount ||
		    rebound.sampleCount != source.sampleCount || rebound.pixelFormat != source.pixelFormat ||
		    rebound.textureType != source.textureType) {
			U_LOG_E("Metal texture device normalization changed texture geometry/format: source=%p rebound=%p",
			        source_texture,
			        (__bridge void *)rebound);
			[rebound release];
			return false;
		}

		U_LOG_I("Metal shared texture rebound to Vulkan MTLDevice: source=%p source_device=%p rebound=%p vk_device=%p",
		        source_texture,
		        (__bridge void *)source.device,
		        (__bridge void *)rebound,
		        (__bridge void *)vk_device);

		*out_texture = (__bridge void *)rebound;
		*out_needs_release = true;
		return true;
	}
}

void
comp_metal_texture_finish_for_vk_device(void *texture, bool needs_release)
{
	if (!needs_release || texture == NULL) {
		return;
	}

	id<MTLTexture> metal_texture = (__bridge id<MTLTexture>)texture;
	[metal_texture release];
}

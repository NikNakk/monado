// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental Metal-first array swapchain backing for macOS.
 * @author OpenAI
 * @ingroup comp_client
 *
 * This deliberately intercepts the in-process native swapchain creation used
 * by the Metal client. The ordinary compositor swapchain is created first so
 * all of Monado's bookkeeping remains unchanged, then each layered VkImage is
 * replaced before the swapchain is exposed to the application:
 *
 *   MTLTextureType2DArray -> VkImportMetalTextureInfoEXT -> VkImage
 *
 * The normal comp_metal_client array export then obtains that same MTLTexture
 * again through vkExportMetalObjectsEXT. This keeps the experiment narrowly
 * scoped and makes it easy to compare against the Vulkan-first path.
 */

#import <Metal/Metal.h>

#include "xrt/xrt_compositor.h"
#include "util/comp_swapchain.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "vk/vk_helpers.h"

#include <stdbool.h>
#include <stdlib.h>

DEBUG_GET_ONCE_BOOL_OPTION(metal_array_import_experiment, "XRT_MACOS_METAL_ARRAY_IMPORT", true)

static MTLPixelFormat
vk_format_to_metal(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
	case VK_FORMAT_R8G8B8A8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
	case VK_FORMAT_B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
	case VK_FORMAT_B8G8R8A8_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return MTLPixelFormatBGR10A2Unorm;
	default: return MTLPixelFormatInvalid;
	}
}

static MTLTextureUsage
xrt_usage_to_metal(enum xrt_swapchain_usage_bits bits)
{
	MTLTextureUsage usage = MTLTextureUsageUnknown;

	if ((bits & XRT_SWAPCHAIN_USAGE_COLOR) != 0 || (bits & XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL) != 0) {
		usage |= MTLTextureUsageRenderTarget;
	}
	if ((bits & XRT_SWAPCHAIN_USAGE_SAMPLED) != 0 || (bits & XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS) != 0) {
		usage |= MTLTextureUsageShaderRead;
	}
	if ((bits & XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS) != 0) {
		usage |= MTLTextureUsageShaderWrite;
	}
	if ((bits & XRT_SWAPCHAIN_USAGE_MUTABLE_FORMAT) != 0) {
		usage |= MTLTextureUsagePixelFormatView;
	}

	return usage;
}

static void
clear_image_views(struct comp_swapchain *sc, uint32_t image_index)
{
	struct vk_bundle *vk = sc->vk;
	struct comp_swapchain_image *image = &sc->images[image_index];

	for (uint32_t layer = 0; layer < image->array_size; layer++) {
		if (image->views.alpha != NULL && image->views.alpha[layer] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, image->views.alpha[layer], NULL);
			image->views.alpha[layer] = VK_NULL_HANDLE;
		}
		if (image->views.no_alpha != NULL && image->views.no_alpha[layer] != VK_NULL_HANDLE) {
			vk->vkDestroyImageView(vk->device, image->views.no_alpha[layer], NULL);
			image->views.no_alpha[layer] = VK_NULL_HANDLE;
		}
	}
}

static VkResult
create_image_views(struct comp_swapchain *sc,
                   uint32_t image_index,
                   const struct xrt_swapchain_create_info *info)
{
	struct vk_bundle *vk = sc->vk;
	struct comp_swapchain_image *image = &sc->images[image_index];
	VkImage vk_image = sc->vkic.images[image_index].handle;
	VkFormat format = (VkFormat)info->format;
	VkImageAspectFlagBits aspect = vk_csci_get_image_view_aspect(format, info->bits);

	VkComponentMapping no_alpha_components = {
	    .r = VK_COMPONENT_SWIZZLE_R,
	    .g = VK_COMPONENT_SWIZZLE_G,
	    .b = VK_COMPONENT_SWIZZLE_B,
	    .a = VK_COMPONENT_SWIZZLE_ONE,
	};

	for (uint32_t layer = 0; layer < info->array_size; layer++) {
		VkImageSubresourceRange range = {
		    .aspectMask = aspect,
		    .baseMipLevel = 0,
		    .levelCount = 1,
		    .baseArrayLayer = layer,
		    .layerCount = 1,
		};

		VkResult ret = vk_create_view(vk,
		                              vk_image,
		                              VK_IMAGE_VIEW_TYPE_2D,
		                              format,
		                              range,
		                              &image->views.alpha[layer]);
		if (ret != VK_SUCCESS) {
			return ret;
		}

		ret = vk_create_view_swizzle(vk,
		                             vk_image,
		                             VK_IMAGE_VIEW_TYPE_2D,
		                             format,
		                             range,
		                             no_alpha_components,
		                             &image->views.no_alpha[layer]);
		if (ret != VK_SUCCESS) {
			return ret;
		}
	}

	return VK_SUCCESS;
}

static VkResult
replace_image_with_metal_texture(struct comp_swapchain *sc,
                                 uint32_t image_index,
                                 const struct xrt_swapchain_create_info *info,
                                 id<MTLTexture> texture)
{
	struct vk_bundle *vk = sc->vk;

	clear_image_views(sc, image_index);

	struct vk_image *image = &sc->vkic.images[image_index];
	if (image->handle != VK_NULL_HANDLE) {
		vk->vkDestroyImage(vk->device, image->handle, NULL);
		image->handle = VK_NULL_HANDLE;
	}
	if (image->memory != VK_NULL_HANDLE) {
		vk->vkFreeMemory(vk->device, image->memory, NULL);
		image->memory = VK_NULL_HANDLE;
	}
	image->size = 0;
	image->use_dedicated_allocation = false;

	VkImageUsageFlags usage = vk_csci_get_image_usage_flags(vk, (VkFormat)info->format, info->bits);
	if (usage == 0) {
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	VkImageCreateFlags flags = 0;
	if ((info->bits & XRT_SWAPCHAIN_USAGE_MUTABLE_FORMAT) != 0) {
		flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	}

	VkExportMetalObjectCreateInfoEXT export_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
	    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
	};
	VkImportMetalTextureInfoEXT import_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_TEXTURE_INFO_EXT,
	    .pNext = &export_info,
	    .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
	    .mtlTexture = texture,
	};
	VkImageCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &import_info,
	    .flags = flags,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = (VkFormat)info->format,
	    .extent = {.width = info->width, .height = info->height, .depth = 1},
	    .mipLevels = info->mip_count,
	    .arrayLayers = info->array_size,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = usage,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult ret = vk->vkCreateImage(vk->device, &create_info, NULL, &image->handle);
	if (ret != VK_SUCCESS) {
		return ret;
	}

	// Prove that the imported VkImage exports back to the exact Metal object
	// that was used to create it. This is the central invariant of the test.
	VkExportMetalTextureInfoEXT texture_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
	    .image = image->handle,
	    .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
	};
	VkExportMetalObjectsInfoEXT objects_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &texture_info,
	};
	vk->vkExportMetalObjectsEXT(vk->device, &objects_info);
	if (texture_info.mtlTexture == NULL || texture_info.mtlTexture != texture) {
		U_LOG_E("Metal-first array round-trip mismatch: image=%u imported=%p exported=%p",
		        image_index,
		        (__bridge void *)texture,
		        (__bridge void *)texture_info.mtlTexture);
		vk->vkDestroyImage(vk->device, image->handle, NULL);
		image->handle = VK_NULL_HANDLE;
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	ret = create_image_views(sc, image_index, info);
	if (ret != VK_SUCCESS) {
		return ret;
	}

	U_LOG_I("Metal-first array image: image=%u texture=%p VkImage=%p type=%lu array_length=%lu storage=%lu usage=0x%lx",
	        image_index,
	        (__bridge void *)texture,
	        (void *)image->handle,
	        (unsigned long)texture.textureType,
	        (unsigned long)texture.arrayLength,
	        (unsigned long)texture.storageMode,
	        (unsigned long)texture.usage);

	return VK_SUCCESS;
}

xrt_result_t
client_metal_array_import_experiment_create_swapchain(struct xrt_compositor_native *xcn,
                                                       const struct xrt_swapchain_create_info *info,
                                                       struct xrt_swapchain_native **out_xscn,
                                                       void *metal_device)
{
	// Start through the ordinary path so comp_swapchain gets all of its normal
	// FIFO, use-count, mutex and view-array bookkeeping. The images are replaced
	// before this function returns to comp_metal_client.
	xrt_result_t xret = xrt_comp_native_create_swapchain(xcn, info, out_xscn);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	if (!debug_get_bool_option_metal_array_import_experiment() || info->array_size <= 1) {
		return XRT_SUCCESS;
	}

	if (info->face_count != 1 || info->sample_count != 1) {
		U_LOG_W("Metal-first array experiment skipped: array_size=%u face_count=%u sample_count=%u",
		        info->array_size,
		        info->face_count,
		        info->sample_count);
		return XRT_SUCCESS;
	}

	struct comp_swapchain *sc = (struct comp_swapchain *)(*out_xscn);
	struct vk_bundle *vk = sc->vk;
	if (vk == NULL || !vk->has_EXT_metal_objects || vk->vkExportMetalObjectsEXT == NULL) {
		U_LOG_E("Metal-first array experiment requires VK_EXT_metal_objects");
		xrt_swapchain_native_reference(out_xscn, NULL);
		return XRT_ERROR_VULKAN;
	}

	id<MTLDevice> device = (__bridge id<MTLDevice>)metal_device;
	MTLPixelFormat metal_format = vk_format_to_metal((VkFormat)info->format);
	if (device == nil || metal_format == MTLPixelFormatInvalid) {
		U_LOG_E("Metal-first array experiment cannot create texture: device=%p vk_format=%u",
		        metal_device,
		        info->format);
		xrt_swapchain_native_reference(out_xscn, NULL);
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}

	MTLTextureDescriptor *descriptor = [[MTLTextureDescriptor alloc] init];
	descriptor.textureType = MTLTextureType2DArray;
	descriptor.pixelFormat = metal_format;
	descriptor.width = info->width;
	descriptor.height = info->height;
	descriptor.depth = 1;
	descriptor.mipmapLevelCount = info->mip_count;
	descriptor.sampleCount = 1;
	descriptor.arrayLength = info->array_size;
	// Apple's cross-process VR example uses Private storage for shareable
	// 2D-array textures. Shareability is provided by MTLSharedTextureHandle,
	// not by MTLStorageModeShared.
	descriptor.storageMode = MTLStorageModePrivate;
	descriptor.usage = xrt_usage_to_metal(info->bits);

	U_LOG_I("Metal-first array experiment: replacing %u Vulkan-first images with shareable Metal textures size=%ux%u array_size=%u vk_format=%u metal_format=%lu usage=0x%lx",
	        sc->vkic.image_count,
	        info->width,
	        info->height,
	        info->array_size,
	        info->format,
	        (unsigned long)metal_format,
	        (unsigned long)descriptor.usage);

	for (uint32_t i = 0; i < sc->vkic.image_count; i++) {
		id<MTLTexture> texture = [device newSharedTextureWithDescriptor:descriptor];
		if (texture == nil) {
			U_LOG_E("Metal-first array texture creation failed at image %u", i);
			[descriptor release];
			xrt_swapchain_native_reference(out_xscn, NULL);
			return XRT_ERROR_ALLOCATION;
		}

		VkResult ret = replace_image_with_metal_texture(sc, i, info, texture);
		// MoltenVK retains an imported Metal texture for the lifetime of its
		// VkImage; comp_metal_client will independently retain the exported
		// object while exposing it to the OpenXR application.
		[texture release];

		if (ret != VK_SUCCESS) {
			U_LOG_E("Metal-first array VkImage import failed at image %u: %d", i, (int)ret);
			[descriptor release];
			xrt_swapchain_native_reference(out_xscn, NULL);
			return XRT_ERROR_VULKAN;
		}
	}

	[descriptor release];
	U_LOG_I("Metal-first array experiment active: final swapchain is Metal-owned and Vulkan-imported");
	return XRT_SUCCESS;
}

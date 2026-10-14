// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#import <Metal/Metal.h>
#include "comp_vk_metal_texture.h"
#include "comp_metal_client.h"
#include "vk/vk_compositor_flags.h"
#include "util/u_debug.h"
#include <stdatomic.h>
#include <stdlib.h>

DEBUG_GET_ONCE_BOOL_OPTION(vk_metal_textures, "XRT_MACOS_VK_CLIENT_METAL_TEXTURES", false)
static atomic_uint context_count;
enum
{
	MAX_CONTEXTS = 8
};
struct client_vk_metal_texture_context
{
	struct xrt_compositor_native *native;
	struct xrt_compositor_metal *metal;
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
};
bool
client_vk_metal_texture_enabled(void)
{
	return debug_get_bool_option_vk_metal_textures();
}
bool
client_vk_metal_texture_required(const struct xrt_swapchain_create_info *info)
{
	return info->array_size != 1 || info->face_count != 1 || info->sample_count != 1 || info->mip_count != 1 ||
	       (vk_csci_get_barrier_aspect_mask(info->format) & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
}
static MTLPixelFormat
format_to_metal(VkFormat format)
{
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
	case VK_FORMAT_R8G8B8A8_SRGB: return MTLPixelFormatRGBA8Unorm_sRGB;
	case VK_FORMAT_B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
	case VK_FORMAT_B8G8R8A8_SRGB: return MTLPixelFormatBGRA8Unorm_sRGB;
	case VK_FORMAT_R16G16B16A16_UNORM: return MTLPixelFormatRGBA16Unorm;
	case VK_FORMAT_R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
	case VK_FORMAT_R32_SFLOAT: return MTLPixelFormatR32Float;
	case VK_FORMAT_D16_UNORM: return MTLPixelFormatDepth16Unorm;
	case VK_FORMAT_D32_SFLOAT: return MTLPixelFormatDepth32Float;
	case VK_FORMAT_D32_SFLOAT_S8_UINT: return MTLPixelFormatDepth32Float_Stencil8;
	default: return MTLPixelFormatInvalid;
	}
}
struct client_vk_metal_texture_context *
client_vk_metal_texture_context_create(struct xrt_compositor_native *xcn, struct vk_bundle *vk)
{
	if (!vk->has_EXT_metal_objects || !vk->vkExportMetalObjectsEXT)
		return NULL;
	struct client_vk_metal_texture_context *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	if (atomic_fetch_add_explicit(&context_count, 1, memory_order_relaxed) >= MAX_CONTEXTS) {
		atomic_fetch_sub_explicit(&context_count, 1, memory_order_relaxed);
		U_LOG_E(
		    "Vulkan Metal frontend budget exhausted: 8 active or retained contexts; restart after unresolved "
		    "GPU work");
		free(c);
		return NULL;
	}
	VkExportMetalDeviceInfoEXT device = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
	VkExportMetalObjectsInfoEXT objects = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	                                       .pNext = &device};
	vk->vkExportMetalObjectsEXT(vk->device, &objects);
	c->device = [(id<MTLDevice>)device.mtlDevice retain];
	c->native = xcn;
	if (!c->device) {
		client_vk_metal_texture_context_destroy(c);
		return NULL;
	}
	return c;
}
void
client_vk_metal_texture_context_destroy(struct client_vk_metal_texture_context *c)
{
	if (!c)
		return;
	if (c->metal) {
		struct xrt_compositor *base = &c->metal->base;
		xrt_comp_destroy(&base);
	}
	[c->queue release];
	[c->device release];
	atomic_fetch_sub_explicit(&context_count, 1, memory_order_relaxed);
	free(c);
}
xrt_result_t
client_vk_metal_texture_swapchain_create(struct client_vk_metal_texture_context *c,
                                         const struct xrt_swapchain_create_info *info,
                                         struct xrt_swapchain_metal **out_swapchain,
                                         struct xrt_swapchain_native **out_native)
{
	*out_swapchain = NULL;
	*out_native = NULL;
	if (!c || info->face_count != 1 || info->sample_count != 1 || info->mip_count != 1 || info->array_size == 0 ||
	    (info->create & XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT) ||
	    (info->bits & XRT_SWAPCHAIN_USAGE_MUTABLE_FORMAT))
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	MTLPixelFormat format = format_to_metal(info->format);
	if (format == MTLPixelFormatInvalid)
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	if (!c->metal) {
		c->queue = [c->device newCommandQueue];
		if (!c->queue)
			return XRT_ERROR_ALLOCATION;
#ifdef XRT_FEATURE_SERVICE
		c->metal = c->native->is_remote ? client_metal_service_compositor_create(c->native, c->device, c->queue)
		                                : client_metal_direct_compositor_create(c->native, c->device, c->queue);
#else
		c->metal = client_metal_direct_compositor_create(c->native, c->device, c->queue);
#endif
		if (!c->metal) {
			[c->queue release];
			c->queue = nil;
			return XRT_ERROR_ALLOCATION;
		}
	}
	struct xrt_swapchain_create_properties properties = {0};
	xrt_result_t property_result = xrt_comp_get_swapchain_create_properties(&c->native->base, info, &properties);
	if (property_result != XRT_SUCCESS)
		return property_result;
	struct xrt_swapchain_create_info metal_info = *info;
	metal_info.format = format;
	// Keep the Vulkan frontend's native allocation depth for colour/depth parity.
	if (properties.image_count == 3)
		metal_info.create |= XRT_SWAPCHAIN_CREATE_MACOS_THREE_IMAGES;

	struct xrt_swapchain *sc = NULL;
	xrt_result_t result = xrt_comp_create_swapchain(&c->metal->base, &metal_info, &sc);
	if (result != XRT_SUCCESS)
		return result;
	*out_swapchain = (struct xrt_swapchain_metal *)sc;
	xrt_swapchain_native_reference(out_native, client_metal_swapchain_get_native(*out_swapchain));
	U_LOG_I("Vulkan Metal-backed swapchain: format=%lld layers=%u images=%u remote=%u", (long long)info->format,
	        info->array_size, sc->image_count, c->native->is_remote);
	return XRT_SUCCESS;
}
VkResult
client_vk_metal_texture_import(struct vk_bundle *vk,
                               const struct xrt_swapchain_create_info *info,
                               void *texture,
                               VkImage *out_image)
{
	*out_image = VK_NULL_HANDLE;
	VkExportMetalObjectCreateInfoEXT export_info = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
	                                                .exportObjectType =
	                                                    VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT};
	VkImportMetalTextureInfoEXT import = {.sType = VK_STRUCTURE_TYPE_IMPORT_METAL_TEXTURE_INFO_EXT,
	                                      .pNext = &export_info,
	                                      .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
	                                      .mtlTexture = (MTLTexture_id)texture};
	VkImageCreateInfo image = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                           .pNext = &import,
	                           .imageType = VK_IMAGE_TYPE_2D,
	                           .format = info->format,
	                           .extent = {info->width, info->height, 1},
	                           .mipLevels = 1,
	                           .arrayLayers = info->array_size,
	                           .samples = VK_SAMPLE_COUNT_1_BIT,
	                           .tiling = VK_IMAGE_TILING_OPTIMAL,
	                           .usage = vk_csci_get_image_usage_flags(vk, info->format, info->bits),
	                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	                           .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	if (image.usage == 0)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	VkResult result = vk->vkCreateImage(vk->device, &image, NULL, out_image);
	if (result != VK_SUCCESS)
		return result;
	VkExportMetalTextureInfoEXT native = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
	                                      .image = *out_image,
	                                      .plane = VK_IMAGE_ASPECT_PLANE_0_BIT};
	VkExportMetalObjectsInfoEXT objects = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	                                       .pNext = &native};
	vk->vkExportMetalObjectsEXT(vk->device, &objects);
	if (native.mtlTexture != texture) {
		vk->vkDestroyImage(vk->device, *out_image, NULL);
		*out_image = VK_NULL_HANDLE;
		U_LOG_E("Vulkan Metal import changed the underlying texture");
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}
	U_LOG_I(
	    "Vulkan Metal import: image=%p source=%p exported=%p format=%u layers=%u; no image memory allocation or "
	    "bind",
	    (void *)*out_image, texture, native.mtlTexture, (unsigned)info->format, info->array_size);
	return VK_SUCCESS;
}

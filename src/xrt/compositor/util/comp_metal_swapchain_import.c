// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Thread-local direct Metal texture import for compositor swapchains.
 * @ingroup comp_util
 */

#include "util/comp_metal_swapchain_import.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "vk/vk_helpers.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct metal_swapchain_import_request
{
	bool active;
	bool consumed;
	const struct vk_image_collection *direct_vkic;
	struct xrt_swapchain_create_info info;
	uint32_t image_count;
	void *textures[XRT_MAX_SWAPCHAIN_IMAGES];
};

static __thread struct metal_swapchain_import_request g_request = {0};

static bool
create_info_matches(const struct xrt_swapchain_create_info *a, const struct xrt_swapchain_create_info *b)
{
	if (a->create != b->create || a->bits != b->bits || a->format != b->format || a->sample_count != b->sample_count ||
	    a->width != b->width || a->height != b->height || a->face_count != b->face_count ||
	    a->array_size != b->array_size || a->mip_count != b->mip_count || a->format_count != b->format_count) {
		return false;
	}

	for (uint32_t i = 0; i < a->format_count; i++) {
		if (a->formats[i] != b->formats[i]) {
			return false;
		}
	}
	return true;
}

bool
comp_metal_swapchain_import_begin(const struct xrt_swapchain_create_info *info,
                                  uint32_t image_count,
                                  void *const *metal_textures)
{
	if (info == NULL || metal_textures == NULL || image_count == 0 || image_count > XRT_MAX_SWAPCHAIN_IMAGES ||
	    g_request.active) {
		return false;
	}

	memset(&g_request, 0, sizeof(g_request));
	g_request.active = true;
	g_request.info = *info;
	g_request.image_count = image_count;
	for (uint32_t i = 0; i < image_count; i++) {
		if (metal_textures[i] == NULL) {
			memset(&g_request, 0, sizeof(g_request));
			return false;
		}
		g_request.textures[i] = metal_textures[i];
	}
	return true;
}

bool
comp_metal_swapchain_import_was_consumed(void)
{
	return g_request.active && g_request.consumed;
}

void
comp_metal_swapchain_import_end(void)
{
	memset(&g_request, 0, sizeof(g_request));
}

static void
destroy_direct_images(struct vk_bundle *vk, struct vk_image_collection *vkic)
{
	for (uint32_t i = 0; i < vkic->image_count; i++) {
		if (vkic->images[i].handle != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, vkic->images[i].handle, NULL);
			vkic->images[i].handle = VK_NULL_HANDLE;
		}
		vkic->images[i].memory = VK_NULL_HANDLE;
		vkic->images[i].size = 0;
		vkic->images[i].use_dedicated_allocation = false;
	}
	vkic->image_count = 0;
}

static VkResult
create_direct_image(struct vk_bundle *vk,
                    const struct xrt_swapchain_create_info *info,
                    uint32_t image_index,
                    void *metal_texture,
                    struct vk_image *out_image)
{
	VkFormat format = (VkFormat)info->format;
	VkImageUsageFlags usage = vk_csci_get_image_usage_flags(vk, format, info->bits);
	if (usage == 0) {
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	VkImageCreateFlags flags = 0;
	if ((info->bits & XRT_SWAPCHAIN_USAGE_MUTABLE_FORMAT) != 0) {
		flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	}
	if (info->face_count == 6) {
		flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	}

	VkExportMetalObjectCreateInfoEXT export_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
	    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_TEXTURE_BIT_EXT,
	};

#ifdef VK_KHR_image_format_list
	VkFormat view_formats[XRT_MAX_SWAPCHAIN_CREATE_INFO_FORMAT_LIST_COUNT] = {0};
	for (uint32_t i = 0; i < info->format_count; i++) {
		view_formats[i] = (VkFormat)info->formats[i];
	}
	VkImageFormatListCreateInfoKHR format_list = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR,
	    .pNext = &export_info,
	    .viewFormatCount = info->format_count,
	    .pViewFormats = view_formats,
	};
#endif

	VkImportMetalTextureInfoEXT import_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_TEXTURE_INFO_EXT,
#ifdef VK_KHR_image_format_list
	    .pNext = vk->has_KHR_image_format_list && info->format_count != 0 ? &format_list : &export_info,
#else
	    .pNext = &export_info,
#endif
	    .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
	    .mtlTexture = metal_texture,
	};

	VkImageCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &import_info,
	    .flags = flags,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = format,
	    .extent = {.width = info->width, .height = info->height, .depth = 1},
	    .mipLevels = info->mip_count,
	    .arrayLayers = info->array_size * info->face_count,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = usage,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkResult ret = vk->vkCreateImage(vk->device, &create_info, NULL, &out_image->handle);
	if (ret != VK_SUCCESS) {
		return ret;
	}

	out_image->memory = VK_NULL_HANDLE;
	out_image->size = 0;
	out_image->use_dedicated_allocation = false;

	VkExportMetalTextureInfoEXT texture_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT,
	    .image = out_image->handle,
	    .plane = VK_IMAGE_ASPECT_PLANE_0_BIT,
	};
	VkExportMetalObjectsInfoEXT objects_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &texture_info,
	};
	vk->vkExportMetalObjectsEXT(vk->device, &objects_info);
	if (texture_info.mtlTexture == NULL || texture_info.mtlTexture != metal_texture) {
		U_LOG_E("Metal direct import round-trip mismatch: image=%u imported=%p exported=%p",
		        image_index,
		        metal_texture,
		        texture_info.mtlTexture);
		vk->vkDestroyImage(vk->device, out_image->handle, NULL);
		out_image->handle = VK_NULL_HANDLE;
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	U_LOG_I("Metal direct VkImage import: image=%u texture=%p VkImage=%p array_layers=%u format=%u usage=0x%x",
	        image_index,
	        metal_texture,
	        (void *)out_image->handle,
	        info->array_size * info->face_count,
	        (unsigned)format,
	        (unsigned)usage);
	return VK_SUCCESS;
}

VkResult
comp_metal_swapchain_import_allocate_or_default(struct vk_bundle *vk,
                                                const struct xrt_swapchain_create_info *info,
                                                uint32_t image_count,
                                                struct vk_image_collection *out_vkic)
{
	if (!g_request.active || g_request.consumed || g_request.image_count != image_count ||
	    !create_info_matches(&g_request.info, info)) {
		return vk_ic_allocate(vk, info, image_count, out_vkic);
	}

	g_request.consumed = true;
	g_request.direct_vkic = out_vkic;

	if (!vk->has_EXT_metal_objects || vk->vkExportMetalObjectsEXT == NULL) {
		U_LOG_E("Metal direct swapchain requires VK_EXT_metal_objects");
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}

	memset(out_vkic, 0, sizeof(*out_vkic));
	out_vkic->info = *info;
	out_vkic->image_count = image_count;

	for (uint32_t i = 0; i < image_count; i++) {
		VkResult ret = create_direct_image(vk, info, i, g_request.textures[i], &out_vkic->images[i]);
		if (ret != VK_SUCCESS) {
			destroy_direct_images(vk, out_vkic);
			return ret;
		}
	}

	U_LOG_I("Metal direct swapchain allocator consumed %u pre-created Metal texture(s); no Vulkan-first image allocation performed",
	        image_count);
	return VK_SUCCESS;
}

VkResult
comp_metal_swapchain_import_get_handles_or_default(struct vk_bundle *vk,
                                                   struct vk_image_collection *vkic,
                                                   uint32_t max_handles,
                                                   xrt_graphics_buffer_handle_t *out_handles)
{
	if (!g_request.active || !g_request.consumed || g_request.direct_vkic != vkic) {
		return vk_ic_get_handles(vk, vkic, max_handles, out_handles);
	}

	if (max_handles < vkic->image_count) {
		return VK_ERROR_TOO_MANY_OBJECTS;
	}

	/* Direct Metal textures have no IOSurface/native-buffer handoff in-process. */
	for (uint32_t i = 0; i < vkic->image_count; i++) {
		out_handles[i] = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	}
	return VK_SUCCESS;
}

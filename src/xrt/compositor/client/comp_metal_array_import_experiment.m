// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental Metal-first swapchain backing for macOS.
 * @author OpenAI
 * @ingroup comp_client
 *
 * The ordinary compositor/client swapchain is created first so all of Monado's
 * normal bookkeeping and client wrapping remain unchanged. Before the finished
 * swapchain is returned to the OpenXR application, each native VkImage is then
 * replaced by a VkImage importing a Metal-owned shareable texture, and the
 * client-facing XrSwapchainImageMetalKHR is changed to that exact same
 * MTLTexture object.
 *
 * This deliberately covers both:
 *
 *   MTLTextureType2D      -> VkImportMetalTextureInfoEXT -> VkImage layers=1
 *   MTLTextureType2DArray -> VkImportMetalTextureInfoEXT -> VkImage layers=N
 *
 * The replacement is deferred until comp_metal_client has finished creating
 * its normal client texture wrappers. That keeps the old IOSurface path valid
 * during construction for arraySize=1, while still ensuring the texture that
 * escapes to the application is the Metal-owned object.
 */

#import <Metal/Metal.h>

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_gfx_metal.h"
#include "util/comp_swapchain.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "vk/vk_helpers.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

DEBUG_GET_ONCE_BOOL_OPTION(metal_array_import_experiment, "XRT_MACOS_METAL_ARRAY_IMPORT", true)

/* comp_metal_client.m is renamed to this symbol by comp_metal_client.h. */
struct xrt_compositor_metal *
client_metal_compositor_create_vanilla(struct xrt_compositor_native *xcn,
                                       void *metal_device,
                                       void *command_queue);

struct metal_first_compositor_link
{
	struct xrt_compositor *xc;
	xrt_result_t (*original_create_swapchain)(struct xrt_compositor *,
	                                          const struct xrt_swapchain_create_info *,
	                                          struct xrt_swapchain **);
	void (*original_destroy)(struct xrt_compositor *);
	struct metal_first_compositor_link *next;
};

struct pending_metal_first_swapchain
{
	bool active;
	struct comp_swapchain *sc;
	struct xrt_swapchain_create_info info;
	uint32_t image_count;
	id<MTLTexture> *textures;
};

static pthread_mutex_t g_contexts_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct metal_first_compositor_link *g_contexts = NULL;
static __thread struct pending_metal_first_swapchain g_pending = {0};

static struct metal_first_compositor_link *
find_context_locked(struct xrt_compositor *xc)
{
	for (struct metal_first_compositor_link *link = g_contexts; link != NULL; link = link->next) {
		if (link->xc == xc) {
			return link;
		}
	}
	return NULL;
}

static void
pending_clear(void)
{
	if (g_pending.textures != NULL) {
		for (uint32_t i = 0; i < g_pending.image_count; i++) {
			if (g_pending.textures[i] != nil) {
				[g_pending.textures[i] release];
				g_pending.textures[i] = nil;
			}
		}
		free(g_pending.textures);
	}
	memset(&g_pending, 0, sizeof(g_pending));
}

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
		U_LOG_E("Metal-first round-trip mismatch: image=%u imported=%p exported=%p",
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

	U_LOG_I("Metal-first image: image=%u texture=%p VkImage=%p type=%lu array_length=%lu storage=%lu usage=0x%lx",
	        image_index,
	        (__bridge void *)texture,
	        (void *)image->handle,
	        (unsigned long)texture.textureType,
	        (unsigned long)texture.arrayLength,
	        (unsigned long)texture.storageMode,
	        (unsigned long)texture.usage);

	return VK_SUCCESS;
}

static xrt_result_t
finish_pending_swapchain(struct xrt_swapchain **out_xsc)
{
	if (!g_pending.active) {
		return XRT_SUCCESS;
	}
	if (out_xsc == NULL || *out_xsc == NULL) {
		pending_clear();
		return XRT_ERROR_ALLOCATION;
	}

	struct xrt_swapchain_metal *metal_xsc = (struct xrt_swapchain_metal *)(*out_xsc);
	if (metal_xsc->base.image_count != g_pending.image_count ||
	    g_pending.sc == NULL ||
	    g_pending.sc->vkic.image_count != g_pending.image_count) {
		U_LOG_E("Metal-first finish mismatch: client_images=%u native_images=%u pending_images=%u",
		        metal_xsc->base.image_count,
		        g_pending.sc != NULL ? g_pending.sc->vkic.image_count : 0,
		        g_pending.image_count);
		pending_clear();
		return XRT_ERROR_ALLOCATION;
	}

	for (uint32_t i = 0; i < g_pending.image_count; i++) {
		id<MTLTexture> texture = g_pending.textures[i];
		VkResult ret = replace_image_with_metal_texture(g_pending.sc, i, &g_pending.info, texture);
		if (ret != VK_SUCCESS) {
			U_LOG_E("Metal-first VkImage import failed at image %u: %d", i, (int)ret);
			pending_clear();
			return XRT_ERROR_VULKAN;
		}

		id<MTLTexture> old_texture = (__bridge id<MTLTexture>)metal_xsc->images[i];
		if (old_texture != nil) {
			[old_texture release];
		}

		// Transfer the +1 retain from newSharedTextureWithDescriptor directly to
		// the client swapchain. client_metal_swapchain_destroy will release it.
		metal_xsc->images[i] = (__bridge void *)texture;
		g_pending.textures[i] = nil;
	}

	free(g_pending.textures);
	g_pending.textures = NULL;
	U_LOG_I("Metal-first swapchain active: client and Vulkan compositor share the same Metal-owned %s texture objects (array_size=%u)",
	        g_pending.info.array_size > 1 ? "2D-array" : "2D",
	        g_pending.info.array_size);
	memset(&g_pending, 0, sizeof(g_pending));
	return XRT_SUCCESS;
}

xrt_result_t
client_metal_array_import_experiment_create_swapchain(struct xrt_compositor_native *xcn,
                                                       const struct xrt_swapchain_create_info *info,
                                                       struct xrt_swapchain_native **out_xscn,
                                                       void *metal_device)
{
	// Always let the ordinary native path establish FIFO/use-count/mutex/view
	// bookkeeping first. The Metal-owned replacement is deliberately deferred
	// until comp_metal_client has finished creating its client-side texture
	// wrappers, avoiding stale IOSurface use for arraySize=1 during construction.
	xrt_result_t xret = xrt_comp_native_create_swapchain(xcn, info, out_xscn);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	if (!debug_get_bool_option_metal_array_import_experiment()) {
		return XRT_SUCCESS;
	}

	if (info->face_count != 1 || info->sample_count != 1 || info->array_size == 0) {
		U_LOG_W("Metal-first experiment skipped: array_size=%u face_count=%u sample_count=%u",
		        info->array_size,
		        info->face_count,
		        info->sample_count);
		return XRT_SUCCESS;
	}

	pending_clear();

	struct comp_swapchain *sc = (struct comp_swapchain *)(*out_xscn);
	struct vk_bundle *vk = sc->vk;
	if (vk == NULL || !vk->has_EXT_metal_objects || vk->vkExportMetalObjectsEXT == NULL) {
		U_LOG_E("Metal-first experiment requires VK_EXT_metal_objects");
		xrt_swapchain_native_reference(out_xscn, NULL);
		return XRT_ERROR_VULKAN;
	}

	id<MTLDevice> device = (__bridge id<MTLDevice>)metal_device;
	MTLPixelFormat metal_format = vk_format_to_metal((VkFormat)info->format);
	if (device == nil || metal_format == MTLPixelFormatInvalid) {
		U_LOG_E("Metal-first experiment cannot create texture: device=%p vk_format=%u",
		        metal_device,
		        info->format);
		xrt_swapchain_native_reference(out_xscn, NULL);
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}

	id<MTLTexture> *textures = calloc(sc->vkic.image_count, sizeof(*textures));
	if (textures == NULL) {
		xrt_swapchain_native_reference(out_xscn, NULL);
		return XRT_ERROR_ALLOCATION;
	}

	MTLTextureDescriptor *descriptor = [[MTLTextureDescriptor alloc] init];
	descriptor.textureType = info->array_size > 1 ? MTLTextureType2DArray : MTLTextureType2D;
	descriptor.pixelFormat = metal_format;
	descriptor.width = info->width;
	descriptor.height = info->height;
	descriptor.depth = 1;
	descriptor.mipmapLevelCount = info->mip_count;
	descriptor.sampleCount = 1;
	descriptor.arrayLength = info->array_size;
	descriptor.storageMode = MTLStorageModePrivate;
	descriptor.usage = xrt_usage_to_metal(info->bits);

	U_LOG_I("Metal-first experiment: preparing %u shareable Metal textures size=%ux%u array_size=%u type=%lu vk_format=%u metal_format=%lu usage=0x%lx",
	        sc->vkic.image_count,
	        info->width,
	        info->height,
	        info->array_size,
	        (unsigned long)descriptor.textureType,
	        info->format,
	        (unsigned long)metal_format,
	        (unsigned long)descriptor.usage);

	for (uint32_t i = 0; i < sc->vkic.image_count; i++) {
		textures[i] = [device newSharedTextureWithDescriptor:descriptor];
		if (textures[i] == nil) {
			U_LOG_E("Metal-first texture creation failed at image %u array_size=%u", i, info->array_size);
			for (uint32_t j = 0; j < i; j++) {
				[textures[j] release];
			}
			free(textures);
			[descriptor release];
			xrt_swapchain_native_reference(out_xscn, NULL);
			return XRT_ERROR_ALLOCATION;
		}
	}

	[descriptor release];
	g_pending.active = true;
	g_pending.sc = sc;
	g_pending.info = *info;
	g_pending.image_count = sc->vkic.image_count;
	g_pending.textures = textures;
	return XRT_SUCCESS;
}

static xrt_result_t
wrapped_create_swapchain(struct xrt_compositor *xc,
                         const struct xrt_swapchain_create_info *info,
                         struct xrt_swapchain **out_xsc)
{
	pthread_mutex_lock(&g_contexts_mutex);
	struct metal_first_compositor_link *link = find_context_locked(xc);
	xrt_result_t (*original_create)(struct xrt_compositor *, const struct xrt_swapchain_create_info *, struct xrt_swapchain **) =
	    link != NULL ? link->original_create_swapchain : NULL;
	pthread_mutex_unlock(&g_contexts_mutex);

	if (original_create == NULL) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}

	pending_clear();
	xrt_result_t xret = original_create(xc, info, out_xsc);
	if (xret != XRT_SUCCESS) {
		pending_clear();
		return xret;
	}

	xret = finish_pending_swapchain(out_xsc);
	if (xret != XRT_SUCCESS) {
		if (out_xsc != NULL && *out_xsc != NULL) {
			xrt_swapchain_reference(out_xsc, NULL);
		}
		return xret;
	}

	return XRT_SUCCESS;
}

static void
wrapped_compositor_destroy(struct xrt_compositor *xc)
{
	void (*original_destroy)(struct xrt_compositor *) = NULL;

	pthread_mutex_lock(&g_contexts_mutex);
	struct metal_first_compositor_link **ptr = &g_contexts;
	while (*ptr != NULL) {
		if ((*ptr)->xc == xc) {
			struct metal_first_compositor_link *link = *ptr;
			*ptr = link->next;
			original_destroy = link->original_destroy;
			free(link);
			break;
		}
		ptr = &(*ptr)->next;
	}
	pthread_mutex_unlock(&g_contexts_mutex);

	pending_clear();
	if (original_destroy != NULL) {
		original_destroy(xc);
	}
}

struct xrt_compositor_metal *
client_metal_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue)
{
	struct xrt_compositor_metal *xcm = client_metal_compositor_create_vanilla(xcn, metal_device, command_queue);
	if (xcm == NULL) {
		return NULL;
	}

	struct metal_first_compositor_link *link = calloc(1, sizeof(*link));
	if (link == NULL) {
		U_LOG_W("Could not allocate Metal-first compositor wrapper; experiment disabled for this compositor");
		return xcm;
	}

	link->xc = &xcm->base;
	link->original_create_swapchain = xcm->base.create_swapchain;
	link->original_destroy = xcm->base.destroy;

	pthread_mutex_lock(&g_contexts_mutex);
	link->next = g_contexts;
	g_contexts = link;
	xcm->base.create_swapchain = wrapped_create_swapchain;
	xcm->base.destroy = wrapped_compositor_destroy;
	pthread_mutex_unlock(&g_contexts_mutex);

	U_LOG_I("Metal-first swapchain wrapper installed; XRT_MACOS_METAL_ARRAY_IMPORT controls both arraySize=1 and arraySize>1");
	return xcm;
}

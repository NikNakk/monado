// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Metal-owned swapchains for the in-process macOS Metal client.
 * @author OpenAI
 * @ingroup comp_client
 */

#import <Metal/Metal.h>

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_gfx_metal.h"
#include "util/comp_metal_swapchain_import.h"
#include "util/comp_swapchain.h"
#include "util/u_logging.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* comp_metal_client.m is renamed to this symbol by comp_metal_client.h. */
struct xrt_compositor_metal *
client_metal_compositor_create_vanilla(struct xrt_compositor_native *xcn,
                                       void *metal_device,
                                       void *command_queue);

struct metal_direct_compositor_link
{
	struct xrt_compositor *xc;
	struct xrt_compositor_native *xcn;
	id<MTLDevice> device;
	id<MTLCommandQueue> command_queue;
	void (*original_destroy)(struct xrt_compositor *);
	struct metal_direct_compositor_link *next;
};

struct metal_direct_swapchain
{
	struct xrt_swapchain_metal base;
	struct xrt_swapchain_native *xscn;
	id<MTLCommandQueue> command_queue;
};

static pthread_mutex_t g_contexts_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct metal_direct_compositor_link *g_contexts = NULL;

static struct metal_direct_compositor_link *
find_context_locked(struct xrt_compositor *xc)
{
	for (struct metal_direct_compositor_link *link = g_contexts; link != NULL; link = link->next) {
		if (link->xc == xc) {
			return link;
		}
	}
	return NULL;
}

static struct metal_direct_compositor_link *
find_context(struct xrt_compositor *xc)
{
	pthread_mutex_lock(&g_contexts_mutex);
	struct metal_direct_compositor_link *link = find_context_locked(xc);
	pthread_mutex_unlock(&g_contexts_mutex);
	return link;
}

static uint32_t
metal_format_to_vk(MTLPixelFormat format)
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm: return 37;      // VK_FORMAT_R8G8B8A8_UNORM
	case MTLPixelFormatRGBA8Unorm_sRGB: return 43; // VK_FORMAT_R8G8B8A8_SRGB
	case MTLPixelFormatBGRA8Unorm: return 44;      // VK_FORMAT_B8G8R8A8_UNORM
	case MTLPixelFormatBGRA8Unorm_sRGB: return 50; // VK_FORMAT_B8G8R8A8_SRGB
	case MTLPixelFormatBGR10A2Unorm: return 64;    // VK_FORMAT_A2B10G10R10_UNORM_PACK32
	default: return 0;
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

static const char *
metal_texture_type_string(MTLTextureType type)
{
	switch (type) {
	case MTLTextureType2D: return "2D";
	case MTLTextureType2DArray: return "2DArray";
	default: return "unknown";
	}
}

static inline struct metal_direct_swapchain *
metal_direct_swapchain(struct xrt_swapchain *xsc)
{
	return (struct metal_direct_swapchain *)xsc;
}

static inline struct xrt_swapchain *
to_native_swapchain(struct xrt_swapchain *xsc)
{
	return &metal_direct_swapchain(xsc)->xscn->base;
}

static void
metal_direct_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct metal_direct_swapchain *sc = metal_direct_swapchain(xsc);

	for (uint32_t i = 0; i < sc->base.base.image_count; i++) {
		id<MTLTexture> texture = (__bridge id<MTLTexture>)sc->base.images[i];
		if (texture != nil) {
			[texture release];
			sc->base.images[i] = NULL;
		}
	}

	xrt_swapchain_native_reference(&sc->xscn, NULL);
	[sc->command_queue release];
	free(sc);
}

static xrt_result_t
metal_direct_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	return xrt_swapchain_acquire_image(to_native_swapchain(xsc), out_index);
}

static xrt_result_t
metal_direct_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	return xrt_swapchain_wait_image(to_native_swapchain(xsc), timeout_ns, index);
}

static xrt_result_t
metal_direct_swapchain_barrier_image(struct xrt_swapchain *xsc,
                                     enum xrt_barrier_direction direction,
                                     uint32_t index)
{
	struct metal_direct_swapchain *sc = metal_direct_swapchain(xsc);

	if (direction == XRT_BARRIER_TO_COMP) {
		/*
		 * Correct fallback when Stage 4 is disabled or unavailable. Stage 4
		 * wraps this callback and replaces this CPU wait with its shared-event
		 * handoff once the timeline pair is ready.
		 */
		@autoreleasepool {
			id<MTLCommandBuffer> command_buffer = [sc->command_queue commandBuffer];
			if (command_buffer == nil) {
				return XRT_ERROR_ALLOCATION;
			}
			[command_buffer commit];
			[command_buffer waitUntilCompleted];
			if (command_buffer.status == MTLCommandBufferStatusError) {
				return XRT_ERROR_NATIVE_HANDLE_FENCE_ERROR;
			}
		}
	}

	struct xrt_swapchain *native = to_native_swapchain(xsc);
	if (native->barrier_image == NULL) {
		return XRT_SUCCESS;
	}
	return xrt_swapchain_barrier_image(native, direction, index);
}

static xrt_result_t
metal_direct_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	return xrt_swapchain_release_image(to_native_swapchain(xsc), index);
}

static void
release_texture_array(id<MTLTexture> *textures, uint32_t image_count)
{
	if (textures == NULL) {
		return;
	}
	for (uint32_t i = 0; i < image_count; i++) {
		if (textures[i] != nil) {
			[textures[i] release];
		}
	}
	free(textures);
}

static xrt_result_t
metal_direct_create_swapchain(struct xrt_compositor *xc,
                              const struct xrt_swapchain_create_info *info,
                              struct xrt_swapchain **out_xsc)
{
	struct metal_direct_compositor_link *link = find_context(xc);
	if (link == NULL || out_xsc == NULL) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}

	if (info->face_count != 1 || info->sample_count != 1 || info->array_size == 0) {
		U_LOG_W("Metal direct swapchain unsupported geometry: array_size=%u face_count=%u sample_count=%u",
		        info->array_size,
		        info->face_count,
		        info->sample_count);
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}

	uint32_t vk_format = metal_format_to_vk((MTLPixelFormat)info->format);
	if (vk_format == 0) {
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}

	struct xrt_swapchain_create_properties xsccp = XRT_STRUCT_INIT;
	xrt_result_t xret = xrt_comp_get_swapchain_create_properties(xc, info, &xsccp);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	if (xsccp.image_count == 0 || xsccp.image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return XRT_ERROR_ALLOCATION;
	}

	struct xrt_swapchain_create_info native_info = *info;
	native_info.format = vk_format;
	native_info.bits |= xsccp.extra_bits;

	MTLTextureDescriptor *descriptor = [[MTLTextureDescriptor alloc] init];
	descriptor.textureType = info->array_size > 1 ? MTLTextureType2DArray : MTLTextureType2D;
	descriptor.pixelFormat = (MTLPixelFormat)info->format;
	descriptor.width = info->width;
	descriptor.height = info->height;
	descriptor.depth = 1;
	descriptor.mipmapLevelCount = info->mip_count;
	descriptor.sampleCount = 1;
	descriptor.arrayLength = info->array_size;
	descriptor.storageMode = MTLStorageModePrivate;
	descriptor.usage = xrt_usage_to_metal(native_info.bits);

	id<MTLTexture> *textures = calloc(xsccp.image_count, sizeof(*textures));
	void **raw_textures = calloc(xsccp.image_count, sizeof(*raw_textures));
	if (textures == NULL || raw_textures == NULL) {
		free(raw_textures);
		free(textures);
		[descriptor release];
		return XRT_ERROR_ALLOCATION;
	}

	U_LOG_I("Metal direct swapchain: creating %u Metal-owned texture(s) size=%ux%u array_size=%u type=%s(%lu) vk_format=%u metal_format=%lld usage=0x%lx",
	        xsccp.image_count,
	        info->width,
	        info->height,
	        info->array_size,
	        metal_texture_type_string(descriptor.textureType),
	        (unsigned long)descriptor.textureType,
	        vk_format,
	        (long long)info->format,
	        (unsigned long)descriptor.usage);

	for (uint32_t i = 0; i < xsccp.image_count; i++) {
		textures[i] = [link->device newSharedTextureWithDescriptor:descriptor];
		if (textures[i] == nil) {
			U_LOG_E("Metal direct texture creation failed at image %u", i);
			[descriptor release];
			free(raw_textures);
			release_texture_array(textures, xsccp.image_count);
			return XRT_ERROR_ALLOCATION;
		}
		raw_textures[i] = (__bridge void *)textures[i];
	}
	[descriptor release];

	if (!comp_metal_swapchain_import_begin(&native_info, xsccp.image_count, raw_textures)) {
		U_LOG_E("Metal direct swapchain could not start allocator handoff");
		free(raw_textures);
		release_texture_array(textures, xsccp.image_count);
		return XRT_ERROR_ALLOCATION;
	}

	struct xrt_swapchain_native *xscn = NULL;
	xret = xrt_comp_native_create_swapchain(link->xcn, &native_info, &xscn);
	bool consumed = comp_metal_swapchain_import_was_consumed();
	comp_metal_swapchain_import_end();
	free(raw_textures);

	if (xret != XRT_SUCCESS) {
		release_texture_array(textures, xsccp.image_count);
		return xret;
	}
	if (!consumed) {
		U_LOG_E("Metal direct swapchain native create succeeded without consuming the direct allocator request");
		xrt_swapchain_native_reference(&xscn, NULL);
		release_texture_array(textures, xsccp.image_count);
		return XRT_ERROR_VULKAN;
	}
	if (xscn == NULL || xscn->base.image_count != xsccp.image_count) {
		U_LOG_E("Metal direct swapchain image-count mismatch: expected=%u actual=%u",
		        xsccp.image_count,
		        xscn != NULL ? xscn->base.image_count : 0);
		xrt_swapchain_native_reference(&xscn, NULL);
		release_texture_array(textures, xsccp.image_count);
		return XRT_ERROR_ALLOCATION;
	}

	for (uint32_t i = 0; i < xsccp.image_count; i++) {
		void *exported = NULL;
		VkImage vk_image = VK_NULL_HANDLE;
		VkResult ret = comp_swapchain_export_metal_texture(xscn, i, &exported, &vk_image);
		if (ret != VK_SUCCESS || exported != (__bridge void *)textures[i]) {
			U_LOG_E("Metal direct client identity mismatch: image=%u VkImage=%p expected=%p exported=%p result=%d",
			        i,
			        (void *)vk_image,
			        (__bridge void *)textures[i],
			        exported,
			        (int)ret);
			xrt_swapchain_native_reference(&xscn, NULL);
			release_texture_array(textures, xsccp.image_count);
			return XRT_ERROR_VULKAN;
		}
	}

	struct metal_direct_swapchain *sc = calloc(1, sizeof(*sc));
	if (sc == NULL) {
		xrt_swapchain_native_reference(&xscn, NULL);
		release_texture_array(textures, xsccp.image_count);
		return XRT_ERROR_ALLOCATION;
	}

	sc->base.base.destroy = metal_direct_swapchain_destroy;
	sc->base.base.acquire_image = metal_direct_swapchain_acquire_image;
	sc->base.base.wait_image = metal_direct_swapchain_wait_image;
	sc->base.base.barrier_image = metal_direct_swapchain_barrier_image;
	sc->base.base.release_image = metal_direct_swapchain_release_image;
	sc->base.base.reference.count = 1;
	sc->base.base.image_count = xsccp.image_count;
	sc->xscn = xscn;
	sc->command_queue = [link->command_queue retain];

	for (uint32_t i = 0; i < xsccp.image_count; i++) {
		/* Transfer newSharedTextureWithDescriptor's +1 ownership to the client. */
		sc->base.images[i] = (__bridge void *)textures[i];
		textures[i] = nil;
	}
	free(textures);

	U_LOG_I("Metal direct swapchain active: no temporary Vulkan/IOSurface allocation; client and compositor share the same Metal-owned %s texture objects (array_size=%u)",
	        info->array_size > 1 ? "2D-array" : "2D",
	        info->array_size);

	*out_xsc = &sc->base.base;
	return XRT_SUCCESS;
}

static xrt_result_t
metal_direct_layer_projection(struct xrt_compositor *xc,
                              struct xrt_device *xdev,
                              struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                              const struct xrt_layer_data *data)
{
	struct metal_direct_compositor_link *link = find_context(xc);
	if (link == NULL) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	struct xrt_swapchain *native[XRT_MAX_VIEWS] = {0};
	for (uint32_t i = 0; i < data->view_count; i++) {
		native[i] = to_native_swapchain(xsc[i]);
	}
	return xrt_comp_layer_projection(&link->xcn->base, xdev, native, data);
}

static xrt_result_t
metal_direct_layer_projection_depth(struct xrt_compositor *xc,
                                    struct xrt_device *xdev,
                                    struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                    struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                    const struct xrt_layer_data *data)
{
	struct metal_direct_compositor_link *link = find_context(xc);
	if (link == NULL) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	struct xrt_swapchain *native[XRT_MAX_VIEWS] = {0};
	struct xrt_swapchain *native_depth[XRT_MAX_VIEWS] = {0};
	for (uint32_t i = 0; i < data->view_count; i++) {
		native[i] = to_native_swapchain(xsc[i]);
		native_depth[i] = to_native_swapchain(d_xsc[i]);
	}
	return xrt_comp_layer_projection_depth(&link->xcn->base, xdev, native, native_depth, data);
}

static xrt_result_t
metal_direct_layer_quad(struct xrt_compositor *xc,
                        struct xrt_device *xdev,
                        struct xrt_swapchain *xsc,
                        const struct xrt_layer_data *data)
{
	struct metal_direct_compositor_link *link = find_context(xc);
	return link != NULL ? xrt_comp_layer_quad(&link->xcn->base, xdev, to_native_swapchain(xsc), data)
	                    : XRT_ERROR_NOT_IMPLEMENTED;
}

static xrt_result_t
metal_direct_layer_cube(struct xrt_compositor *xc,
                        struct xrt_device *xdev,
                        struct xrt_swapchain *xsc,
                        const struct xrt_layer_data *data)
{
	struct metal_direct_compositor_link *link = find_context(xc);
	return link != NULL ? xrt_comp_layer_cube(&link->xcn->base, xdev, to_native_swapchain(xsc), data)
	                    : XRT_ERROR_NOT_IMPLEMENTED;
}

static xrt_result_t
metal_direct_layer_cylinder(struct xrt_compositor *xc,
                            struct xrt_device *xdev,
                            struct xrt_swapchain *xsc,
                            const struct xrt_layer_data *data)
{
	struct metal_direct_compositor_link *link = find_context(xc);
	return link != NULL ? xrt_comp_layer_cylinder(&link->xcn->base, xdev, to_native_swapchain(xsc), data)
	                    : XRT_ERROR_NOT_IMPLEMENTED;
}

static xrt_result_t
metal_direct_layer_equirect1(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct metal_direct_compositor_link *link = find_context(xc);
	return link != NULL ? xrt_comp_layer_equirect1(&link->xcn->base, xdev, to_native_swapchain(xsc), data)
	                    : XRT_ERROR_NOT_IMPLEMENTED;
}

static xrt_result_t
metal_direct_layer_equirect2(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct metal_direct_compositor_link *link = find_context(xc);
	return link != NULL ? xrt_comp_layer_equirect2(&link->xcn->base, xdev, to_native_swapchain(xsc), data)
	                    : XRT_ERROR_NOT_IMPLEMENTED;
}

static void
metal_direct_compositor_destroy(struct xrt_compositor *xc)
{
	struct metal_direct_compositor_link *link = NULL;

	pthread_mutex_lock(&g_contexts_mutex);
	struct metal_direct_compositor_link **ptr = &g_contexts;
	while (*ptr != NULL) {
		if ((*ptr)->xc == xc) {
			link = *ptr;
			*ptr = link->next;
			break;
		}
		ptr = &(*ptr)->next;
	}
	pthread_mutex_unlock(&g_contexts_mutex);

	if (link == NULL) {
		return;
	}

	void (*original_destroy)(struct xrt_compositor *) = link->original_destroy;
	[link->device release];
	[link->command_queue release];
	free(link);

	if (original_destroy != NULL) {
		original_destroy(xc);
	}
}

struct xrt_compositor_metal *
client_metal_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue)
{
	struct xrt_compositor_metal *xcm = client_metal_compositor_create_vanilla(xcn, metal_device, command_queue);
	if (xcm == NULL) {
		return xcm;
	}

	struct metal_direct_compositor_link *link = calloc(1, sizeof(*link));
	if (link == NULL) {
		U_LOG_W("Could not allocate Metal direct-swapchain wrapper; using ordinary Metal swapchains");
		return xcm;
	}

	link->xc = &xcm->base;
	link->xcn = xcn;
	link->device = [(__bridge id<MTLDevice>)metal_device retain];
	link->command_queue = [(__bridge id<MTLCommandQueue>)command_queue retain];
	link->original_destroy = xcm->base.destroy;

	pthread_mutex_lock(&g_contexts_mutex);
	link->next = g_contexts;
	g_contexts = link;
	xcm->base.create_swapchain = metal_direct_create_swapchain;
	xcm->base.layer_projection = metal_direct_layer_projection;
	xcm->base.layer_projection_depth = metal_direct_layer_projection_depth;
	xcm->base.layer_quad = metal_direct_layer_quad;
	xcm->base.layer_cube = metal_direct_layer_cube;
	xcm->base.layer_cylinder = metal_direct_layer_cylinder;
	xcm->base.layer_equirect1 = metal_direct_layer_equirect1;
	xcm->base.layer_equirect2 = metal_direct_layer_equirect2;
	xcm->base.destroy = metal_direct_compositor_destroy;
	pthread_mutex_unlock(&g_contexts_mutex);

	U_LOG_I("Metal swapchain path installed: Metal-owned textures are imported directly into Vulkan for arraySize=1 and arraySize>1");
	return xcm;
}

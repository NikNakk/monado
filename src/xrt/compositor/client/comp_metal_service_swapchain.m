// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Metal-owned swapchains transported to monado-service over XPC.
 * @ingroup comp_client
 */

#import <Metal/Metal.h>

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_gfx_metal.h"
#include "shared/ipc_metal_xpc.h"
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

struct metal_service_compositor_link
{
	struct xrt_compositor *xc;
	struct xrt_compositor_native *xcn;
	id<MTLDevice> device;
	id<MTLCommandQueue> command_queue;
	void (*original_destroy)(struct xrt_compositor *);
	struct metal_service_compositor_link *next;
};

/*
 * Keep xscn immediately after xrt_swapchain_metal: the vanilla Metal layer
 * forwarding code only uses that prefix when translating client swapchains
 * back to native swapchains.
 */
struct metal_service_swapchain
{
	struct xrt_swapchain_metal base;
	struct xrt_swapchain_native *xscn;
	void *vanilla_compat_compositor;
	uint64_t vanilla_compat_debug_release_count;
	id<MTLCommandQueue> command_queue;
};

static pthread_mutex_t g_contexts_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct metal_service_compositor_link *g_contexts = NULL;

static struct metal_service_compositor_link *
find_context_locked(struct xrt_compositor *xc)
{
	for (struct metal_service_compositor_link *link = g_contexts; link != NULL; link = link->next) {
		if (link->xc == xc) {
			return link;
		}
	}
	return NULL;
}

static struct metal_service_compositor_link *
find_context(struct xrt_compositor *xc)
{
	pthread_mutex_lock(&g_contexts_mutex);
	struct metal_service_compositor_link *link = find_context_locked(xc);
	pthread_mutex_unlock(&g_contexts_mutex);
	return link;
}

static uint32_t
metal_format_to_vk(MTLPixelFormat format)
{
	switch (format) {
	case MTLPixelFormatRGBA8Unorm: return 37;
	case MTLPixelFormatRGBA8Unorm_sRGB: return 43;
	case MTLPixelFormatBGRA8Unorm: return 44;
	case MTLPixelFormatBGRA8Unorm_sRGB: return 50;
	case MTLPixelFormatBGR10A2Unorm: return 64;
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

static inline struct metal_service_swapchain *
metal_service_swapchain(struct xrt_swapchain *xsc)
{
	return (struct metal_service_swapchain *)xsc;
}

static inline struct xrt_swapchain *
to_native_swapchain(struct xrt_swapchain *xsc)
{
	return &metal_service_swapchain(xsc)->xscn->base;
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

static void
metal_service_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct metal_service_swapchain *sc = metal_service_swapchain(xsc);

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
metal_service_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	return xrt_swapchain_acquire_image(to_native_swapchain(xsc), out_index);
}

static xrt_result_t
metal_service_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	return xrt_swapchain_wait_image(to_native_swapchain(xsc), timeout_ns, index);
}

static xrt_result_t
metal_service_swapchain_barrier_image(struct xrt_swapchain *xsc,
                                      enum xrt_barrier_direction direction,
                                      uint32_t index)
{
	struct metal_service_swapchain *sc = metal_service_swapchain(xsc);

	if (direction == XRT_BARRIER_TO_COMP) {
		/*
		 * First service milestone: preserve correctness by waiting for all
		 * application Metal work queued before xrReleaseSwapchainImage.
		 * Cross-process MTLSharedEvent Stage 4 is added separately.
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
metal_service_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	return xrt_swapchain_release_image(to_native_swapchain(xsc), index);
}

static xrt_result_t
metal_service_create_swapchain(struct xrt_compositor *xc,
                               const struct xrt_swapchain_create_info *info,
                               struct xrt_swapchain **out_xsc)
{
	struct metal_service_compositor_link *link = find_context(xc);
	if (link == NULL || info == NULL || out_xsc == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	if (info->face_count != 1 || info->sample_count != 1 || info->array_size == 0) {
		U_LOG_W("Metal service swapchain unsupported geometry: array_size=%u face_count=%u sample_count=%u",
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
	struct xrt_image_native *transport_images = calloc(xsccp.image_count, sizeof(*transport_images));
	if (textures == NULL || raw_textures == NULL || transport_images == NULL) {
		free(transport_images);
		free(raw_textures);
		free(textures);
		[descriptor release];
		return XRT_ERROR_ALLOCATION;
	}

	for (uint32_t i = 0; i < xsccp.image_count; i++) {
		textures[i] = [link->device newSharedTextureWithDescriptor:descriptor];
		if (textures[i] == nil) {
			U_LOG_E("Metal service texture creation failed at image %u", i);
			[descriptor release];
			free(transport_images);
			free(raw_textures);
			release_texture_array(textures, xsccp.image_count);
			return XRT_ERROR_ALLOCATION;
		}
		raw_textures[i] = (__bridge void *)textures[i];
	}
	[descriptor release];

	uint64_t token = 0;
	xret = ipc_metal_xpc_publish_textures(raw_textures, xsccp.image_count, &token);
	free(raw_textures);
	if (xret != XRT_SUCCESS) {
		free(transport_images);
		release_texture_array(textures, xsccp.image_count);
		return xret;
	}

	ipc_metal_xpc_make_token_images(token, xsccp.image_count, transport_images);

	U_LOG_I("Metal service swapchain: published %u Metal-owned texture(s) token=0x%016llx size=%ux%u array_size=%u",
	        xsccp.image_count,
	        (unsigned long long)token,
	        info->width,
	        info->height,
	        info->array_size);

	struct xrt_swapchain *native_xsc = NULL;
	xret = xrt_comp_import_swapchain(&link->xcn->base,
	                                &native_info,
	                                transport_images,
	                                xsccp.image_count,
	                                &native_xsc);
	free(transport_images);

	if (xret != XRT_SUCCESS) {
		ipc_metal_xpc_discard_token(token);
		release_texture_array(textures, xsccp.image_count);
		return xret;
	}

	struct xrt_swapchain_native *xscn = (struct xrt_swapchain_native *)native_xsc;
	if (xscn->base.image_count != xsccp.image_count) {
		U_LOG_E("Metal service swapchain image-count mismatch: expected=%u actual=%u",
		        xsccp.image_count,
		        xscn->base.image_count);
		xrt_swapchain_native_reference(&xscn, NULL);
		release_texture_array(textures, xsccp.image_count);
		return XRT_ERROR_ALLOCATION;
	}

	struct metal_service_swapchain *sc = calloc(1, sizeof(*sc));
	if (sc == NULL) {
		xrt_swapchain_native_reference(&xscn, NULL);
		release_texture_array(textures, xsccp.image_count);
		return XRT_ERROR_ALLOCATION;
	}

	sc->base.base.destroy = metal_service_swapchain_destroy;
	sc->base.base.acquire_image = metal_service_swapchain_acquire_image;
	sc->base.base.wait_image = metal_service_swapchain_wait_image;
	sc->base.base.barrier_image = metal_service_swapchain_barrier_image;
	sc->base.base.release_image = metal_service_swapchain_release_image;
	sc->base.base.reference.count = 1;
	sc->base.base.image_count = xsccp.image_count;
	sc->xscn = xscn;
	sc->command_queue = [link->command_queue retain];

	for (uint32_t i = 0; i < xsccp.image_count; i++) {
		sc->base.images[i] = (__bridge void *)textures[i];
		textures[i] = nil;
	}
	free(textures);

	U_LOG_I("Metal service swapchain active: client Metal textures are shared with the Vulkan compositor service (array_size=%u images=%u)",
	        info->array_size,
	        xsccp.image_count);

	*out_xsc = &sc->base.base;
	return XRT_SUCCESS;
}

static void
metal_service_compositor_destroy(struct xrt_compositor *xc)
{
	struct metal_service_compositor_link *link = NULL;

	pthread_mutex_lock(&g_contexts_mutex);
	struct metal_service_compositor_link **ptr = &g_contexts;
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
		return NULL;
	}

	struct metal_service_compositor_link *link = calloc(1, sizeof(*link));
	if (link == NULL) {
		U_LOG_W("Could not allocate Metal service-swapchain wrapper; using ordinary Metal swapchains");
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
	xcm->base.create_swapchain = metal_service_create_swapchain;
	xcm->base.destroy = metal_service_compositor_destroy;
	pthread_mutex_unlock(&g_contexts_mutex);

	U_LOG_I("Metal service swapchain path installed: Metal-owned textures are transported through the XPC broker and imported directly into service Vulkan images");
	return xcm;
}

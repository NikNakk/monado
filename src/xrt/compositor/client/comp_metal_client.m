// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metal client side glue to compositor implementation.
 * @author OpenAI
 * @ingroup comp_client
 */

#import <Metal/Metal.h>

#include "client/comp_metal_client.h"

#include <assert.h>
#include <stdlib.h>


struct client_metal_compositor;

struct client_metal_swapchain
{
	struct xrt_swapchain_metal base;
	struct xrt_swapchain_native *xscn;
	struct client_metal_compositor *c;
};

struct client_metal_compositor
{
	struct xrt_compositor_metal base;
	struct xrt_compositor_native *xcn;
	id<MTLDevice> device;
};

static inline struct client_metal_swapchain *
client_metal_swapchain(struct xrt_swapchain *xsc)
{
	return (struct client_metal_swapchain *)xsc;
}

static inline struct client_metal_compositor *
client_metal_compositor(struct xrt_compositor *xc)
{
	return (struct client_metal_compositor *)xc;
}

static inline struct xrt_swapchain *
to_native_swapchain(struct xrt_swapchain *xsc)
{
	return &client_metal_swapchain(xsc)->xscn->base;
}

static inline struct xrt_compositor *
to_native_compositor(struct xrt_compositor *xc)
{
	return &client_metal_compositor(xc)->xcn->base;
}

static int64_t
vk_format_to_metal(uint32_t format)
{
	switch (format) {
	case 37: return MTLPixelFormatRGBA8Unorm;       // VK_FORMAT_R8G8B8A8_UNORM
	case 43: return MTLPixelFormatRGBA8Unorm_sRGB;  // VK_FORMAT_R8G8B8A8_SRGB
	case 44: return MTLPixelFormatBGRA8Unorm;       // VK_FORMAT_B8G8R8A8_UNORM
	case 50: return MTLPixelFormatBGRA8Unorm_sRGB;  // VK_FORMAT_B8G8R8A8_SRGB
	case 64: return MTLPixelFormatBGR10A2Unorm;     // VK_FORMAT_A2B10G10R10_UNORM_PACK32
	case 124: return MTLPixelFormatDepth16Unorm;    // VK_FORMAT_D16_UNORM
	case 126: return MTLPixelFormatDepth32Float;    // VK_FORMAT_D32_SFLOAT
	case 130: return MTLPixelFormatDepth32Float_Stencil8; // VK_FORMAT_D32_SFLOAT_S8_UINT
	default: return 0;
	}
}

static uint32_t
metal_format_to_vk(int64_t format)
{
	switch ((MTLPixelFormat)format) {
	case MTLPixelFormatRGBA8Unorm: return 37;      // VK_FORMAT_R8G8B8A8_UNORM
	case MTLPixelFormatRGBA8Unorm_sRGB: return 43; // VK_FORMAT_R8G8B8A8_SRGB
	case MTLPixelFormatBGRA8Unorm: return 44;      // VK_FORMAT_B8G8R8A8_UNORM
	case MTLPixelFormatBGRA8Unorm_sRGB: return 50; // VK_FORMAT_B8G8R8A8_SRGB
	case MTLPixelFormatBGR10A2Unorm: return 64;    // VK_FORMAT_A2B10G10R10_UNORM_PACK32
	case MTLPixelFormatDepth16Unorm: return 124;   // VK_FORMAT_D16_UNORM
	case MTLPixelFormatDepth32Float: return 126;   // VK_FORMAT_D32_SFLOAT
	case MTLPixelFormatDepth32Float_Stencil8: return 130; // VK_FORMAT_D32_SFLOAT_S8_UINT
	default: return 0;
	}
}

static MTLTextureUsage
usage_flags_to_metal(enum xrt_swapchain_usage_bits bits)
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

	return usage;
}

static void
client_metal_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct client_metal_swapchain *sc = client_metal_swapchain(xsc);

	for (uint32_t i = 0; i < sc->base.base.image_count; i++) {
		id<MTLTexture> texture = (__bridge id<MTLTexture>)sc->base.images[i];
		if (texture != nil) {
			[texture release];
			sc->base.images[i] = NULL;
		}
	}

	xrt_swapchain_native_reference(&sc->xscn, NULL);

	free(sc);
}

static xrt_result_t
client_metal_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	return xrt_swapchain_acquire_image(to_native_swapchain(xsc), out_index);
}

static xrt_result_t
client_metal_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	return xrt_swapchain_wait_image(to_native_swapchain(xsc), timeout_ns, index);
}

static xrt_result_t
client_metal_swapchain_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	struct xrt_swapchain *native_xsc = to_native_swapchain(xsc);
	if (native_xsc->barrier_image == NULL) {
		return XRT_SUCCESS;
	}

	return xrt_swapchain_barrier_image(native_xsc, direction, index);
}

static xrt_result_t
client_metal_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	return xrt_swapchain_release_image(to_native_swapchain(xsc), index);
}

static xrt_result_t
client_metal_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                                        const struct xrt_swapchain_create_info *info,
                                                        struct xrt_swapchain_create_properties *xsccp)
{
	struct xrt_swapchain_create_info native_info = *info;
	native_info.format = metal_format_to_vk(info->format);
	if (native_info.format == 0) {
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}

	return xrt_comp_get_swapchain_create_properties(to_native_compositor(xc), &native_info, xsccp);
}

static xrt_result_t
client_metal_compositor_create_swapchain(struct xrt_compositor *xc,
                                         const struct xrt_swapchain_create_info *info,
                                         struct xrt_swapchain **out_xsc)
{
	struct client_metal_compositor *c = client_metal_compositor(xc);
	struct xrt_swapchain_create_properties xsccp = XRT_STRUCT_INIT;
	xrt_result_t xret = xrt_comp_get_swapchain_create_properties(&c->xcn->base, info, &xsccp);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	if (info->array_size != 1 || info->face_count != 1) {
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}

	uint32_t vk_format = metal_format_to_vk(info->format);
	if (vk_format == 0) {
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}

	struct xrt_swapchain_create_info native_info = *info;
	native_info.format = vk_format;
	native_info.bits |= xsccp.extra_bits;

	struct xrt_swapchain_native *xscn = NULL;
	xret = xrt_comp_native_create_swapchain(c->xcn, &native_info, &xscn);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	struct client_metal_swapchain *sc = calloc(1, sizeof(*sc));
	if (sc == NULL) {
		xrt_swapchain_native_reference(&xscn, NULL);
		return XRT_ERROR_ALLOCATION;
	}

	sc->base.base.destroy = client_metal_swapchain_destroy;
	sc->base.base.acquire_image = client_metal_swapchain_acquire_image;
	sc->base.base.wait_image = client_metal_swapchain_wait_image;
	sc->base.base.barrier_image = client_metal_swapchain_barrier_image;
	sc->base.base.release_image = client_metal_swapchain_release_image;
	sc->base.base.reference.count = 1;
	sc->base.base.image_count = xscn->base.image_count;
	sc->xscn = xscn;
	sc->c = c;

	MTLTextureDescriptor *descriptor =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)info->format
	                                                      width:info->width
	                                                     height:info->height
	                                                  mipmapped:info->mip_count > 1];
	descriptor.usage = usage_flags_to_metal(info->bits);

	for (uint32_t i = 0; i < xscn->base.image_count; i++) {
		IOSurfaceRef surface = xscn->images[i].handle;
		if (!xrt_graphics_buffer_is_valid(surface)) {
			client_metal_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_ALLOCATION;
		}

		id<MTLTexture> texture = [c->device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
		if (texture == nil) {
			client_metal_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_ALLOCATION;
		}

		sc->base.images[i] = texture;
	}

	*out_xsc = &sc->base.base;
	return XRT_SUCCESS;
}

static xrt_result_t
client_metal_compositor_import_swapchain(struct xrt_compositor *xc,
                                         const struct xrt_swapchain_create_info *info,
                                         struct xrt_image_native *native_images,
                                         uint32_t image_count,
                                         struct xrt_swapchain **out_xsc)
{
	(void)xc;
	(void)info;
	(void)native_images;
	(void)image_count;
	(void)out_xsc;
	return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
}

static xrt_result_t
client_metal_compositor_import_fence(struct xrt_compositor *xc,
                                     xrt_graphics_sync_handle_t handle,
                                     struct xrt_compositor_fence **out_xcf)
{
	return xrt_comp_import_fence(to_native_compositor(xc), handle, out_xcf);
}

static xrt_result_t
client_metal_compositor_create_semaphore(struct xrt_compositor *xc,
                                         xrt_graphics_sync_handle_t *out_handle,
                                         struct xrt_compositor_semaphore **out_xcsem)
{
	return xrt_comp_create_semaphore(to_native_compositor(xc), out_handle, out_xcsem);
}

static xrt_result_t
client_metal_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	return xrt_comp_begin_session(to_native_compositor(xc), info);
}

static xrt_result_t
client_metal_compositor_end_session(struct xrt_compositor *xc)
{
	return xrt_comp_end_session(to_native_compositor(xc));
}

static xrt_result_t
client_metal_compositor_predict_frame(struct xrt_compositor *xc,
                                      int64_t *out_frame_id,
                                      int64_t *out_wake_time_ns,
                                      int64_t *out_predicted_gpu_time_ns,
                                      int64_t *out_predicted_display_time_ns,
                                      int64_t *out_predicted_display_period_ns)
{
	return xrt_comp_predict_frame(to_native_compositor(xc), out_frame_id, out_wake_time_ns,
	                              out_predicted_gpu_time_ns, out_predicted_display_time_ns,
	                              out_predicted_display_period_ns);
}

static xrt_result_t
client_metal_compositor_mark_frame(struct xrt_compositor *xc,
                                   int64_t frame_id,
                                   enum xrt_compositor_frame_point point,
                                   int64_t when_ns)
{
	return xrt_comp_mark_frame(to_native_compositor(xc), frame_id, point, when_ns);
}

static xrt_result_t
client_metal_compositor_wait_frame(struct xrt_compositor *xc,
                                   int64_t *out_frame_id,
                                   int64_t *out_predicted_display_time,
                                   int64_t *out_predicted_display_period)
{
	return xrt_comp_wait_frame(to_native_compositor(xc), out_frame_id, out_predicted_display_time,
	                           out_predicted_display_period);
}

static xrt_result_t
client_metal_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	return xrt_comp_begin_frame(to_native_compositor(xc), frame_id);
}

static xrt_result_t
client_metal_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	return xrt_comp_discard_frame(to_native_compositor(xc), frame_id);
}

static xrt_result_t
client_metal_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	return xrt_comp_layer_begin(to_native_compositor(xc), data);
}

static xrt_result_t
client_metal_compositor_layer_projection(struct xrt_compositor *xc,
                                         struct xrt_device *xdev,
                                         struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                         const struct xrt_layer_data *data)
{
	struct xrt_swapchain *xscn[XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < data->view_count; i++) {
		xscn[i] = to_native_swapchain(xsc[i]);
	}
	return xrt_comp_layer_projection(to_native_compositor(xc), xdev, xscn, data);
}

static xrt_result_t
client_metal_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                               struct xrt_device *xdev,
                                               struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                               struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                               const struct xrt_layer_data *data)
{
	struct xrt_swapchain *xscn[XRT_MAX_VIEWS];
	struct xrt_swapchain *d_xscn[XRT_MAX_VIEWS];
	for (uint32_t i = 0; i < data->view_count; i++) {
		xscn[i] = to_native_swapchain(xsc[i]);
		d_xscn[i] = to_native_swapchain(d_xsc[i]);
	}
	return xrt_comp_layer_projection_depth(to_native_compositor(xc), xdev, xscn, d_xscn, data);
}

static xrt_result_t
client_metal_compositor_layer_quad(struct xrt_compositor *xc,
                                   struct xrt_device *xdev,
                                   struct xrt_swapchain *xsc,
                                   const struct xrt_layer_data *data)
{
	return xrt_comp_layer_quad(to_native_compositor(xc), xdev, to_native_swapchain(xsc), data);
}

static xrt_result_t
client_metal_compositor_layer_cube(struct xrt_compositor *xc,
                                   struct xrt_device *xdev,
                                   struct xrt_swapchain *xsc,
                                   const struct xrt_layer_data *data)
{
	return xrt_comp_layer_cube(to_native_compositor(xc), xdev, to_native_swapchain(xsc), data);
}

static xrt_result_t
client_metal_compositor_layer_cylinder(struct xrt_compositor *xc,
                                       struct xrt_device *xdev,
                                       struct xrt_swapchain *xsc,
                                       const struct xrt_layer_data *data)
{
	return xrt_comp_layer_cylinder(to_native_compositor(xc), xdev, to_native_swapchain(xsc), data);
}

static xrt_result_t
client_metal_compositor_layer_equirect1(struct xrt_compositor *xc,
                                        struct xrt_device *xdev,
                                        struct xrt_swapchain *xsc,
                                        const struct xrt_layer_data *data)
{
	return xrt_comp_layer_equirect1(to_native_compositor(xc), xdev, to_native_swapchain(xsc), data);
}

static xrt_result_t
client_metal_compositor_layer_equirect2(struct xrt_compositor *xc,
                                        struct xrt_device *xdev,
                                        struct xrt_swapchain *xsc,
                                        const struct xrt_layer_data *data)
{
	return xrt_comp_layer_equirect2(to_native_compositor(xc), xdev, to_native_swapchain(xsc), data);
}

static xrt_result_t
client_metal_compositor_layer_passthrough(struct xrt_compositor *xc,
                                          struct xrt_device *xdev,
                                          const struct xrt_layer_data *data)
{
	return xrt_comp_layer_passthrough(to_native_compositor(xc), xdev, data);
}

static xrt_result_t
client_metal_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	return xrt_comp_layer_commit(to_native_compositor(xc), sync_handle);
}

static xrt_result_t
client_metal_compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                                    struct xrt_compositor_semaphore *xcsem,
                                                    uint64_t value)
{
	return xrt_comp_layer_commit_with_semaphore(to_native_compositor(xc), xcsem, value);
}

static xrt_result_t
client_metal_compositor_get_display_refresh_rate(struct xrt_compositor *xc, float *out_display_refresh_rate_hz)
{
	return xrt_comp_get_display_refresh_rate(to_native_compositor(xc), out_display_refresh_rate_hz);
}

static xrt_result_t
client_metal_compositor_request_display_refresh_rate(struct xrt_compositor *xc, float display_refresh_rate_hz)
{
	return xrt_comp_request_display_refresh_rate(to_native_compositor(xc), display_refresh_rate_hz);
}

static xrt_result_t
client_metal_compositor_set_performance_level(struct xrt_compositor *xc,
                                              enum xrt_perf_domain domain,
                                              enum xrt_perf_set_level level)
{
	return xrt_comp_set_performance_level(to_native_compositor(xc), domain, level);
}

static xrt_result_t
client_metal_compositor_get_reference_bounds_rect(struct xrt_compositor *xc,
                                                  enum xrt_reference_space_type reference_space_type,
                                                  struct xrt_vec2 *bounds)
{
	return xrt_comp_get_reference_bounds_rect(to_native_compositor(xc), reference_space_type, bounds);
}

static xrt_result_t
client_metal_compositor_create_passthrough(struct xrt_compositor *xc, const struct xrt_passthrough_create_info *info)
{
	return xrt_comp_create_passthrough(to_native_compositor(xc), info);
}

static xrt_result_t
client_metal_compositor_create_passthrough_layer(struct xrt_compositor *xc,
                                                 const struct xrt_passthrough_layer_create_info *info)
{
	return xrt_comp_create_passthrough_layer(to_native_compositor(xc), info);
}

static xrt_result_t
client_metal_compositor_destroy_passthrough(struct xrt_compositor *xc)
{
	return xrt_comp_destroy_passthrough(to_native_compositor(xc));
}

static xrt_result_t
client_metal_compositor_set_thread_hint(struct xrt_compositor *xc, enum xrt_thread_hint hint, uint32_t thread_id)
{
	return xrt_comp_set_thread_hint(to_native_compositor(xc), hint, thread_id);
}

static void
client_metal_compositor_destroy(struct xrt_compositor *xc)
{
	struct client_metal_compositor *c = client_metal_compositor(xc);
	free(c);
}

struct xrt_compositor_metal *
client_metal_compositor_create(struct xrt_compositor_native *xcn, void *metal_device, void *command_queue)
{
	(void)command_queue;

	if (xcn == NULL || metal_device == NULL) {
		return NULL;
	}

	struct client_metal_compositor *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return NULL;
	}

	c->xcn = xcn;
	c->device = (__bridge id<MTLDevice>)metal_device;

	c->base.base.get_swapchain_create_properties = client_metal_compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = client_metal_compositor_create_swapchain;
	c->base.base.import_swapchain = client_metal_compositor_import_swapchain;
	c->base.base.import_fence = client_metal_compositor_import_fence;
	c->base.base.create_semaphore = client_metal_compositor_create_semaphore;
	c->base.base.create_passthrough = client_metal_compositor_create_passthrough;
	c->base.base.create_passthrough_layer = client_metal_compositor_create_passthrough_layer;
	c->base.base.destroy_passthrough = client_metal_compositor_destroy_passthrough;
	c->base.base.begin_session = client_metal_compositor_begin_session;
	c->base.base.end_session = client_metal_compositor_end_session;
	c->base.base.predict_frame = client_metal_compositor_predict_frame;
	c->base.base.mark_frame = client_metal_compositor_mark_frame;
	c->base.base.wait_frame = client_metal_compositor_wait_frame;
	c->base.base.begin_frame = client_metal_compositor_begin_frame;
	c->base.base.discard_frame = client_metal_compositor_discard_frame;
	c->base.base.layer_begin = client_metal_compositor_layer_begin;
	c->base.base.layer_projection = client_metal_compositor_layer_projection;
	c->base.base.layer_projection_depth = client_metal_compositor_layer_projection_depth;
	c->base.base.layer_quad = client_metal_compositor_layer_quad;
	c->base.base.layer_cube = client_metal_compositor_layer_cube;
	c->base.base.layer_cylinder = client_metal_compositor_layer_cylinder;
	c->base.base.layer_equirect1 = client_metal_compositor_layer_equirect1;
	c->base.base.layer_equirect2 = client_metal_compositor_layer_equirect2;
	c->base.base.layer_passthrough = client_metal_compositor_layer_passthrough;
	c->base.base.layer_commit = client_metal_compositor_layer_commit;
	c->base.base.layer_commit_with_semaphore = client_metal_compositor_layer_commit_with_semaphore;
	c->base.base.get_display_refresh_rate = client_metal_compositor_get_display_refresh_rate;
	c->base.base.request_display_refresh_rate = client_metal_compositor_request_display_refresh_rate;
	c->base.base.set_performance_level = client_metal_compositor_set_performance_level;
	c->base.base.get_reference_bounds_rect = client_metal_compositor_get_reference_bounds_rect;
	c->base.base.destroy = client_metal_compositor_destroy;
	c->base.base.set_thread_hint = client_metal_compositor_set_thread_hint;
	c->base.base.info.max_texture_size = xcn->base.info.max_texture_size;

	for (uint32_t i = 0; i < xcn->base.info.format_count; i++) {
		int64_t format = vk_format_to_metal((uint32_t)xcn->base.info.formats[i]);
		if (format == 0) {
			continue;
		}

		c->base.base.info.formats[c->base.base.info.format_count++] = format;
	}

	if (c->base.base.info.format_count == 0) {
		client_metal_compositor_destroy(&c->base.base);
		return NULL;
	}

	return &c->base;
}

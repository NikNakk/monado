// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Minimal D3D11 client compositor for Wine/DXMT on macOS.
 *
 * This intentionally avoids WIL and Windows shared-handle import. D3D11
 * textures are created on the application's DXMT device, their Basalt
 * IOSurface IDs are imported into the native macOS Monado service, and a local
 * D3D11 fence establishes producer completion before layer commit.
 */

#include "client/comp_d3d11_client.h"
#include "client/ipc_client.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <windows.h>

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GUID kBasaltIOSurfaceIdGuid = {
    0xdd807311, 0x529e, 0x4856, {0xa5, 0xc0, 0x48, 0xbe, 0xd2, 0x04, 0x81, 0x29}};

/*
 * Private-data GUID added by the Monado v0.80 DXMT patch. Shared D3D11 fences
 * expose the bootstrap registration name of their backing MTLSharedEvent.
 */
static const GUID kMonadoSharedFenceBootstrapNameGuid = {
    0x8a1e78d5, 0x9762, 0x4f7a, {0xb0, 0xad, 0x1d, 0x62, 0xf6, 0xa4, 0x9d, 0x31}};

struct client_d3d11_compositor
{
	struct xrt_compositor_d3d11 base;
	struct xrt_compositor_native *xcn;
	ID3D11Device *device;
	ID3D11DeviceContext *context;
	ID3D11DeviceContext4 *context4;
	ID3D11Fence *fence;
	HANDLE fence_event;
	uint64_t fence_value;
	struct xrt_compositor_semaphore *gpu_semaphore;
	bool gpu_sync_active;

	LARGE_INTEGER qpc_frequency;
	FILE *timing_trace;
	int64_t timing_frame_id;
	double last_wait_frame_us;
};

struct client_d3d11_swapchain
{
	struct xrt_swapchain_d3d11 base;
	struct xrt_swapchain *native;
	struct client_d3d11_compositor *c;
};


static double
qpc_elapsed_us(struct client_d3d11_compositor *c, LARGE_INTEGER start, LARGE_INTEGER end)
{
	if (c->qpc_frequency.QuadPart <= 0) {
		return 0.0;
	}
	return ((double)(end.QuadPart - start.QuadPart) * 1000000.0) / (double)c->qpc_frequency.QuadPart;
}

static LARGE_INTEGER
qpc_now(void)
{
	LARGE_INTEGER now = {};
	QueryPerformanceCounter(&now);
	return now;
}

static void
trace_frame_timing(struct client_d3d11_compositor *c,
                   double producer_wait_us,
                   double ipc_commit_us,
                   double layer_commit_total_us)
{
	if (c->timing_trace == NULL) {
		return;
	}

	fprintf(c->timing_trace,
	        "%" PRId64 ",%" PRIu64 ",%u,%.3f,%.3f,%.3f,%.3f\n",
	        c->timing_frame_id,
	        c->fence_value,
	        c->gpu_sync_active ? 1u : 0u,
	        c->last_wait_frame_us,
	        producer_wait_us,
	        ipc_commit_us,
	        layer_commit_total_us);
	fflush(c->timing_trace);
}

static void
init_timing_trace(struct client_d3d11_compositor *c)
{
	QueryPerformanceFrequency(&c->qpc_frequency);
	c->timing_frame_id = -1;

	char path[2048] = {};
	DWORD len = GetEnvironmentVariableA("MONADO_WINE_TIMING_TRACE", path, (DWORD)sizeof(path));
	if (len == 0 || len >= sizeof(path) || strcmp(path, "0") == 0) {
		return;
	}

	c->timing_trace = fopen(path, "w");
	if (c->timing_trace == NULL) {
		U_LOG_W("Could not open Wine timing trace '%s'", path);
		return;
	}

	fprintf(c->timing_trace,
	        "frame_id,fence_value,gpu_sync,wait_frame_us,producer_wait_us,ipc_commit_us,layer_commit_total_us\n");
	fflush(c->timing_trace);
	U_LOG_I("Wine D3D11 timing trace: %s", path);
}

static struct client_d3d11_compositor *
as_compositor(struct xrt_compositor *xc)
{
	return (struct client_d3d11_compositor *)xc;
}

static struct client_d3d11_swapchain *
as_swapchain(struct xrt_swapchain *xsc)
{
	return (struct client_d3d11_swapchain *)xsc;
}

static struct xrt_compositor *
native_compositor(struct xrt_compositor *xc)
{
	return &as_compositor(xc)->xcn->base;
}

static struct xrt_swapchain *
native_swapchain(struct xrt_swapchain *xsc)
{
	return as_swapchain(xsc)->native;
}

/*
 * Vulkan wire-format values used by native Monado. Keep the PE client free of
 * a Vulkan loader/runtime dependency: these four VkFormat values are stable
 * API enum values from the Vulkan specification.
 */
enum wine_bridge_vk_format
{
	WINE_VK_FORMAT_R8G8B8A8_UNORM = 37,
	WINE_VK_FORMAT_R8G8B8A8_SRGB = 43,
	WINE_VK_FORMAT_B8G8R8A8_UNORM = 44,
	WINE_VK_FORMAT_B8G8R8A8_SRGB = 50,
};

static int64_t
dxgi_to_vk(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_B8G8R8A8_UNORM: return WINE_VK_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return WINE_VK_FORMAT_B8G8R8A8_SRGB;
	case DXGI_FORMAT_R8G8B8A8_UNORM: return WINE_VK_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return WINE_VK_FORMAT_R8G8B8A8_SRGB;
	default: return 0;
	}
}

static DXGI_FORMAT
vk_to_dxgi(int64_t format)
{
	switch (format) {
	case WINE_VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case WINE_VK_FORMAT_B8G8R8A8_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case WINE_VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case WINE_VK_FORMAT_R8G8B8A8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

static xrt_result_t
swapchain_acquire(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	return xrt_swapchain_acquire_image(native_swapchain(xsc), out_index);
}

static xrt_result_t
swapchain_wait(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	return xrt_swapchain_wait_image(native_swapchain(xsc), timeout_ns, index);
}

static xrt_result_t
swapchain_barrier(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	(void)xsc;
	(void)direction;
	(void)index;
	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_release(struct xrt_swapchain *xsc, uint32_t index)
{
	return xrt_swapchain_release_image(native_swapchain(xsc), index);
}

static void
swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct client_d3d11_swapchain *sc = as_swapchain(xsc);
	for (uint32_t i = 0; i < sc->base.base.image_count; ++i) {
		if (sc->base.images[i] != NULL) {
			sc->base.images[i]->Release();
			sc->base.images[i] = NULL;
		}
	}
	xrt_swapchain_reference(&sc->native, NULL);
	free(sc);
}

static xrt_result_t
get_swapchain_create_properties(struct xrt_compositor *xc,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_swapchain_create_properties *out)
{
	int64_t vk_format = dxgi_to_vk((DXGI_FORMAT)info->format);
	if (vk_format == 0) {
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}
	struct xrt_swapchain_create_info native_info = *info;
	native_info.format = vk_format;
	return xrt_comp_get_swapchain_create_properties(native_compositor(xc), &native_info, out);
}

static UINT
usage_to_bind_flags(enum xrt_swapchain_usage_bits bits)
{
	UINT flags = 0;
	if ((bits & XRT_SWAPCHAIN_USAGE_COLOR) != 0) {
		flags |= D3D11_BIND_RENDER_TARGET;
	}
	if ((bits & XRT_SWAPCHAIN_USAGE_SAMPLED) != 0) {
		flags |= D3D11_BIND_SHADER_RESOURCE;
	}
	if ((bits & XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS) != 0) {
		flags |= D3D11_BIND_UNORDERED_ACCESS;
	}
	return flags;
}

static xrt_result_t
create_swapchain(struct xrt_compositor *xc,
                 const struct xrt_swapchain_create_info *info,
                 struct xrt_swapchain **out_xsc)
{
	struct client_d3d11_compositor *c = as_compositor(xc);
	if (info == NULL || out_xsc == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	if (info->array_size != 1 || info->face_count != 1 || info->mip_count != 1 || info->sample_count != 1) {
		U_LOG_W("Wine D3D11 bridge currently supports simple 2D swapchains only");
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}
	if ((info->bits & XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL) != 0) {
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}

	int64_t vk_format = dxgi_to_vk((DXGI_FORMAT)info->format);
	if (vk_format == 0) {
		return XRT_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	}

	struct xrt_swapchain_create_info native_info = *info;
	native_info.format = vk_format;
	struct xrt_swapchain_create_properties props = {0};
	xrt_result_t xret = xrt_comp_get_swapchain_create_properties(&c->xcn->base, &native_info, &props);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	if (props.image_count == 0 || props.image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return XRT_ERROR_ALLOCATION;
	}
	native_info.bits = (enum xrt_swapchain_usage_bits)(native_info.bits | props.extra_bits);

	struct client_d3d11_swapchain *sc =
	    (struct client_d3d11_swapchain *)calloc(1, sizeof(struct client_d3d11_swapchain));
	if (sc == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	sc->c = c;

	uint32_t ids[XRT_MAX_SWAPCHAIN_IMAGES] = {0};
	for (uint32_t i = 0; i < props.image_count; ++i) {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = info->width;
		desc.Height = info->height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = (DXGI_FORMAT)info->format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = usage_to_bind_flags(native_info.bits);
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

		ID3D11Texture2D *texture = NULL;
		HRESULT hr = c->device->CreateTexture2D(&desc, NULL, &texture);
		if (FAILED(hr) || texture == NULL) {
			U_LOG_E("Wine D3D11 CreateTexture2D failed image=%u hr=0x%08lx", i, (unsigned long)hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_D3D11;
		}

		UINT size = sizeof(ids[i]);
		hr = texture->GetPrivateData(kBasaltIOSurfaceIdGuid, &size, &ids[i]);
		if (FAILED(hr) || size != sizeof(ids[i]) || ids[i] == 0) {
			U_LOG_E("DXMT texture did not expose a Basalt IOSurface ID: image=%u hr=0x%08lx size=%u id=%u",
			        i, (unsigned long)hr, size, ids[i]);
			texture->Release();
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_D3D11;
		}

		sc->base.images[i] = texture;
	}

	xret = ipc_client_compositor_import_iosurface_ids(c->xcn, &native_info, props.image_count, ids, &sc->native);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Wine D3D11 IOSurface import failed: result=%d", xret);
		swapchain_destroy(&sc->base.base);
		return xret;
	}

	sc->base.base.destroy = swapchain_destroy;
	sc->base.base.acquire_image = swapchain_acquire;
	sc->base.base.wait_image = swapchain_wait;
	sc->base.base.barrier_image = swapchain_barrier;
	sc->base.base.release_image = swapchain_release;
	sc->base.base.reference.count = 1;
	sc->base.base.image_count = props.image_count;

	U_LOG_I("Wine D3D11 swapchain imported: images=%u size=%ux%u dxgi_format=%lld ids=%u,%u,%u",
	        props.image_count, info->width, info->height, (long long)info->format,
	        ids[0], props.image_count > 1 ? ids[1] : 0, props.image_count > 2 ? ids[2] : 0);

	*out_xsc = &sc->base.base;
	return XRT_SUCCESS;
}

static xrt_result_t
begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	return xrt_comp_begin_session(native_compositor(xc), info);
}
static xrt_result_t end_session(struct xrt_compositor *xc) { return xrt_comp_end_session(native_compositor(xc)); }
static xrt_result_t
wait_frame(struct xrt_compositor *xc, int64_t *id, int64_t *display, int64_t *period)
{
	struct client_d3d11_compositor *c = as_compositor(xc);
	LARGE_INTEGER start = qpc_now();
	xrt_result_t xret = xrt_comp_wait_frame(native_compositor(xc), id, display, period);
	LARGE_INTEGER end = qpc_now();
	c->last_wait_frame_us = qpc_elapsed_us(c, start, end);
	if (xret == XRT_SUCCESS && id != NULL) {
		c->timing_frame_id = *id;
	}
	return xret;
}
static xrt_result_t begin_frame(struct xrt_compositor *xc, int64_t id) { return xrt_comp_begin_frame(native_compositor(xc), id); }
static xrt_result_t discard_frame(struct xrt_compositor *xc, int64_t id) { return xrt_comp_discard_frame(native_compositor(xc), id); }
static xrt_result_t
layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	return xrt_comp_layer_begin(native_compositor(xc), data);
}

static xrt_result_t
layer_projection(struct xrt_compositor *xc,
                 struct xrt_device *xdev,
                 struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                 const struct xrt_layer_data *data)
{
	struct xrt_swapchain *native[XRT_MAX_VIEWS] = {0};
	for (uint32_t i = 0; i < data->view_count; ++i) {
		native[i] = native_swapchain(xsc[i]);
	}
	return xrt_comp_layer_projection(native_compositor(xc), xdev, native, data);
}

static xrt_result_t
layer_projection_depth(struct xrt_compositor *xc,
                       struct xrt_device *xdev,
                       struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                       struct xrt_swapchain *depth[XRT_MAX_VIEWS],
                       const struct xrt_layer_data *data)
{
	struct xrt_swapchain *native[XRT_MAX_VIEWS] = {0};
	struct xrt_swapchain *native_depth[XRT_MAX_VIEWS] = {0};
	for (uint32_t i = 0; i < data->view_count; ++i) {
		native[i] = native_swapchain(xsc[i]);
		native_depth[i] = native_swapchain(depth[i]);
	}
	return xrt_comp_layer_projection_depth(native_compositor(xc), xdev, native, native_depth, data);
}

static xrt_result_t
layer_quad(struct xrt_compositor *xc, struct xrt_device *xdev, struct xrt_swapchain *xsc, const struct xrt_layer_data *data)
{
	return xrt_comp_layer_quad(native_compositor(xc), xdev, native_swapchain(xsc), data);
}
static xrt_result_t
layer_cube(struct xrt_compositor *xc, struct xrt_device *xdev, struct xrt_swapchain *xsc, const struct xrt_layer_data *data)
{
	return xrt_comp_layer_cube(native_compositor(xc), xdev, native_swapchain(xsc), data);
}
static xrt_result_t
layer_cylinder(struct xrt_compositor *xc, struct xrt_device *xdev, struct xrt_swapchain *xsc, const struct xrt_layer_data *data)
{
	return xrt_comp_layer_cylinder(native_compositor(xc), xdev, native_swapchain(xsc), data);
}
static xrt_result_t
layer_equirect1(struct xrt_compositor *xc, struct xrt_device *xdev, struct xrt_swapchain *xsc, const struct xrt_layer_data *data)
{
	return xrt_comp_layer_equirect1(native_compositor(xc), xdev, native_swapchain(xsc), data);
}
static xrt_result_t
layer_equirect2(struct xrt_compositor *xc, struct xrt_device *xdev, struct xrt_swapchain *xsc, const struct xrt_layer_data *data)
{
	return xrt_comp_layer_equirect2(native_compositor(xc), xdev, native_swapchain(xsc), data);
}
static xrt_result_t
layer_passthrough(struct xrt_compositor *xc, struct xrt_device *xdev, const struct xrt_layer_data *data)
{
	return xrt_comp_layer_passthrough(native_compositor(xc), xdev, data);
}

static xrt_result_t
signal_producer(struct client_d3d11_compositor *c, uint64_t *out_value)
{
	if (c->fence == NULL || c->context4 == NULL) {
		return XRT_ERROR_D3D11;
	}

	const uint64_t value = ++c->fence_value;
	HRESULT hr = c->context4->Signal(c->fence, value);
	if (FAILED(hr)) {
		return XRT_ERROR_D3D11;
	}
	/*
	 * DXMT's immediate-context Signal() already invalidates the current pass,
	 * enqueues the MTLSharedEvent signal, and calls Flush() itself. A second
	 * Flush() here needlessly commits an empty/next chunk every frame.
	 */
	if (out_value != NULL) {
		*out_value = value;
	}
	return XRT_SUCCESS;
}

static xrt_result_t
wait_for_producer(struct client_d3d11_compositor *c)
{
	if (c->fence == NULL || c->context4 == NULL || c->fence_event == NULL) {
		c->context->Flush();
		U_LOG_E("Wine D3D11 bridge has no fence support");
		return XRT_ERROR_D3D11;
	}

	uint64_t value = 0;
	xrt_result_t xret = signal_producer(c, &value);
	if (xret != XRT_SUCCESS) {
		return xret;
	}
	HRESULT hr = c->fence->SetEventOnCompletion(value, c->fence_event);
	if (FAILED(hr)) {
		return XRT_ERROR_D3D11;
	}

	DWORD result = WaitForSingleObject(c->fence_event, 5000);
	return result == WAIT_OBJECT_0 ? XRT_SUCCESS : XRT_ERROR_D3D11;
}

static xrt_result_t
layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct client_d3d11_compositor *c = as_compositor(xc);
	(void)sync_handle;

	LARGE_INTEGER total_start = qpc_now();
	LARGE_INTEGER producer_start = total_start;
	xrt_result_t xret;
	uint64_t signal_value = 0;

	if (c->gpu_sync_active && c->gpu_semaphore != NULL) {
		xret = signal_producer(c, &signal_value);
	} else {
		xret = wait_for_producer(c);
		signal_value = c->fence_value;
	}

	LARGE_INTEGER producer_end = qpc_now();
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Wine D3D11 producer synchronization failed: %d", xret);
		return xret;
	}

	LARGE_INTEGER ipc_start = qpc_now();
	if (c->gpu_sync_active && c->gpu_semaphore != NULL) {
		xret = xrt_comp_layer_commit_with_semaphore(&c->xcn->base, c->gpu_semaphore, signal_value);
	} else {
		xret = xrt_comp_layer_commit(&c->xcn->base, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
	}
	LARGE_INTEGER ipc_end = qpc_now();

	double producer_us = qpc_elapsed_us(c, producer_start, producer_end);
	double ipc_us = qpc_elapsed_us(c, ipc_start, ipc_end);
	double total_us = qpc_elapsed_us(c, total_start, ipc_end);
	trace_frame_timing(c, producer_us, ipc_us, total_us);

	if (producer_us > 1500.0 || ipc_us > 1500.0) {
		U_LOG_W("Wine frame stall: frame=%" PRId64 " producer=%.3fus ipc=%.3fus total=%.3fus",
		        c->timing_frame_id, producer_us, ipc_us, total_us);
	}

	return xret;
}

static xrt_result_t
set_thread_hint(struct xrt_compositor *xc, enum xrt_thread_hint hint, uint32_t thread_id)
{
	return xrt_comp_set_thread_hint(native_compositor(xc), hint, thread_id);
}
static xrt_result_t
get_refresh(struct xrt_compositor *xc, float *hz)
{
	return xrt_comp_get_display_refresh_rate(native_compositor(xc), hz);
}
static xrt_result_t
request_refresh(struct xrt_compositor *xc, float hz)
{
	return xrt_comp_request_display_refresh_rate(native_compositor(xc), hz);
}
static xrt_result_t
set_performance(struct xrt_compositor *xc, enum xrt_perf_domain domain, enum xrt_perf_set_level level)
{
	return xrt_comp_set_performance_level(native_compositor(xc), domain, level);
}
static xrt_result_t
get_bounds(struct xrt_compositor *xc, enum xrt_reference_space_type type, struct xrt_vec2 *bounds)
{
	return xrt_comp_get_reference_bounds_rect(native_compositor(xc), type, bounds);
}

static void
destroy_compositor(struct xrt_compositor *xc)
{
	struct client_d3d11_compositor *c = as_compositor(xc);
	if (c->timing_trace != NULL) {
		fclose(c->timing_trace);
		c->timing_trace = NULL;
	}
	if (c->gpu_semaphore != NULL) {
		xrt_compositor_semaphore_reference(&c->gpu_semaphore, NULL);
	}
	if (c->fence_event != NULL) CloseHandle(c->fence_event);
	if (c->fence != NULL) c->fence->Release();
	if (c->context4 != NULL) c->context4->Release();
	if (c->context != NULL) c->context->Release();
	if (c->device != NULL) c->device->Release();
	free(c);
}

struct xrt_compositor_d3d11 *
client_d3d11_compositor_create(struct xrt_compositor_native *xcn, ID3D11Device *device)
{
	if (xcn == NULL || device == NULL) return NULL;

	struct client_d3d11_compositor *c =
	    (struct client_d3d11_compositor *)calloc(1, sizeof(struct client_d3d11_compositor));
	if (c == NULL) return NULL;

	c->xcn = xcn;
	c->device = device;
	init_timing_trace(c);
	c->device->AddRef();
	c->device->GetImmediateContext(&c->context);
	if (c->context == NULL) {
		destroy_compositor(&c->base.base);
		return NULL;
	}

	HRESULT hr = c->context->QueryInterface(IID_ID3D11DeviceContext4, (void **)&c->context4);
	if (SUCCEEDED(hr)) {
		ID3D11Device5 *device5 = NULL;
		hr = c->device->QueryInterface(IID_ID3D11Device5, (void **)&device5);
		if (SUCCEEDED(hr) && device5 != NULL) {
			/*
			 * DXMT shared fences are backed by bootstrap-registered
			 * MTLSharedEvents. The Monado DXMT patch exposes that bootstrap
			 * name through GetPrivateData so native Monado can import the same
			 * event as a Vulkan timeline semaphore.
			 */
			hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_ID3D11Fence, (void **)&c->fence);
			if (FAILED(hr) || c->fence == NULL) {
				U_LOG_W("DXMT shared D3D11 fence unavailable (hr=0x%08lx); falling back to local fence",
				        (unsigned long)hr);
				hr = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_ID3D11Fence, (void **)&c->fence);
			}
			device5->Release();

			if (SUCCEEDED(hr) && c->fence != NULL) {
				c->fence_event = CreateEventW(NULL, FALSE, FALSE, NULL);

				char gpu_sync_env[16] = {};
				DWORD env_len = GetEnvironmentVariableA(
				    "MONADO_WINE_GPU_SYNC", gpu_sync_env, (DWORD)sizeof(gpu_sync_env));
				bool allow_gpu_sync = !(env_len > 0 && strcmp(gpu_sync_env, "0") == 0);

				if (allow_gpu_sync) {
					char bootstrap_name[IPC_METAL_BOOTSTRAP_NAME_SIZE] = {};
					UINT bootstrap_size = sizeof(bootstrap_name);
					HRESULT private_hr = c->fence->GetPrivateData(
					    kMonadoSharedFenceBootstrapNameGuid, &bootstrap_size, bootstrap_name);
					if (SUCCEEDED(private_hr) && bootstrap_size > 0 &&
					    bootstrap_size <= sizeof(bootstrap_name)) {
						bootstrap_name[sizeof(bootstrap_name) - 1] = '\0';
						xrt_result_t sync_ret =
						    ipc_client_compositor_import_metal_bootstrap_semaphore(
						        c->xcn, bootstrap_name, &c->gpu_semaphore);
						if (sync_ret == XRT_SUCCESS && c->gpu_semaphore != NULL) {
							c->gpu_sync_active = true;
							U_LOG_I("Wine D3D11 GPU-only shared-event sync active: '%s'",
							        bootstrap_name);
						} else {
							U_LOG_W("Native DXMT shared-event import failed: result=%d; CPU fence fallback active",
							        sync_ret);
						}
					} else {
						U_LOG_I("DXMT fence does not expose Monado shared-event metadata; CPU fence fallback active");
					}
				} else {
					U_LOG_I("Wine D3D11 GPU-only sync disabled by MONADO_WINE_GPU_SYNC=0");
				}
			}
		}
	}

	c->base.base.get_swapchain_create_properties = get_swapchain_create_properties;
	c->base.base.create_swapchain = create_swapchain;
	c->base.base.begin_session = begin_session;
	c->base.base.end_session = end_session;
	c->base.base.wait_frame = wait_frame;
	c->base.base.begin_frame = begin_frame;
	c->base.base.discard_frame = discard_frame;
	c->base.base.layer_begin = layer_begin;
	c->base.base.layer_projection = layer_projection;
	c->base.base.layer_projection_depth = layer_projection_depth;
	c->base.base.layer_quad = layer_quad;
	c->base.base.layer_cube = layer_cube;
	c->base.base.layer_cylinder = layer_cylinder;
	c->base.base.layer_equirect1 = layer_equirect1;
	c->base.base.layer_equirect2 = layer_equirect2;
	c->base.base.layer_passthrough = layer_passthrough;
	c->base.base.layer_commit = layer_commit;
	c->base.base.destroy = destroy_compositor;
	c->base.base.set_thread_hint = set_thread_hint;
	c->base.base.get_display_refresh_rate = get_refresh;
	c->base.base.request_display_refresh_rate = request_refresh;
	c->base.base.set_performance_level = set_performance;
	c->base.base.get_reference_bounds_rect = get_bounds;
	c->base.base.info.max_texture_size = xcn->base.info.max_texture_size;

	for (uint32_t i = 0; i < xcn->base.info.format_count; ++i) {
		DXGI_FORMAT format = vk_to_dxgi(xcn->base.info.formats[i]);
		if (format == DXGI_FORMAT_UNKNOWN) continue;
		c->base.base.info.formats[c->base.base.info.format_count++] = (int64_t)format;
	}

	if (c->base.base.info.format_count == 0 || c->fence == NULL || c->fence_event == NULL) {
		U_LOG_E("Wine D3D11 bridge initialization failed: formats=%u fence=%p event=%p",
		        c->base.base.info.format_count, (void *)c->fence, c->fence_event);
		destroy_compositor(&c->base.base);
		return NULL;
	}

	U_LOG_I("Wine D3D11 client compositor ready: formats=%u", c->base.base.info.format_count);
	return &c->base;
}

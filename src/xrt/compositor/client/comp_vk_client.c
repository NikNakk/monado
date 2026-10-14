// Copyright 2019-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan client side glue to compositor implementation.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup comp_client
 */

#include "vk/vk_compositor_flags.h"

#include "util/u_misc.h"
#include "util/u_handles.h"
#include "util/u_trace_marker.h"
#include "util/u_debug.h"

#include "comp_vk_client.h"
#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
#include "comp_vk_metal_texture.h"
#include "os/os_time.h"
#endif

#ifdef XRT_HAVE_VK_CLIENT_SHARED_EVENT
#include "comp_vk_client_shared_event.h"
#endif


// Prefixed with OXR since the only user right now is the OpenXR state tracker.
DEBUG_GET_ONCE_LOG_OPTION(vulkan_log, "OXR_VULKAN_LOG", U_LOGGING_INFO)

/*!
 * Down-cast helper.
 *
 * @private @memberof client_vk_swapchain
 */
static inline struct client_vk_swapchain *
client_vk_swapchain(struct xrt_swapchain *xsc)
{
	return (struct client_vk_swapchain *)xsc;
}

/*!
 * Helper to get the native swapchain.
 */
static inline struct xrt_swapchain *
to_native_swapchain(struct xrt_swapchain *xsc)
{
	return &client_vk_swapchain(xsc)->xscn->base;
}

static inline struct xrt_swapchain *
to_backing_swapchain(struct xrt_swapchain *xsc)
{
#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	struct client_vk_swapchain *sc = client_vk_swapchain(xsc);
	if (sc->xscm)
		return &sc->xscm->base;
#endif
	return to_native_swapchain(xsc);
}

/*!
 * Down-cast helper.
 *
 * @private @memberof client_vk_compositor
 */
static inline struct client_vk_compositor *
client_vk_compositor(struct xrt_compositor *xc)
{
	return (struct client_vk_compositor *)xc;
}

/*!
 * Helper to get the native swapchain.
 */
static inline struct xrt_compositor *
to_native_compositor(struct xrt_compositor *xc)
{
	return &client_vk_compositor(xc)->xcn->base;
}


/*
 *
 * Transition helpers.
 *
 */

static xrt_result_t
submit_image_barrier(struct client_vk_swapchain *sc, VkCommandBuffer cmd_buffer)
{
	COMP_TRACE_MARKER();

	struct client_vk_compositor *c = sc->c;
	struct vk_bundle *vk = &c->vk;
	VkResult ret;

	// Note we do not submit a fence here, it's not needed.
	ret = vk_cmd_pool_submit_cmd_buffer(vk, &c->pool, cmd_buffer);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_cmd_pool_submit_cmd_buffer: %s %u", vk_result_string(ret), ret);
		return XRT_ERROR_FAILED_TO_SUBMIT_VULKAN_COMMANDS;
	}

	return XRT_SUCCESS;
}


/*
 *
 * Semaphore helpers.
 *
 */

#ifdef VK_KHR_timeline_semaphore
static xrt_result_t
setup_semaphore(struct client_vk_compositor *c)
{
	xrt_graphics_sync_handle_t handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	struct xrt_compositor_semaphore *xcsem = NULL;
	struct vk_bundle *vk = &c->vk;
	xrt_result_t xret;
	VkResult ret;

	xret = xrt_comp_create_semaphore(&c->xcn->base, &handle, &xcsem);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create semaphore!");
		return xret;
	}

	VkSemaphore semaphore = VK_NULL_HANDLE;
	ret = vk_create_timeline_semaphore_from_native(vk, handle, &semaphore);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateSemaphore: %s", vk_result_string(ret));
		u_graphics_sync_unref(&handle);
		xrt_compositor_semaphore_reference(&xcsem, NULL);
		return XRT_ERROR_VULKAN;
	}

	VK_NAME_SEMAPHORE(vk, semaphore, "timeline semaphore");

	c->sync.semaphore = semaphore;
	c->sync.xcsem = xcsem; // No need to reference.

	return XRT_SUCCESS;
}
#endif


/*
 *
 * Frame submit helpers.
 *
 */

static bool
submit_handle(struct client_vk_compositor *c, xrt_graphics_sync_handle_t sync_handle, xrt_result_t *out_xret)
{
	// Did we get a ready made handle, assume it's in the command stream and call commit directly.
	if (!xrt_graphics_sync_handle_is_valid(sync_handle)) {
		return false;
	}

	// Commit consumes the sync_handle.
	*out_xret = xrt_comp_layer_commit(&c->xcn->base, sync_handle);
	return true;
}

static bool
submit_semaphore(struct client_vk_compositor *c, xrt_result_t *out_xret)
{
#ifdef VK_KHR_timeline_semaphore
	if (c->sync.xcsem == NULL) {
		return false;
	}

	struct vk_bundle *vk = &c->vk;
	VkResult ret;

	VkSemaphore semaphores[1] = {
	    c->sync.semaphore,
	};
	uint64_t values[1] = {
	    c->sync.value + 1,
	};

	VkTimelineSemaphoreSubmitInfo semaphore_submit_info = {
	    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
	    .waitSemaphoreValueCount = 0,
	    .pWaitSemaphoreValues = NULL,
	    .signalSemaphoreValueCount = ARRAY_SIZE(values),
	    .pSignalSemaphoreValues = values,
	};
	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .pNext = &semaphore_submit_info,
	    .signalSemaphoreCount = ARRAY_SIZE(semaphores),
	    .pSignalSemaphores = semaphores,
	};

	vk_queue_lock(vk->main_queue);
	ret = vk->vkQueueSubmit(   //
	    vk->main_queue->queue, // queue
	    1,                     // submitCount
	    &submit_info,          // pSubmits
	    VK_NULL_HANDLE);       // fence
	vk_queue_unlock(vk->main_queue);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkQueueSubmit: %s", vk_result_string(ret));
		*out_xret = XRT_ERROR_VULKAN;
		return true;
	}

	c->sync.value = values[0];
	*out_xret = xrt_comp_layer_commit_with_semaphore( //
	    &c->xcn->base,                                // xc
	    c->sync.xcsem,                                // xcsem
	    values[0]);                                   // value

	return true;
#else
	return false;
#endif
}

static bool
submit_fence(struct client_vk_compositor *c, xrt_result_t *out_xret)
{
	xrt_graphics_sync_handle_t sync_handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	struct vk_bundle *vk = &c->vk;
	VkResult ret;

#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
	// Using sync fds currently to match OpenGL extension.
	bool sync_fence = vk->external.fence_sync_fd;
#elif defined(XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE)
	bool sync_fence = vk->external.fence_win32_handle;
#else
#error "Need port to export fence sync handles"
#endif

	if (!sync_fence) {
		return false;
	}

	{
		COMP_TRACE_IDENT(create_and_submit_fence);

		ret = vk_create_and_submit_fence_native(vk, &sync_handle);
		if (ret != VK_SUCCESS) {
			// This is really bad, log again.
			U_LOG_E("Could not create and submit a native fence!");
			*out_xret = XRT_ERROR_VULKAN;
			return true; // If we fail we should still return.
		}
	}

	*out_xret = xrt_comp_layer_commit(&c->xcn->base, sync_handle);
	return true;
}

static bool
submit_fallback(struct client_vk_compositor *c, xrt_result_t *out_xret)
{
	struct vk_bundle *vk = &c->vk;

	{
		COMP_TRACE_IDENT(device_wait_idle);

		// Last course of action fallback.
		vk_queue_lock(vk->main_queue);
		vk->vkQueueWaitIdle(vk->main_queue->queue);
		vk_queue_unlock(vk->main_queue);
	}

	*out_xret = xrt_comp_layer_commit(&c->xcn->base, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
	return true;
}


/*
 *
 * Swapchain function.
 *
 */

#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
static bool
metal_swapchain_complete(struct client_vk_swapchain *sc)
{
	struct vk_bundle *vk = &sc->c->vk;
	for (uint32_t i = 0; i < sc->base.base.image_count; ++i) {
		if (sc->acquired[i] ||
		    (sc->acquire_fence[i] && vk->vkGetFenceStatus(vk->device, sc->acquire_fence[i]) != VK_SUCCESS) ||
		    (sc->release_fence[i] && vk->vkGetFenceStatus(vk->device, sc->release_fence[i]) != VK_SUCCESS))
			return false;
	}
	return true;
}
#endif

static void
client_vk_swapchain_destroy(struct xrt_swapchain *xsc)
{
	COMP_TRACE_MARKER();

	struct client_vk_swapchain *sc = client_vk_swapchain(xsc);
	struct client_vk_compositor *c = sc->c;
	struct vk_bundle *vk = &c->vk;

#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	if (c->metal && !metal_swapchain_complete(sc)) {
		if (!sc->retired) {
			sc->retired = true;
			sc->retired_next = c->retired_swapchains;
			c->retired_swapchains = sc;
		}
		U_LOG_D("Deferring incomplete Vulkan Metal swapchain until bounded frontend teardown");
		return;
	}
#endif
	vk_cmd_pool_lock(&c->pool);
	for (uint32_t i = 0; i < sc->base.base.image_count; i++) {
		if (sc->acquire[i] != VK_NULL_HANDLE) {
			vk->vkFreeCommandBuffers(vk->device, c->pool.pool, 1, &sc->acquire[i]);
			sc->acquire[i] = VK_NULL_HANDLE;
		}
		if (sc->release[i] != VK_NULL_HANDLE) {
			vk->vkFreeCommandBuffers(vk->device, c->pool.pool, 1, &sc->release[i]);
			sc->release[i] = VK_NULL_HANDLE;
		}
	}
	vk_cmd_pool_unlock(&c->pool);

	for (uint32_t i = 0; i < sc->base.base.image_count; i++) {
		if (sc->base.images[i] != VK_NULL_HANDLE) {
			vk->vkDestroyImage(vk->device, sc->base.images[i], NULL);
			sc->base.images[i] = VK_NULL_HANDLE;
		}

		if (sc->mems[i] != VK_NULL_HANDLE) {
			vk->vkFreeMemory(vk->device, sc->mems[i], NULL);
			sc->mems[i] = VK_NULL_HANDLE;
		}
	}

#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	for (uint32_t i = 0; i < sc->base.base.image_count; ++i) {
		if (sc->acquire_fence[i])
			vk->vkDestroyFence(vk->device, sc->acquire_fence[i], NULL);
		if (sc->release_fence[i])
			vk->vkDestroyFence(vk->device, sc->release_fence[i], NULL);
	}
	if (sc->xscm) {
		struct xrt_swapchain *metal = &sc->xscm->base;
		xrt_swapchain_reference(&metal, NULL);
	}
#endif
	// Drop our reference, does NULL checking.
	xrt_swapchain_native_reference(&sc->xscn, NULL);

	free(sc);
}

#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
static void
client_vk_reap_retired(struct client_vk_compositor *c)
{
	struct client_vk_swapchain **link = &c->retired_swapchains;
	while (*link) {
		struct client_vk_swapchain *sc = *link;
		if (!metal_swapchain_complete(sc)) {
			link = &sc->retired_next;
			continue;
		}
		*link = sc->retired_next;
		sc->retired = false;
		client_vk_swapchain_destroy(&sc->base.base);
	}
}
#endif

static xrt_result_t
client_vk_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	COMP_TRACE_MARKER();

	struct client_vk_swapchain *sc = client_vk_swapchain(xsc);
	xrt_result_t xret;
	uint32_t index = 0;

	// Pipe down call into native swapchain.
	xret = xrt_swapchain_acquire_image(to_backing_swapchain(xsc), &index);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	if (sc->c->metal)
		sc->acquired[index] = true;
#endif
	// Finally done.
	*out_index = index;

	return XRT_SUCCESS;
}

static xrt_result_t
client_vk_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	COMP_TRACE_MARKER();

	// Pipe down call into native swapchain.
	return xrt_swapchain_wait_image(to_backing_swapchain(xsc), timeout_ns, index);
}

static xrt_result_t
client_vk_swapchain_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	COMP_TRACE_MARKER();

	struct client_vk_swapchain *sc = client_vk_swapchain(xsc);
	VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;

	switch (direction) {
	case XRT_BARRIER_TO_APP: cmd_buffer = sc->acquire[index]; break;
	case XRT_BARRIER_TO_COMP: cmd_buffer = sc->release[index]; break;
	default: assert(false);
	}

#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	if (sc->c->metal) {
		struct vk_bundle *vk = &sc->c->vk;
		VkFence *fence =
		    direction == XRT_BARRIER_TO_APP ? &sc->acquire_fence[index] : &sc->release_fence[index];
		if (*fence) {
			if (vk->vkGetFenceStatus(vk->device, *fence) != VK_SUCCESS)
				return XRT_ERROR_VULKAN;
			if (vk->vkResetFences(vk->device, 1, fence) != VK_SUCCESS)
				return XRT_ERROR_VULKAN;
		} else {
			VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			if (vk->vkCreateFence(vk->device, &fi, NULL, fence) != VK_SUCCESS)
				return XRT_ERROR_VULKAN;
		}
		VkSubmitInfo si = {
		    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd_buffer};
		vk_queue_lock(vk->main_queue);
		VkResult result = vk->vkQueueSubmit(vk->main_queue->queue, 1, &si, *fence);
		vk_queue_unlock(vk->main_queue);
		return result == VK_SUCCESS ? XRT_SUCCESS : XRT_ERROR_FAILED_TO_SUBMIT_VULKAN_COMMANDS;
	}
#endif
	return submit_image_barrier(sc, cmd_buffer);
}

static xrt_result_t
client_vk_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	COMP_TRACE_MARKER();

	// Pipe down call into native swapchain.
	xrt_result_t result = xrt_swapchain_release_image(to_backing_swapchain(xsc), index);
#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	if (result == XRT_SUCCESS && client_vk_swapchain(xsc)->c->metal)
		client_vk_swapchain(xsc)->acquired[index] = false;
#endif
	return result;
}

static xrt_result_t
client_vk_compositor_passthrough_create(struct xrt_compositor *xc, const struct xrt_passthrough_create_info *info)
{
	struct client_vk_compositor *c = client_vk_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_create_passthrough(&c->xcn->base, info);
}

static xrt_result_t
client_vk_compositor_passthrough_layer_create(struct xrt_compositor *xc,
                                              const struct xrt_passthrough_layer_create_info *info)
{
	struct client_vk_compositor *c = client_vk_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_create_passthrough_layer(&c->xcn->base, info);
}

static xrt_result_t
client_vk_compositor_passthrough_destroy(struct xrt_compositor *xc)
{
	struct client_vk_compositor *c = client_vk_compositor(xc);

	// Pipe down call into native compositor.
	return xrt_comp_destroy_passthrough(&c->xcn->base);
}

/*
 *
 * Compositor functions.
 *
 */

static void
client_vk_compositor_destroy(struct xrt_compositor *xc)
{
	COMP_TRACE_MARKER();

	struct client_vk_compositor *c = client_vk_compositor(xc);
	struct vk_bundle *vk = &c->vk;

#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	if (c->metal) {
		int64_t deadline = os_monotonic_get_ns() + 500000000ll;
		for (;;) {
			client_vk_reap_retired(c);
			uint64_t observed = 0;
			bool timeline_ready =
			    !c->sync.value ||
			    (vk->vkGetSemaphoreCounterValue(vk->device, c->sync.semaphore, &observed) == VK_SUCCESS &&
			     observed >= c->sync.value);
			if (!c->retired_swapchains && timeline_ready)
				break;
			if (os_monotonic_get_ns() >= deadline) {
				U_LOG_W("Retaining unresolved Vulkan Metal frontend after 500 ms teardown drain");
				return;
			}
			os_nanosleep(1000000ull);
		}
	}
#endif
	if (c->sync.semaphore != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, c->sync.semaphore, NULL);
		c->sync.semaphore = VK_NULL_HANDLE;
	}
	xrt_compositor_semaphore_reference(&c->sync.xcsem, NULL);

	// Now safe to free the pool.
	vk_cmd_pool_destroy(vk, &c->pool);

	vk_deinit_mutex(vk);
#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	client_vk_metal_texture_context_destroy(c->metal);
#endif
	free(c);
}

static xrt_result_t
client_vk_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	COMP_TRACE_MARKER();

	// Pipe down call into native compositor.
	return xrt_comp_begin_session(to_native_compositor(xc), info);
}

static xrt_result_t
client_vk_compositor_end_session(struct xrt_compositor *xc)
{
	COMP_TRACE_MARKER();

	// Pipe down call into native compositor.
	return xrt_comp_end_session(to_native_compositor(xc));
}

static xrt_result_t
client_vk_compositor_wait_frame(struct xrt_compositor *xc,
                                int64_t *out_frame_id,
                                int64_t *predicted_display_time,
                                int64_t *predicted_display_period)
{
	COMP_TRACE_MARKER();

	// Pipe down call into native compositor.
	return xrt_comp_wait_frame(    //
	    to_native_compositor(xc),  //
	    out_frame_id,              //
	    predicted_display_time,    //
	    predicted_display_period); //
}

static xrt_result_t
client_vk_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	// Pipe down call into native compositor.
	return xrt_comp_begin_frame(to_native_compositor(xc), frame_id);
}

static xrt_result_t
client_vk_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	// Pipe down call into native compositor.
	return xrt_comp_discard_frame(to_native_compositor(xc), frame_id);
}

static xrt_result_t
client_vk_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	COMP_TRACE_MARKER();

	return xrt_comp_layer_begin(to_native_compositor(xc), data);
}

static xrt_result_t
client_vk_compositor_layer_projection(struct xrt_compositor *xc,
                                      struct xrt_device *xdev,
                                      struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                      const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscn[XRT_MAX_VIEWS];

	assert(data->type == XRT_LAYER_PROJECTION);
	for (uint32_t i = 0; i < data->view_count; ++i) {
		xscn[i] = &client_vk_swapchain(xsc[i])->xscn->base;
	}

	xcn = to_native_compositor(xc);

	return xrt_comp_layer_projection(xcn, xdev, xscn, data);
}


static xrt_result_t
client_vk_compositor_layer_stereo_projection_depth(struct xrt_compositor *xc,
                                                   struct xrt_device *xdev,
                                                   struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                                   struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                                   const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;

	struct xrt_swapchain *xscn[XRT_MAX_VIEWS];
	struct xrt_swapchain *d_xscn[XRT_MAX_VIEWS];

	assert(data->type == XRT_LAYER_PROJECTION_DEPTH);

	xcn = to_native_compositor(xc);
	for (uint32_t i = 0; i < data->view_count; ++i) {
		xscn[i] = to_native_swapchain(xsc[i]);
		d_xscn[i] = to_native_swapchain(d_xsc[i]);
	}

	return xrt_comp_layer_projection_depth(xcn, xdev, xscn, d_xscn, data);
}

static xrt_result_t
client_vk_compositor_layer_quad(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc,
                                const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscfb;

	assert(data->type == XRT_LAYER_QUAD);

	xcn = to_native_compositor(xc);
	xscfb = to_native_swapchain(xsc);

	return xrt_comp_layer_quad(xcn, xdev, xscfb, data);
}

static xrt_result_t
client_vk_compositor_layer_cube(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc,
                                const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscfb;

	assert(data->type == XRT_LAYER_CUBE);

	xcn = to_native_compositor(xc);
	xscfb = to_native_swapchain(xsc);

	return xrt_comp_layer_cube(xcn, xdev, xscfb, data);
}

static xrt_result_t
client_vk_compositor_layer_cylinder(struct xrt_compositor *xc,
                                    struct xrt_device *xdev,
                                    struct xrt_swapchain *xsc,
                                    const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscfb;

	assert(data->type == XRT_LAYER_CYLINDER);

	xcn = to_native_compositor(xc);
	xscfb = to_native_swapchain(xsc);

	return xrt_comp_layer_cylinder(xcn, xdev, xscfb, data);
}

static xrt_result_t
client_vk_compositor_layer_equirect1(struct xrt_compositor *xc,
                                     struct xrt_device *xdev,
                                     struct xrt_swapchain *xsc,
                                     const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscfb;

	assert(data->type == XRT_LAYER_EQUIRECT1);

	xcn = to_native_compositor(xc);
	xscfb = to_native_swapchain(xsc);

	return xrt_comp_layer_equirect1(xcn, xdev, xscfb, data);
}

static xrt_result_t
client_vk_compositor_layer_equirect2(struct xrt_compositor *xc,
                                     struct xrt_device *xdev,
                                     struct xrt_swapchain *xsc,
                                     const struct xrt_layer_data *data)
{
	struct xrt_compositor *xcn;
	struct xrt_swapchain *xscfb;

	assert(data->type == XRT_LAYER_EQUIRECT2);

	xcn = to_native_compositor(xc);
	xscfb = to_native_swapchain(xsc);

	return xrt_comp_layer_equirect2(xcn, xdev, xscfb, data);
}

static xrt_result_t
client_vk_compositor_layer_passthrough(struct xrt_compositor *xc,
                                       struct xrt_device *xdev,
                                       const struct xrt_layer_data *data)
{
	struct client_vk_compositor *c = client_vk_compositor(xc);

	assert(data->type == XRT_LAYER_PASSTHROUGH);

	return xrt_comp_layer_passthrough(&c->xcn->base, xdev, data);
}

static xrt_result_t
client_vk_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	COMP_TRACE_MARKER();

	struct client_vk_compositor *c = client_vk_compositor(xc);

	if (c->renderdoc_enabled) {
		struct vk_bundle *vk = &c->vk;
		VkResult ret = vk_cmd_pool_submit_cmd_buffer(vk, &c->pool, c->dcb);
		if (ret != VK_SUCCESS) {
			VK_ERROR(vk, "vk_cmd_pool_submit_cmd_buffer: %s %u", vk_result_string(ret), ret);
			return XRT_ERROR_FAILED_TO_SUBMIT_VULKAN_COMMANDS;
		}
	}

	xrt_result_t xret = XRT_SUCCESS;
	if (submit_handle(c, sync_handle, &xret)) {
		return xret;
	} else if (submit_semaphore(c, &xret)) {
		return xret;
	} else if (submit_fence(c, &xret)) {
		return xret;
	} else if (submit_fallback(c, &xret)) {
		return xret;
	} else {
		// Really bad state.
		return XRT_ERROR_VULKAN;
	}
}

static xrt_result_t
client_vk_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                                     const struct xrt_swapchain_create_info *info,
                                                     struct xrt_swapchain_create_properties *xsccp)
{
	return xrt_comp_get_swapchain_create_properties(to_native_compositor(xc), info, xsccp);
}

static xrt_result_t
client_vk_swapchain_create(struct xrt_compositor *xc,
                           const struct xrt_swapchain_create_info *info,
                           struct xrt_swapchain **out_xsc)
{
	COMP_TRACE_MARKER();

	struct client_vk_compositor *c = client_vk_compositor(xc);
	struct vk_bundle *vk = &c->vk;
	VkResult ret;
	xrt_result_t xret;

#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	if (c->metal)
		client_vk_reap_retired(c);
#endif

	struct xrt_swapchain_create_properties xsccp = XRT_STRUCT_INIT;
	xret = xrt_comp_get_swapchain_create_properties(&c->xcn->base, info, &xsccp);
	if (xret != XRT_SUCCESS) {
		VK_ERROR(vk, "Failed to get create properties: %u", xret);
		return xret;
	}

	// Update the create info.
	struct xrt_swapchain_create_info xinfo = *info;
	xinfo.bits |= xsccp.extra_bits;

	struct xrt_swapchain_native *xscn = NULL;
#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	struct xrt_swapchain_metal *xscm = NULL;
	if (client_vk_metal_texture_required(&xinfo)) {
		if (!client_vk_metal_texture_enabled())
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		xret = client_vk_metal_texture_swapchain_create(c->metal, &xinfo, &xscm, &xscn);
	} else
#endif
	{
		xret = xrt_comp_native_create_swapchain(c->xcn, &xinfo, &xscn);
	}
	if (xret != XRT_SUCCESS)
		return xret;
	struct xrt_swapchain *xsc = &xscn->base;

	VkAccessFlags barrier_access_mask = vk_csci_get_barrier_access_mask(xinfo.bits);

	VkImageLayout barrier_optimal_layout = vk_csci_get_barrier_optimal_layout(xinfo.format);
	assert(barrier_optimal_layout != VK_IMAGE_LAYOUT_UNDEFINED);

	VkImageAspectFlags barrier_aspect_mask = vk_csci_get_barrier_aspect_mask(xinfo.format);
	assert(barrier_aspect_mask != 0);

	struct client_vk_swapchain *sc = U_TYPED_CALLOC(struct client_vk_swapchain);
	if (!sc) {
#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
		if (xscm) {
			struct xrt_swapchain *metal = &xscm->base;
			xrt_swapchain_reference(&metal, NULL);
		}
#endif
		xrt_swapchain_native_reference(&xscn, NULL);
		return XRT_ERROR_ALLOCATION;
	}
	sc->base.base.destroy = client_vk_swapchain_destroy;
	sc->base.base.acquire_image = client_vk_swapchain_acquire_image;
	sc->base.base.wait_image = client_vk_swapchain_wait_image;
	sc->base.base.barrier_image = client_vk_swapchain_barrier_image;
	sc->base.base.release_image = client_vk_swapchain_release_image;
	sc->base.base.reference.count = 1;
	sc->base.base.image_count = xsc->image_count; // Fetch the number of images from the native swapchain.
	sc->c = c;
	sc->xscn = xscn;
#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	sc->xscm = xscm;
#endif

	for (uint32_t i = 0; i < xsc->image_count; i++) {
#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
		if (sc->xscm)
			ret = client_vk_metal_texture_import(vk, &xinfo, sc->xscm->images[i], &sc->base.images[i]);
		else
#endif
		{
			ret = vk_create_image_from_native(vk, &xinfo, &xscn->images[i], &sc->base.images[i],
			                                  &sc->mems[i]);
		}

		if (ret != VK_SUCCESS) {
			client_vk_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_VULKAN;
		}

		VK_NAME_IMAGE(vk, sc->base.images[i], "vk_image_collection image");
		if (sc->mems[i])
			VK_NAME_DEVICE_MEMORY(vk, sc->mems[i], "vk_image_collection device_memory");
	}

	vk_cmd_pool_lock(&c->pool);
	const VkCommandBufferUsageFlags flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;

	// Prerecord command buffers for swapchain image ownership/layout transitions
	for (uint32_t i = 0; i < xsc->image_count; i++) {
		ret = vk_cmd_pool_create_and_begin_cmd_buffer_locked(vk, &c->pool, flags, &sc->acquire[i]);
		if (ret != VK_SUCCESS) {
			vk_cmd_pool_unlock(&c->pool);
			client_vk_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_VULKAN;
		}
		VK_NAME_COMMAND_BUFFER(vk, sc->acquire[i], "client_vk_swapchain acquire command buffer");
		ret = vk_cmd_pool_create_and_begin_cmd_buffer_locked(vk, &c->pool, flags, &sc->release[i]);
		if (ret != VK_SUCCESS) {
			vk_cmd_pool_unlock(&c->pool);
			client_vk_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_VULKAN;
		}
		VK_NAME_COMMAND_BUFFER(vk, sc->release[i], "client_vk_swapchain release command buffer");

		VkImageSubresourceRange subresource_range = {
		    .aspectMask = barrier_aspect_mask,
		    .baseMipLevel = 0,
		    .levelCount = VK_REMAINING_MIP_LEVELS,
		    .baseArrayLayer = 0,
		    .layerCount = VK_REMAINING_ARRAY_LAYERS,
		};

		VkImageMemoryBarrier acquire = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = barrier_access_mask,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = barrier_optimal_layout,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .image = sc->base.images[i],
		    .subresourceRange = subresource_range,
		};

		VkImageMemoryBarrier release = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = barrier_access_mask,
		    .dstAccessMask = 0,
		    .oldLayout = barrier_optimal_layout,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .srcQueueFamilyIndex = vk->main_queue->family_index,
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_IOSURFACE)
		    // IOSurface-backed images stay within the same logical queue ownership model on Apple.
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
#else
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
#endif
		    .image = sc->base.images[i],
		    .subresourceRange = subresource_range,
		};

#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
		if (c->metal)
			release.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
#endif
		//! @todo less conservative pipeline stage masks based on usage
		vk->vkCmdPipelineBarrier(               //
		    sc->acquire[i],                     // commandBuffer
		    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,  // srcStageMask
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, // dstStageMask
		    0,                                  // dependencyFlags
		    0,                                  // memoryBarrierCount
		    NULL,                               // pMemoryBarriers
		    0,                                  // bufferMemoryBarrierCount
		    NULL,                               // pBufferMemoryBarriers
		    1,                                  // imageMemoryBarrierCount
		    &acquire);                          // pImageMemoryBarriers

		vk->vkCmdPipelineBarrier(                 //
		    sc->release[i],                       // commandBuffer
		    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,   // srcStageMask
		    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, // dstStageMask
		    0,                                    // dependencyFlags
		    0,                                    // memoryBarrierCount
		    NULL,                                 // pMemoryBarriers
		    0,                                    // bufferMemoryBarrierCount
		    NULL,                                 // pBufferMemoryBarriers
		    1,                                    // imageMemoryBarrierCount
		    &release);                            // pImageMemoryBarriers

		ret = vk->vkEndCommandBuffer(sc->acquire[i]);
		if (ret != VK_SUCCESS) {
			VK_ERROR(vk, "vkEndCommandBuffer: %s", vk_result_string(ret));
			vk_cmd_pool_unlock(&c->pool);
			client_vk_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_VULKAN;
		}
		ret = vk->vkEndCommandBuffer(sc->release[i]);
		if (ret != VK_SUCCESS) {
			VK_ERROR(vk, "vkEndCommandBuffer: %s", vk_result_string(ret));
			vk_cmd_pool_unlock(&c->pool);
			client_vk_swapchain_destroy(&sc->base.base);
			return XRT_ERROR_VULKAN;
		}
	}
	vk_cmd_pool_unlock(&c->pool);


	*out_xsc = &sc->base.base;

	return XRT_SUCCESS;
}

struct client_vk_compositor *
client_vk_compositor_create(struct xrt_compositor_native *xcn,
                            VkInstance instance,
                            PFN_vkGetInstanceProcAddr getProc,
                            VkPhysicalDevice physicalDevice,
                            VkDevice device,
                            bool external_fence_fd_enabled,
                            bool external_semaphore_fd_enabled,
                            bool timeline_semaphore_enabled,
                            bool image_format_list_enabled,
                            bool metal_objects_enabled,
                            bool debug_utils_enabled,
                            bool renderdoc_enabled,
                            uint32_t queueFamilyIndex,
                            uint32_t queueIndex)
{
	COMP_TRACE_MARKER();

	xrt_result_t xret;
	VkResult ret;
	struct client_vk_compositor *c = U_TYPED_CALLOC(struct client_vk_compositor);
	if (!c)
		return NULL;

	c->base.get_swapchain_create_properties = client_vk_compositor_get_swapchain_create_properties;
	c->base.create_swapchain = client_vk_swapchain_create;
	c->base.create_passthrough = client_vk_compositor_passthrough_create;
	c->base.create_passthrough_layer = client_vk_compositor_passthrough_layer_create;
	c->base.destroy_passthrough = client_vk_compositor_passthrough_destroy;
	c->base.begin_session = client_vk_compositor_begin_session;
	c->base.end_session = client_vk_compositor_end_session;
	c->base.wait_frame = client_vk_compositor_wait_frame;
	c->base.begin_frame = client_vk_compositor_begin_frame;
	c->base.discard_frame = client_vk_compositor_discard_frame;
	c->base.layer_begin = client_vk_compositor_layer_begin;
	c->base.layer_projection = client_vk_compositor_layer_projection;
	c->base.layer_projection_depth = client_vk_compositor_layer_stereo_projection_depth;
	c->base.layer_quad = client_vk_compositor_layer_quad;
	c->base.layer_cube = client_vk_compositor_layer_cube;
	c->base.layer_cylinder = client_vk_compositor_layer_cylinder;
	c->base.layer_equirect1 = client_vk_compositor_layer_equirect1;
	c->base.layer_equirect2 = client_vk_compositor_layer_equirect2;
	c->base.layer_passthrough = client_vk_compositor_layer_passthrough;
	c->base.layer_commit = client_vk_compositor_layer_commit;
	c->base.destroy = client_vk_compositor_destroy;

	c->xcn = xcn;
	// passthrough our formats from the native compositor to the client
	for (uint32_t i = 0; i < xcn->base.info.format_count; i++) {
		c->base.info.formats[i] = xcn->base.info.formats[i];
	}

	c->base.info.format_count = xcn->base.info.format_count;
	c->renderdoc_enabled = renderdoc_enabled;

	// Default to info.
	enum u_logging_level log_level = debug_get_log_option_vulkan_log();

	ret = vk_init_from_given(          //
	    &c->vk,                        // vk_bundle
	    getProc,                       // vkGetInstanceProcAddr
	    instance,                      // instance
	    physicalDevice,                // physical_device
	    device,                        // device
	    queueFamilyIndex,              // queue_family_index
	    queueIndex,                    // queue_index
	    external_fence_fd_enabled,     // external_fence_fd_enabled
	    external_semaphore_fd_enabled, // external_semaphore_fd_enabled
	    timeline_semaphore_enabled,    // timeline_semaphore_enabled
	    image_format_list_enabled,     // image_format_list_enabled
	    metal_objects_enabled,         // metal_objects_enabled
	    debug_utils_enabled,           // debug_utils_enabled
	    log_level);                    // log_level
	if (ret != VK_SUCCESS) {
		goto err_free;
	}

	ret = vk_init_mutex(&c->vk);
	if (ret != VK_SUCCESS) {
		goto err_free;
	}

	ret = vk_cmd_pool_init(&c->vk, &c->pool, 0);
	if (ret != VK_SUCCESS) {
		goto err_mutex;
	}

	VK_NAME_COMMAND_POOL(&c->vk, c->pool.pool, "client_vk_compositor command pool");

#ifdef VK_KHR_timeline_semaphore
	if (vk_can_import_and_export_timeline_semaphore(&c->vk)) {
		xret = setup_semaphore(c);
		if (xret != XRT_SUCCESS) {
			goto err_pool;
		}
	}
#endif

#ifdef XRT_HAVE_VK_CLIENT_SHARED_EVENT
	// macOS has no exportable timeline or fence, so commit would fall back to
	// vkQueueWaitIdle; opt in to a Metal shared-event timeline instead.
	if (c->sync.xcsem == NULL && client_vk_shared_event_enabled()) {
		(void)client_vk_shared_event_create(c->xcn, &c->vk, &c->sync.semaphore, &c->sync.xcsem, &c->sync.value);
	}
#endif

#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	if (client_vk_metal_texture_enabled() || c->sync.xcsem) {
		if (c->vk.has_EXT_metal_objects && c->vk.vkExportMetalObjectsEXT) {
			c->metal = client_vk_metal_texture_context_create(c->xcn, &c->vk);
			if (!c->metal)
				goto err_pool;
		} else {
			U_LOG_W("Metal texture import unavailable; retaining native single-layer Vulkan swapchains");
		}
	}
#endif

	// Get max texture size.
	{
		struct vk_bundle *vk = &c->vk;
		VkPhysicalDeviceProperties pdp;
		vk->vkGetPhysicalDeviceProperties(vk->physical_device, &pdp);
		c->base.info.max_texture_size = pdp.limits.maxImageDimension2D;
	}

	if (!c->renderdoc_enabled) {
		return c;
	}

	struct vk_bundle *vk = &c->vk;
	if (!vk->has_EXT_debug_utils) {
		c->renderdoc_enabled = false;
		return c;
	}

	// Create a no-op VkCommandBuffer and submit it to the VkQueue, just for inserting a debug label into
	// RenderDoc for triggering the capture.
	ret = vk_cmd_pool_create_begin_insert_label_and_end_cmd_buffer_locked(
	    vk, &c->pool, "vr-marker,frame_end,type,application", &c->dcb);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_cmd_pool_create_insert_debug_label_and_end_cmd_buffer: %s", vk_result_string(ret));
		goto err_pool;
	}

	return c;

err_pool:
	if (c->sync.semaphore)
		c->vk.vkDestroySemaphore(c->vk.device, c->sync.semaphore, NULL);
	xrt_compositor_semaphore_reference(&c->sync.xcsem, NULL);
#ifdef XRT_HAVE_VK_CLIENT_METAL_TEXTURES
	client_vk_metal_texture_context_destroy(c->metal);
#endif
	vk_cmd_pool_destroy(&c->vk, &c->pool);
err_mutex:
	vk_deinit_mutex(&c->vk);
err_free:
	free(c);

	return NULL;
}

xrt_result_t
client_vk_compositor_blit_to_swapchain(struct client_vk_compositor *c,
                                       struct xrt_swapchain *xsc,
                                       const struct vk_cmd_first_mip_image *src_image,
                                       const struct vk_cmd_blit_image_params *src_params,
                                       uint32_t dst_index,
                                       const struct vk_cmd_blit_image_params *dst_params)
{
	COMP_TRACE_MARKER();

	struct client_vk_swapchain *sc = client_vk_swapchain(xsc);
	struct vk_bundle *vk = &c->vk;
	VkCommandBuffer cmd_buffer = VK_NULL_HANDLE;
	VkResult ret;

	vk_cmd_pool_lock(&c->pool);

	ret = vk_cmd_pool_create_and_begin_cmd_buffer_locked(vk, &c->pool, 0, &cmd_buffer);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_cmd_pool_create_and_begin_cmd_buffer_locked: %s", vk_result_string(ret));
		vk_cmd_pool_unlock(&c->pool);
		return XRT_ERROR_VULKAN;
	}

	struct vk_cmd_image_transfer_info blit_info = {
	    .src =
	        {
	            .params = *src_params,
	            .fm_image = *src_image,
	        },
	    .dst =
	        {
	            .params = *dst_params,
	            .fm_image =
	                {
	                    .base_array_layer = 0,
	                    .aspect_mask = VK_IMAGE_ASPECT_COLOR_BIT,
	                    .image = sc->base.images[dst_index],
	                },
	        },
	};

	vk_cmd_blit_image_locked(vk, cmd_buffer, &blit_info);

	ret = vk_cmd_pool_end_submit_wait_and_free_cmd_buffer_locked(vk, &c->pool, cmd_buffer);
	vk_cmd_pool_unlock(&c->pool);

	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vk_cmd_pool_end_submit_wait_and_free_cmd_buffer_locked: %s", vk_result_string(ret));
		return XRT_ERROR_VULKAN;
	}

	return XRT_SUCCESS;
}

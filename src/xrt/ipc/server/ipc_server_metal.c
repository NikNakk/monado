// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS Metal resource import for IPC server.
 * @ingroup ipc_server
 */

#include "server/ipc_server.h"
#include "shared/ipc_metal_xpc.h"
#include "util/u_trace_marker.h"

#ifdef XRT_OS_OSX
#include "util/comp_metal_semaphore_provider.h"
#include "util/comp_metal_swapchain_handoff.h"
#include "util/comp_swapchain_gpu_reuse.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#endif

#ifdef XRT_OS_OSX
/*
 * OpenXR permits acquire_image() to return an image that is not writable yet;
 * wait_image() is the operation that establishes availability. The default
 * compositor swapchain therefore returns the oldest FIFO image without looking
 * at its use state.
 *
 * For cross-process Metal swapchains that is unnecessarily expensive: GPU
 * reuse tracking can know that the oldest image still has a pending compositor
 * consumer or an unsignalled GPU-use timeline value while a newer FIFO image is
 * already reusable. A client such as GAV then blocks in xrWaitSwapchainImage()
 * despite another image being immediately available.
 *
 * Keep the authoritative wait_image() synchronization unchanged. This wrapper
 * only changes which already-released FIFO image acquire_image() prefers. Each
 * candidate is probed with a zero-timeout wait; busy candidates are rotated to
 * the back of the FIFO. If no candidate is immediately reusable, the oldest
 * candidate is returned exactly as before and the real wait happens in the
 * subsequent xrWaitSwapchainImage().
 */
struct metal_ipc_smart_acquire_tracker
{
	struct xrt_swapchain *xsc;
	xrt_result_t (*original_acquire_image)(struct xrt_swapchain *, uint32_t *);
	void (*original_destroy)(struct xrt_swapchain *);
	struct metal_ipc_smart_acquire_tracker *next;
};

static pthread_mutex_t g_metal_ipc_smart_acquire_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct metal_ipc_smart_acquire_tracker *g_metal_ipc_smart_acquire_trackers = NULL;

static struct metal_ipc_smart_acquire_tracker *
metal_ipc_smart_acquire_find_locked(struct xrt_swapchain *xsc)
{
	for (struct metal_ipc_smart_acquire_tracker *tracker = g_metal_ipc_smart_acquire_trackers;
	     tracker != NULL;
	     tracker = tracker->next) {
		if (tracker->xsc == xsc) {
			return tracker;
		}
	}

	return NULL;
}

static xrt_result_t
metal_ipc_smart_acquire_requeue(struct xrt_swapchain *xsc, const uint32_t *indices, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		xrt_result_t xret = xrt_swapchain_release_image(xsc, indices[i]);
		if (xret != XRT_SUCCESS) {
			return xret;
		}
	}

	return XRT_SUCCESS;
}

static xrt_result_t
metal_ipc_smart_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	if (xsc == NULL || out_index == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	pthread_mutex_lock(&g_metal_ipc_smart_acquire_mutex);
	struct metal_ipc_smart_acquire_tracker *tracker = metal_ipc_smart_acquire_find_locked(xsc);
	if (tracker == NULL || tracker->original_acquire_image == NULL) {
		pthread_mutex_unlock(&g_metal_ipc_smart_acquire_mutex);
		return XRT_ERROR_VULKAN;
	}

	uint32_t skipped[XRT_MAX_SWAPCHAIN_IMAGES] = {0};
	uint32_t skipped_count = 0;
	uint32_t selected_index = 0;
	bool selected_ready = false;
	xrt_result_t acquire_result = XRT_ERROR_NO_IMAGE_AVAILABLE;

	/*
	 * Only images currently present in the native FIFO can be returned by
	 * acquire_image(). At most image_count probes are therefore needed.
	 */
	for (uint32_t i = 0; i < xsc->image_count; i++) {
		uint32_t candidate = 0;
		acquire_result = tracker->original_acquire_image(xsc, &candidate);
		if (acquire_result != XRT_SUCCESS) {
			break;
		}

		/*
		 * GPU-reuse tracking wraps wait_image(), so a zero-timeout probe tests
		 * all three relevant domains without weakening any of them:
		 * normal Monado image use, pending compositor consumers, and the
		 * compositor GPU-use timeline.
		 */
		xrt_result_t probe_result = xrt_swapchain_wait_image(xsc, 0, candidate);
		if (probe_result == XRT_SUCCESS) {
			selected_index = candidate;
			selected_ready = true;
			break;
		}

		/*
		 * XRT_TIMEOUT is the expected busy result. Treat any other probe
		 * failure conservatively as busy too: acquire itself should not gain a
		 * new failure mode from this optimization, and the real wait will
		 * report the error if this image ultimately has to be used.
		 */
		skipped[skipped_count++] = candidate;
	}

	if (selected_ready) {
		/*
		 * Leave the ready candidate acquired. Requeue skipped busy images in
		 * their original order behind any candidates we did not need to scan.
		 */
		xrt_result_t xret = metal_ipc_smart_acquire_requeue(xsc, skipped, skipped_count);
		if (xret != XRT_SUCCESS) {
			/* Best effort: do not silently leave the selected candidate acquired. */
			xrt_swapchain_release_image(xsc, selected_index);
			pthread_mutex_unlock(&g_metal_ipc_smart_acquire_mutex);
			return xret;
		}

		*out_index = selected_index;
		pthread_mutex_unlock(&g_metal_ipc_smart_acquire_mutex);
		return XRT_SUCCESS;
	}

	if (skipped_count > 0) {
		/*
		 * Nothing was immediately reusable. Preserve the old behaviour
		 * exactly: keep the oldest candidate acquired and put every later
		 * candidate back in FIFO order. xrWaitSwapchainImage() will block on
		 * the oldest image just as it did before smart acquisition.
		 */
		selected_index = skipped[0];
		xrt_result_t xret = metal_ipc_smart_acquire_requeue(xsc, &skipped[1], skipped_count - 1);
		if (xret != XRT_SUCCESS) {
			xrt_swapchain_release_image(xsc, selected_index);
			pthread_mutex_unlock(&g_metal_ipc_smart_acquire_mutex);
			return xret;
		}

		*out_index = selected_index;
		pthread_mutex_unlock(&g_metal_ipc_smart_acquire_mutex);
		return XRT_SUCCESS;
	}

	pthread_mutex_unlock(&g_metal_ipc_smart_acquire_mutex);
	return acquire_result;
}

static void
metal_ipc_smart_acquire_destroy(struct xrt_swapchain *xsc)
{
	void (*original_destroy)(struct xrt_swapchain *) = NULL;
	struct metal_ipc_smart_acquire_tracker *removed = NULL;

	pthread_mutex_lock(&g_metal_ipc_smart_acquire_mutex);
	struct metal_ipc_smart_acquire_tracker **tracker_ptr = &g_metal_ipc_smart_acquire_trackers;
	while (*tracker_ptr != NULL) {
		if ((*tracker_ptr)->xsc == xsc) {
			removed = *tracker_ptr;
			*tracker_ptr = removed->next;
			break;
		}
		tracker_ptr = &(*tracker_ptr)->next;
	}

	if (removed != NULL) {
		xsc->acquire_image = removed->original_acquire_image;
		xsc->destroy = removed->original_destroy;
		original_destroy = removed->original_destroy;
	}
	pthread_mutex_unlock(&g_metal_ipc_smart_acquire_mutex);

	free(removed);

	if (original_destroy != NULL) {
		original_destroy(xsc);
	}
}

static xrt_result_t
metal_ipc_smart_acquire_enable(struct xrt_swapchain *xsc)
{
	if (xsc == NULL || xsc->acquire_image == NULL || xsc->wait_image == NULL || xsc->release_image == NULL ||
	    xsc->destroy == NULL || xsc->image_count == 0 || xsc->image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	struct metal_ipc_smart_acquire_tracker *tracker = calloc(1, sizeof(*tracker));
	if (tracker == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	pthread_mutex_lock(&g_metal_ipc_smart_acquire_mutex);
	if (metal_ipc_smart_acquire_find_locked(xsc) != NULL) {
		pthread_mutex_unlock(&g_metal_ipc_smart_acquire_mutex);
		free(tracker);
		return XRT_SUCCESS;
	}

	tracker->xsc = xsc;
	tracker->original_acquire_image = xsc->acquire_image;
	tracker->original_destroy = xsc->destroy;
	tracker->next = g_metal_ipc_smart_acquire_trackers;
	g_metal_ipc_smart_acquire_trackers = tracker;

	xsc->acquire_image = metal_ipc_smart_acquire_image;
	xsc->destroy = metal_ipc_smart_acquire_destroy;
	pthread_mutex_unlock(&g_metal_ipc_smart_acquire_mutex);

	return XRT_SUCCESS;
}
#endif

static xrt_result_t
find_free_swapchain_index(volatile struct ipc_client_state *ics, uint32_t *out_index)
{
	for (uint32_t index = 0; index < IPC_MAX_CLIENT_SWAPCHAINS; index++) {
		if (!ics->swapchain_data[index].active) {
			*out_index = index;
			return XRT_SUCCESS;
		}
	}

	IPC_ERROR(ics->server, "Too many swapchains!");
	return XRT_ERROR_IPC_FAILURE;
}

static xrt_result_t
find_free_semaphore_index(volatile struct ipc_client_state *ics, uint32_t *out_index)
{
	for (uint32_t index = 0; index < IPC_MAX_CLIENT_SEMAPHORES; index++) {
		if (ics->xcsems[index] == NULL) {
			*out_index = index;
			return XRT_SUCCESS;
		}
	}

	IPC_ERROR(ics->server, "Too many compositor semaphores alive!");
	return XRT_ERROR_IPC_FAILURE;
}

xrt_result_t
ipc_handle_swapchain_import_metal(volatile struct ipc_client_state *ics,
                                  const struct xrt_swapchain_create_info *info,
                                  uint64_t token,
                                  uint32_t image_count,
                                  uint32_t *out_id)
{
	IPC_TRACE_MARKER();

#ifndef XRT_OS_OSX
	(void)ics;
	(void)info;
	(void)token;
	(void)image_count;
	(void)out_id;
	return XRT_ERROR_NOT_IMPLEMENTED;
#else
	if (ics == NULL || info == NULL || out_id == NULL || ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}
	if (image_count == 0 || image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	uint32_t index = 0;
	xrt_result_t xret = find_free_swapchain_index(ics, &index);
	if (xret != XRT_SUCCESS) {
		ipc_metal_xpc_discard_token(token);
		return xret;
	}

	void *textures[XRT_MAX_SWAPCHAIN_IMAGES] = {0};
	xret = ipc_metal_xpc_take_textures(token, image_count, textures);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server,
		          "Failed to retrieve Metal XPC swapchain textures token=0x%016llx count=%u",
		          (unsigned long long)token,
		          image_count);
		return xret;
	}

	if (!comp_metal_swapchain_import_begin(info, image_count, textures)) {
		ipc_metal_xpc_release_textures(textures, image_count);
		return XRT_ERROR_ALLOCATION;
	}

	struct xrt_swapchain *xsc = NULL;
	xret = xrt_comp_create_swapchain(ics->xc, info, &xsc);
	bool consumed = comp_metal_swapchain_import_was_consumed();
	comp_metal_swapchain_import_end();

	/*
	 * VkImportMetalTextureInfoEXT has retained/consumed the Metal backing it
	 * needs by the time the synchronous create call returns. The broker-side
	 * reconstruction references are no longer needed here.
	 */
	ipc_metal_xpc_release_textures(textures, image_count);

	if (xret != XRT_SUCCESS) {
		return xret;
	}
	if (!consumed || xsc == NULL || xsc->image_count != image_count) {
		IPC_ERROR(ics->server,
		          "Metal IPC swapchain allocator mismatch: consumed=%s expected=%u actual=%u",
		          consumed ? "true" : "false",
		          image_count,
		          xsc != NULL ? xsc->image_count : 0);
		xrt_swapchain_reference(&xsc, NULL);
		return XRT_ERROR_VULKAN;
	}

	/*
	 * Cross-process Metal textures need an explicit service-GPU completion
	 * barrier before xrWaitSwapchainImage can return them to the producer.
	 */
	xret = comp_swapchain_gpu_reuse_enable(xsc);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ics->server,
		          "Failed to enable Metal IPC swapchain GPU reuse tracking: result=%d",
		          xret);
		xrt_swapchain_reference(&xsc, NULL);
		return xret;
	}

	/*
	 * Prefer an already-reusable released image rather than blindly handing
	 * the client the oldest FIFO image and making its subsequent wait block.
	 * This does not replace or relax GPU reuse tracking: wait_image() remains
	 * the final authority before the application can write the image.
	 */
	xret = metal_ipc_smart_acquire_enable(xsc);
	if (xret != XRT_SUCCESS) {
		IPC_WARN(ics->server,
		         "Could not enable Metal IPC smart swapchain acquire; keeping safe FIFO behaviour: result=%d",
		         xret);
	} else {
		IPC_INFO(ics->server,
		         "Metal IPC smart swapchain acquire enabled: images=%u swapchain=%p",
		         xsc->image_count,
		         (void *)xsc);
	}

	ics->swapchain_count++;
	ics->xscs[index] = xsc;
	ics->swapchain_data[index].active = true;
	ics->swapchain_data[index].width = info->width;
	ics->swapchain_data[index].height = info->height;
	ics->swapchain_data[index].format = info->format;
	ics->swapchain_data[index].image_count = xsc->image_count;
	*out_id = index;

	IPC_INFO(ics->server,
	         "Metal IPC swapchain active: id=%u images=%u size=%ux%u array_size=%u token=0x%016llx",
	         index,
	         image_count,
	         info->width,
	         info->height,
	         info->array_size,
	         (unsigned long long)token);

	return XRT_SUCCESS;
#endif
}

xrt_result_t
ipc_handle_swapchain_import_iosurface(volatile struct ipc_client_state *ics,
                                      const struct xrt_swapchain_create_info *info,
                                      const struct ipc_arg_swapchain_iosurface *args,
                                      uint32_t *out_id)
{
	IPC_TRACE_MARKER();

#ifndef XRT_OS_OSX
	(void)ics;
	(void)info;
	(void)args;
	(void)out_id;
	return XRT_ERROR_NOT_IMPLEMENTED;
#else
	if (ics == NULL || info == NULL || args == NULL || out_id == NULL || ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}
	const uint32_t image_count = args->image_count;
	if (image_count == 0 || image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	for (uint32_t i = 0; i < image_count; i++) {
		if (args->ids[i] == 0) {
			return XRT_ERROR_INVALID_ARGUMENT;
		}
	}

	uint32_t index = 0;
	xrt_result_t xret = find_free_swapchain_index(ics, &index);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	if (!comp_metal_swapchain_import_begin_iosurface_ids(info, image_count, args->ids)) {
		return XRT_ERROR_ALLOCATION;
	}

	struct xrt_swapchain *xsc = NULL;
	xret = xrt_comp_create_swapchain(ics->xc, info, &xsc);
	bool consumed = comp_metal_swapchain_import_was_consumed();
	comp_metal_swapchain_import_end();

	if (xret != XRT_SUCCESS) {
		return xret;
	}
	if (!consumed || xsc == NULL || xsc->image_count != image_count) {
		IPC_ERROR(ics->server,
		          "External IOSurface allocator mismatch: consumed=%s expected=%u actual=%u",
		          consumed ? "true" : "false",
		          image_count,
		          xsc != NULL ? xsc->image_count : 0);
		xrt_swapchain_reference(&xsc, NULL);
		return XRT_ERROR_VULKAN;
	}

	xret = comp_swapchain_gpu_reuse_enable(xsc);
	if (xret != XRT_SUCCESS) {
		xrt_swapchain_reference(&xsc, NULL);
		return xret;
	}

	xret = metal_ipc_smart_acquire_enable(xsc);
	if (xret != XRT_SUCCESS) {
		IPC_WARN(ics->server,
		         "External IOSurface smart acquire unavailable; retaining safe FIFO behaviour: result=%d",
		         xret);
	}

	ics->swapchain_count++;
	ics->xscs[index] = xsc;
	ics->swapchain_data[index].active = true;
	ics->swapchain_data[index].width = info->width;
	ics->swapchain_data[index].height = info->height;
	ics->swapchain_data[index].format = info->format;
	ics->swapchain_data[index].image_count = xsc->image_count;
	*out_id = index;

	IPC_INFO(ics->server,
	         "External IOSurface swapchain active: id=%u images=%u size=%ux%u array_size=%u first_surface=%u",
	         index, image_count, info->width, info->height, info->array_size, args->ids[0]);
	return XRT_SUCCESS;
#endif
}

xrt_result_t
ipc_handle_compositor_semaphore_import_metal_bootstrap(volatile struct ipc_client_state *ics,
                                                       const struct ipc_metal_bootstrap_name *bootstrap,
                                                       uint32_t *out_id)
{
	IPC_TRACE_MARKER();

#ifndef XRT_OS_OSX
	(void)ics;
	(void)bootstrap;
	(void)out_id;
	return XRT_ERROR_NOT_IMPLEMENTED;
#else
	if (ics == NULL || bootstrap == NULL || out_id == NULL || ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}
	if (bootstrap->name[0] == '\0' ||
	    memchr(bootstrap->name, '\0', sizeof(bootstrap->name)) == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	uint32_t id = 0;
	xrt_result_t xret = find_free_semaphore_index(ics, &id);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	struct xrt_compositor_semaphore *xcsem = NULL;
	xret = comp_metal_semaphore_import_bootstrap_event(bootstrap->name, &xcsem);
	if (xret != XRT_SUCCESS || xcsem == NULL) {
		IPC_WARN(ics->server,
		         "DXMT shared-event semaphore import unavailable: name='%s' result=%d",
		         bootstrap->name,
		         xret);
		return xret != XRT_SUCCESS ? xret : XRT_ERROR_VULKAN;
	}

	ics->xcsems[id] = xcsem;
	ics->compositor_semaphore_count++;
	*out_id = id;

	IPC_INFO(ics->server,
	         "DXMT shared-event compositor semaphore active: id=%u name='%s'",
	         id,
	         bootstrap->name);
	return XRT_SUCCESS;
#endif
}

xrt_result_t
ipc_handle_compositor_semaphore_create_metal(volatile struct ipc_client_state *ics,
                                             uint32_t *out_id,
                                             uint64_t *out_token)
{
	IPC_TRACE_MARKER();

#ifndef XRT_OS_OSX
	(void)ics;
	(void)out_id;
	(void)out_token;
	return XRT_ERROR_NOT_IMPLEMENTED;
#else
	if (ics == NULL || out_id == NULL || out_token == NULL || ics->xc == NULL) {
		return XRT_ERROR_IPC_SESSION_NOT_CREATED;
	}

	uint32_t id = 0;
	xrt_result_t xret = find_free_semaphore_index(ics, &id);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	struct xrt_compositor_semaphore *xcsem = NULL;
	void *raw_shared_event = NULL;
	xret = comp_metal_semaphore_create_client_pair(&xcsem, &raw_shared_event);
	if (xret != XRT_SUCCESS || xcsem == NULL || raw_shared_event == NULL) {
		IPC_ERROR(ics->server,
		          "Failed to create Metal shared-event compositor semaphore: result=%d",
		          xret);
		if (xcsem != NULL) {
			xrt_compositor_semaphore_reference(&xcsem, NULL);
		}
		return xret != XRT_SUCCESS ? xret : XRT_ERROR_VULKAN;
	}

	uint64_t token = 0;
	xret = ipc_metal_xpc_publish_shared_event(raw_shared_event, &token);
	if (xret != XRT_SUCCESS) {
		xrt_compositor_semaphore_reference(&xcsem, NULL);
		return xret;
	}

	ics->xcsems[id] = xcsem;
	ics->compositor_semaphore_count++;
	*out_id = id;
	*out_token = token;

	IPC_INFO(ics->server,
	         "Metal IPC Stage 4 semaphore active: id=%u token=0x%016llx event=%p",
	         id,
	         (unsigned long long)token,
	         raw_shared_event);

	return XRT_SUCCESS;
#endif
}

// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Metal app-release handoff through Monado's compositor wait thread.
 * @ingroup comp_client
 */

#import <Metal/Metal.h>

#include "client/comp_metal_release_wait_thread.h"
#include "util/comp_metal_semaphore_probe.h"
#include "util/u_debug.h"
#include "util/u_handles.h"
#include "util/u_logging.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

DEBUG_GET_ONCE_BOOL_OPTION(metal_app_release_wait_thread,
                           "XRT_MACOS_APP_RELEASE_SHARED_EVENT_WAIT_THREAD",
                           false)

#define METAL_WAIT_THREAD_LOG_WINDOW 240

struct client_metal_wait_thread_context;

struct client_metal_wait_thread_swapchain
{
	struct xrt_swapchain *xsc;
	struct client_metal_wait_thread_context *owner;
	xrt_result_t (*original_barrier_image)(struct xrt_swapchain *, enum xrt_barrier_direction, uint32_t);
	void (*original_destroy)(struct xrt_swapchain *);
	struct client_metal_wait_thread_swapchain *next;
};

struct client_metal_wait_thread_context
{
	struct xrt_compositor *xc;
	id<MTLCommandQueue> command_queue;
	id<MTLSharedEvent> shared_event;
	struct xrt_compositor_semaphore *xcsem;

	pthread_mutex_t signal_mutex;
	bool signal_mutex_initialized;
	bool pair_attempted;
	bool pair_ready;
	uint64_t next_value;
	uint64_t last_submitted_value;
	uint64_t last_committed_value;
	uint64_t signal_count;
	uint64_t wait_thread_submit_count;

	xrt_result_t (*original_create_swapchain)(struct xrt_compositor *,
	                                          const struct xrt_swapchain_create_info *,
	                                          struct xrt_swapchain **);
	xrt_result_t (*original_layer_commit)(struct xrt_compositor *, xrt_graphics_sync_handle_t);
	xrt_result_t (*original_layer_commit_with_semaphore)(struct xrt_compositor *,
	                                                     struct xrt_compositor_semaphore *,
	                                                     uint64_t);
	void (*original_destroy)(struct xrt_compositor *);

	struct client_metal_wait_thread_context *next;
};

static pthread_mutex_t g_contexts_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct client_metal_wait_thread_context *g_contexts = NULL;
static struct client_metal_wait_thread_swapchain *g_swapchains = NULL;

static struct client_metal_wait_thread_context *
find_context_locked(struct xrt_compositor *xc)
{
	for (struct client_metal_wait_thread_context *c = g_contexts; c != NULL; c = c->next) {
		if (c->xc == xc) {
			return c;
		}
	}
	return NULL;
}

static struct client_metal_wait_thread_swapchain *
find_swapchain_locked(struct xrt_swapchain *xsc)
{
	for (struct client_metal_wait_thread_swapchain *sc = g_swapchains; sc != NULL; sc = sc->next) {
		if (sc->xsc == xsc) {
			return sc;
		}
	}
	return NULL;
}

bool
client_metal_release_wait_thread_enabled(void)
{
	return debug_get_bool_option_metal_app_release_wait_thread();
}

static bool
ensure_pair(struct client_metal_wait_thread_context *c)
{
	pthread_mutex_lock(&c->signal_mutex);
	if (c->pair_attempted) {
		bool ready = c->pair_ready;
		pthread_mutex_unlock(&c->signal_mutex);
		return ready;
	}
	c->pair_attempted = true;

	struct xrt_compositor_semaphore *xcsem = NULL;
	void *raw_shared_event = NULL;
	xrt_result_t xret = comp_metal_semaphore_create_client_pair(&xcsem, &raw_shared_event);
	if (xret != XRT_SUCCESS || xcsem == NULL || raw_shared_event == NULL) {
		U_LOG_W("Metal app-release wait-thread handoff unavailable: result=%d; blocking release handoff unchanged",
		        xret);
		if (xcsem != NULL) {
			xrt_compositor_semaphore_reference(&xcsem, NULL);
		}
		pthread_mutex_unlock(&c->signal_mutex);
		return false;
	}

	id<MTLSharedEvent> event = [(__bridge id<MTLSharedEvent>)raw_shared_event retain];
	if (event == nil) {
		U_LOG_W("Metal app-release wait-thread handoff exported a nil MTLSharedEvent");
		xrt_compositor_semaphore_reference(&xcsem, NULL);
		pthread_mutex_unlock(&c->signal_mutex);
		return false;
	}

	c->xcsem = xcsem;
	c->shared_event = event;
	c->next_value = event.signaledValue;
	c->last_submitted_value = c->next_value;
	c->last_committed_value = c->next_value;
	c->pair_ready = true;

	U_LOG_I("Metal app-release Stage 4 ready: event=%p initial_value=%llu; app CPU barrier bypassed and frame readiness delegated to Monado wait thread",
	        (__bridge void *)event,
	        (unsigned long long)c->next_value);

	pthread_mutex_unlock(&c->signal_mutex);
	return true;
}

static xrt_result_t
wrapped_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	pthread_mutex_lock(&g_contexts_mutex);
	struct client_metal_wait_thread_swapchain *link = find_swapchain_locked(xsc);
	struct client_metal_wait_thread_context *c = link != NULL ? link->owner : NULL;
	xrt_result_t (*original_barrier)(struct xrt_swapchain *, enum xrt_barrier_direction, uint32_t) =
	    link != NULL ? link->original_barrier_image : NULL;
	pthread_mutex_unlock(&g_contexts_mutex);

	if (original_barrier == NULL) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	if (direction != XRT_BARRIER_TO_COMP || c == NULL || !c->pair_ready) {
		return original_barrier(xsc, direction, index);
	}

	@autoreleasepool {
		id<MTLCommandBuffer> signal_buffer = nil;
		uint64_t signal_value = 0;

		pthread_mutex_lock(&c->signal_mutex);
		signal_buffer = [c->command_queue commandBuffer];
		if (signal_buffer != nil) {
			signal_value = ++c->next_value;
			[signal_buffer encodeSignalEvent:c->shared_event value:signal_value];
			[signal_buffer commit];
			c->last_submitted_value = signal_value;
			c->signal_count++;
		}
		pthread_mutex_unlock(&c->signal_mutex);

		if (signal_buffer == nil) {
			U_LOG_W("Metal app-release Stage 4 could not allocate signal command buffer; using blocking barrier");
			return original_barrier(xsc, direction, index);
		}

		// Do not perform the old waitUntilCompleted() barrier. The latest timeline
		// value will be handed to layer_commit_with_semaphore, whose multi-compositor
		// wait thread marks the frame GPU-done and schedulable only after this Metal
		// queue signal has completed.
		return XRT_SUCCESS;
	}
}

static void
wrapped_swapchain_destroy(struct xrt_swapchain *xsc)
{
	void (*original_destroy)(struct xrt_swapchain *) = NULL;

	pthread_mutex_lock(&g_contexts_mutex);
	struct client_metal_wait_thread_swapchain **ptr = &g_swapchains;
	while (*ptr != NULL) {
		if ((*ptr)->xsc == xsc) {
			struct client_metal_wait_thread_swapchain *link = *ptr;
			*ptr = link->next;
			original_destroy = link->original_destroy;
			free(link);
			break;
		}
		ptr = &(*ptr)->next;
	}
	pthread_mutex_unlock(&g_contexts_mutex);

	if (original_destroy != NULL) {
		original_destroy(xsc);
	}
}

static void
wrap_swapchain(struct client_metal_wait_thread_context *c, struct xrt_swapchain *xsc)
{
	if (xsc == NULL || xsc->barrier_image == NULL || xsc->destroy == NULL) {
		return;
	}

	struct client_metal_wait_thread_swapchain *link = calloc(1, sizeof(*link));
	if (link == NULL) {
		U_LOG_W("Could not allocate Metal app-release Stage 4 swapchain wrapper");
		return;
	}

	link->xsc = xsc;
	link->owner = c;
	link->original_barrier_image = xsc->barrier_image;
	link->original_destroy = xsc->destroy;

	pthread_mutex_lock(&g_contexts_mutex);
	link->next = g_swapchains;
	g_swapchains = link;
	xsc->barrier_image = wrapped_barrier_image;
	xsc->destroy = wrapped_swapchain_destroy;
	pthread_mutex_unlock(&g_contexts_mutex);
}

static xrt_result_t
wrapped_create_swapchain(struct xrt_compositor *xc,
                         const struct xrt_swapchain_create_info *info,
                         struct xrt_swapchain **out_xsc)
{
	pthread_mutex_lock(&g_contexts_mutex);
	struct client_metal_wait_thread_context *c = find_context_locked(xc);
	xrt_result_t (*original_create)(struct xrt_compositor *, const struct xrt_swapchain_create_info *, struct xrt_swapchain **) =
	    c != NULL ? c->original_create_swapchain : NULL;
	pthread_mutex_unlock(&g_contexts_mutex);

	if (c == NULL || original_create == NULL) {
		return XRT_ERROR_NOT_IMPLEMENTED;
	}

	xrt_result_t xret = original_create(xc, info, out_xsc);
	if (xret != XRT_SUCCESS || out_xsc == NULL || *out_xsc == NULL) {
		return xret;
	}

	if (ensure_pair(c)) {
		wrap_swapchain(c, *out_xsc);
	}
	return xret;
}

static xrt_result_t
wrapped_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	pthread_mutex_lock(&g_contexts_mutex);
	struct client_metal_wait_thread_context *c = find_context_locked(xc);
	xrt_result_t (*original_layer_commit)(struct xrt_compositor *, xrt_graphics_sync_handle_t) =
	    c != NULL ? c->original_layer_commit : NULL;
	xrt_result_t (*original_layer_commit_with_semaphore)(struct xrt_compositor *, struct xrt_compositor_semaphore *, uint64_t) =
	    c != NULL ? c->original_layer_commit_with_semaphore : NULL;
	pthread_mutex_unlock(&g_contexts_mutex);

	if (c == NULL || original_layer_commit == NULL) {
		u_graphics_sync_unref(&sync_handle);
		return XRT_ERROR_NOT_IMPLEMENTED;
	}
	if (!c->pair_ready || original_layer_commit_with_semaphore == NULL) {
		return original_layer_commit(xc, sync_handle);
	}

	uint64_t wait_value = 0;
	pthread_mutex_lock(&c->signal_mutex);
	if (c->last_submitted_value > c->last_committed_value) {
		wait_value = c->last_submitted_value;
	}
	pthread_mutex_unlock(&c->signal_mutex);

	if (wait_value == 0) {
		return original_layer_commit(xc, sync_handle);
	}

	// Metal has no xrt_graphics_sync_handle here: its synchronization is the
	// shared-event timeline. Hand that timeline to Monado's normal multi-client
	// frame-readiness path instead of blocking the application thread or the
	// comp_main Vulkan queue.
	u_graphics_sync_unref(&sync_handle);
	xrt_result_t xret = original_layer_commit_with_semaphore(xc, c->xcsem, wait_value);
	if (xret != XRT_SUCCESS) {
		U_LOG_W("Metal app-release Stage 4 layer_commit_with_semaphore failed at value %llu (result=%d); falling back to CPU timeline wait",
		        (unsigned long long)wait_value,
		        xret);
		xrt_result_t wait_result = xrt_compositor_semaphore_wait(c->xcsem, wait_value, 1000000000ull);
		if (wait_result != XRT_SUCCESS) {
			U_LOG_E("Metal app-release Stage 4 fallback timeline wait failed at value %llu: result=%d",
			        (unsigned long long)wait_value,
			        wait_result);
			return wait_result;
		}
		return original_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
	}

	pthread_mutex_lock(&c->signal_mutex);
	if (wait_value > c->last_committed_value) {
		c->last_committed_value = wait_value;
	}
	c->wait_thread_submit_count++;
	uint64_t submit_count = c->wait_thread_submit_count;
	uint64_t observed = c->shared_event.signaledValue;
	pthread_mutex_unlock(&c->signal_mutex);

	if ((submit_count % METAL_WAIT_THREAD_LOG_WINDOW) == 0) {
		U_LOG_I("Metal app-release Stage 4: wait-thread handoffs=%llu latest_value=%llu observed_event=%llu; app CPU barrier and comp_main queue wait both bypassed",
		        (unsigned long long)submit_count,
		        (unsigned long long)wait_value,
		        (unsigned long long)observed);
	}

	return XRT_SUCCESS;
}

static void
wrapped_compositor_destroy(struct xrt_compositor *xc)
{
	struct client_metal_wait_thread_context *c = NULL;

	pthread_mutex_lock(&g_contexts_mutex);
	struct client_metal_wait_thread_context **ptr = &g_contexts;
	while (*ptr != NULL) {
		if ((*ptr)->xc == xc) {
			c = *ptr;
			*ptr = c->next;
			break;
		}
		ptr = &(*ptr)->next;
	}
	pthread_mutex_unlock(&g_contexts_mutex);

	if (c == NULL) {
		return;
	}

	void (*original_destroy)(struct xrt_compositor *) = c->original_destroy;
	if (c->shared_event != nil) {
		[c->shared_event release];
		c->shared_event = nil;
	}
	xrt_compositor_semaphore_reference(&c->xcsem, NULL);
	[c->command_queue release];
	c->command_queue = nil;

	if (c->signal_mutex_initialized) {
		pthread_mutex_destroy(&c->signal_mutex);
	}
	free(c);

	if (original_destroy != NULL) {
		original_destroy(xc);
	}
}

struct xrt_compositor_metal *
client_metal_release_wait_thread_attach(struct xrt_compositor_metal *xcm, void *command_queue)
{
	if (xcm == NULL || command_queue == NULL || !client_metal_release_wait_thread_enabled()) {
		return xcm;
	}
	if (xcm->base.layer_commit_with_semaphore == NULL) {
		U_LOG_W("Metal app-release Stage 4 unavailable: compositor has no layer_commit_with_semaphore path");
		return xcm;
	}

	struct client_metal_wait_thread_context *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return xcm;
	}
	if (pthread_mutex_init(&c->signal_mutex, NULL) != 0) {
		free(c);
		return xcm;
	}
	c->signal_mutex_initialized = true;

	c->xc = &xcm->base;
	c->command_queue = [(__bridge id<MTLCommandQueue>)command_queue retain];
	c->original_create_swapchain = xcm->base.create_swapchain;
	c->original_layer_commit = xcm->base.layer_commit;
	c->original_layer_commit_with_semaphore = xcm->base.layer_commit_with_semaphore;
	c->original_destroy = xcm->base.destroy;

	pthread_mutex_lock(&g_contexts_mutex);
	c->next = g_contexts;
	g_contexts = c;
	xcm->base.create_swapchain = wrapped_create_swapchain;
	xcm->base.layer_commit = wrapped_layer_commit;
	xcm->base.destroy = wrapped_compositor_destroy;
	pthread_mutex_unlock(&g_contexts_mutex);

	U_LOG_I("Metal app-release Stage 4 enabled; shared-event completion will gate frame readiness on Monado's wait thread");
	return xcm;
}

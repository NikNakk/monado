// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Diagnostic app-release Metal shared-event synchronization wrapper.
 * @ingroup comp_client
 */

#import <Metal/Metal.h>

#include "client/comp_metal_release_sync.h"
#include "os/os_time.h"
#include "util/comp_metal_semaphore_probe.h"
#include "util/u_debug.h"
#include "util/u_logging.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

DEBUG_GET_ONCE_BOOL_OPTION(metal_app_release_shared_event, "XRT_MACOS_APP_RELEASE_SHARED_EVENT", false)
DEBUG_GET_ONCE_BOOL_OPTION(metal_release_sync_trace, "PSVR2_TIMING_TRACE", false)

#define METAL_RELEASE_SYNC_LOG_WINDOW 240

struct client_metal_release_sync_context;

struct client_metal_release_sync_swapchain
{
	struct xrt_swapchain *xsc;
	struct client_metal_release_sync_context *owner;
	xrt_result_t (*original_barrier_image)(struct xrt_swapchain *, enum xrt_barrier_direction, uint32_t);
	void (*original_destroy)(struct xrt_swapchain *);
	struct client_metal_release_sync_swapchain *next;
};

struct client_metal_release_sync_context
{
	struct xrt_compositor *xc;
	id<MTLCommandQueue> command_queue;
	id<MTLSharedEvent> shared_event;
	struct xrt_compositor_semaphore *xcsem;

	pthread_mutex_t signal_mutex;
	bool signal_mutex_initialized;
	pthread_mutex_t trace_mutex;
	bool trace_mutex_initialized;

	bool pair_attempted;
	bool pair_ready;
	uint64_t next_value;
	uint64_t last_submitted_value;
	uint64_t signal_count;

	FILE *trace_file;
	uint64_t trace_rows;

	xrt_result_t (*original_create_swapchain)(struct xrt_compositor *,
	                                          const struct xrt_swapchain_create_info *,
	                                          struct xrt_swapchain **);
	void (*original_destroy)(struct xrt_compositor *);

	struct client_metal_release_sync_context *next;
};

static pthread_mutex_t g_contexts_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct client_metal_release_sync_context *g_contexts = NULL;
static struct client_metal_release_sync_swapchain *g_swapchains = NULL;

static struct client_metal_release_sync_context *
find_context_locked(struct xrt_compositor *xc)
{
	for (struct client_metal_release_sync_context *c = g_contexts; c != NULL; c = c->next) {
		if (c->xc == xc) {
			return c;
		}
	}
	return NULL;
}

static struct client_metal_release_sync_swapchain *
find_swapchain_locked(struct xrt_swapchain *xsc)
{
	for (struct client_metal_release_sync_swapchain *sc = g_swapchains; sc != NULL; sc = sc->next) {
		if (sc->xsc == xsc) {
			return sc;
		}
	}
	return NULL;
}

static void
trace_open(struct client_metal_release_sync_context *c)
{
	if (!debug_get_bool_option_metal_release_sync_trace()) {
		return;
	}

	const char *dir = getenv("PSVR2_TIMING_TRACE_DIR");
	if (dir == NULL || dir[0] == '\0') {
		dir = "/tmp";
	}

	char path[1024];
	const size_t dir_len = strlen(dir);
	const char *separator = dir_len > 0 && dir[dir_len - 1] == '/' ? "" : "/";
	snprintf(path,
	         sizeof(path),
	         "%s%smonado_psvr2_%d_metal_release_shared_event.csv",
	         dir,
	         separator,
	         (int)getpid());

	c->trace_file = fopen(path, "w");
	if (c->trace_file == NULL) {
		U_LOG_W("Could not open Metal app-release shared-event trace '%s'", path);
		return;
	}

	setvbuf(c->trace_file, NULL, _IOFBF, 64 * 1024);
	fputs("sequence,swapchain_ptr,image_index,signal_value,event_value_before,before_signal_ns,after_signal_commit_ns,"
	      "after_blocking_barrier_ns,event_value_after,signal_submit_duration_ns,blocking_barrier_duration_ns,"
	      "signal_command_status,barrier_result\n",
	      c->trace_file);
	fflush(c->trace_file);
	U_LOG_I("Metal app-release shared-event diagnostic trace: %s", path);
}

static void
trace_record(struct client_metal_release_sync_context *c,
             struct xrt_swapchain *xsc,
             uint32_t image_index,
             uint64_t signal_value,
             uint64_t event_value_before,
             uint64_t before_signal_ns,
             uint64_t after_signal_commit_ns,
             uint64_t after_blocking_barrier_ns,
             uint64_t event_value_after,
             MTLCommandBufferStatus signal_status,
             xrt_result_t barrier_result)
{
	if (c->trace_file == NULL || !c->trace_mutex_initialized) {
		return;
	}

	const uint64_t signal_submit_duration_ns = after_signal_commit_ns >= before_signal_ns
	                                               ? after_signal_commit_ns - before_signal_ns
	                                               : 0;
	const uint64_t blocking_barrier_duration_ns = after_blocking_barrier_ns >= after_signal_commit_ns
	                                                  ? after_blocking_barrier_ns - after_signal_commit_ns
	                                                  : 0;

	pthread_mutex_lock(&c->trace_mutex);
	const uint64_t sequence = ++c->trace_rows;
	fprintf(c->trace_file,
	        "%llu,%p,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%ld,%d\n",
	        (unsigned long long)sequence,
	        (void *)xsc,
	        image_index,
	        (unsigned long long)signal_value,
	        (unsigned long long)event_value_before,
	        (unsigned long long)before_signal_ns,
	        (unsigned long long)after_signal_commit_ns,
	        (unsigned long long)after_blocking_barrier_ns,
	        (unsigned long long)event_value_after,
	        (unsigned long long)signal_submit_duration_ns,
	        (unsigned long long)blocking_barrier_duration_ns,
	        (long)signal_status,
	        (int)barrier_result);
	if ((sequence % METAL_RELEASE_SYNC_LOG_WINDOW) == 0) {
		fflush(c->trace_file);
	}
	pthread_mutex_unlock(&c->trace_mutex);
}

static void
trace_close(struct client_metal_release_sync_context *c)
{
	if (!c->trace_mutex_initialized) {
		return;
	}

	pthread_mutex_lock(&c->trace_mutex);
	if (c->trace_file != NULL) {
		fflush(c->trace_file);
		fclose(c->trace_file);
		c->trace_file = NULL;
	}
	pthread_mutex_unlock(&c->trace_mutex);
}

static bool
ensure_pair(struct client_metal_release_sync_context *c)
{
	pthread_mutex_lock(&c->signal_mutex);
	if (c->pair_attempted) {
		const bool ready = c->pair_ready;
		pthread_mutex_unlock(&c->signal_mutex);
		return ready;
	}
	c->pair_attempted = true;

	struct xrt_compositor_semaphore *xcsem = NULL;
	void *raw_shared_event = NULL;
	xrt_result_t xret = comp_metal_semaphore_create_client_pair(&xcsem, &raw_shared_event);
	if (xret != XRT_SUCCESS || xcsem == NULL || raw_shared_event == NULL) {
		U_LOG_W("Metal app-release shared-event diagnostic unavailable: result=%d; blocking release handoff unchanged",
		        xret);
		if (xcsem != NULL) {
			xrt_compositor_semaphore_reference(&xcsem, NULL);
		}
		pthread_mutex_unlock(&c->signal_mutex);
		return false;
	}

	id<MTLSharedEvent> shared_event = [(__bridge id<MTLSharedEvent>)raw_shared_event retain];
	if (shared_event == nil) {
		U_LOG_W("Metal app-release shared-event diagnostic export produced nil event; blocking release handoff unchanged");
		xrt_compositor_semaphore_reference(&xcsem, NULL);
		pthread_mutex_unlock(&c->signal_mutex);
		return false;
	}

	c->xcsem = xcsem;
	c->shared_event = shared_event;
	c->next_value = shared_event.signaledValue;
	c->last_submitted_value = c->next_value;
	c->pair_ready = true;
	U_LOG_I("Metal app-release shared-event Stage 2 ready: event=%p initial_value=%llu; CPU wait remains active",
	        (__bridge void *)shared_event,
	        (unsigned long long)c->next_value);
	pthread_mutex_unlock(&c->signal_mutex);
	return true;
}

static xrt_result_t
wrapped_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	pthread_mutex_lock(&g_contexts_mutex);
	struct client_metal_release_sync_swapchain *link = find_swapchain_locked(xsc);
	struct client_metal_release_sync_context *c = link != NULL ? link->owner : NULL;
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
		uint64_t event_value_before = 0;
		uint64_t before_signal_ns = os_monotonic_get_ns();
		uint64_t after_signal_commit_ns = before_signal_ns;

		// Serialize command-buffer creation, timeline value allocation and commit.
		// Metal command queues execute committed command buffers in order, so this
		// ensures timeline values increase in exactly the order they are queued.
		pthread_mutex_lock(&c->signal_mutex);
		signal_buffer = [c->command_queue commandBuffer];
		if (signal_buffer != nil) {
			signal_value = ++c->next_value;
			event_value_before = c->shared_event.signaledValue;
			[signal_buffer encodeSignalEvent:c->shared_event value:signal_value];
			[signal_buffer commit];
			c->last_submitted_value = signal_value;
			c->signal_count++;
			after_signal_commit_ns = os_monotonic_get_ns();
		}
		pthread_mutex_unlock(&c->signal_mutex);

		if (signal_buffer == nil) {
			U_LOG_W("Metal app-release shared-event diagnostic could not allocate signal command buffer; using blocking handoff only");
			return original_barrier(xsc, direction, index);
		}

		// Stage 2 deliberately preserves the original barrier. It submits its own
		// marker on this same queue and blocks until it completes, so application
		// visibility semantics are unchanged while we validate the shared event.
		xrt_result_t xret = original_barrier(xsc, direction, index);
		const uint64_t after_blocking_barrier_ns = os_monotonic_get_ns();
		const uint64_t event_value_after = c->shared_event.signaledValue;
		const MTLCommandBufferStatus signal_status = signal_buffer.status;

		trace_record(c,
		             xsc,
		             index,
		             signal_value,
		             event_value_before,
		             before_signal_ns,
		             after_signal_commit_ns,
		             after_blocking_barrier_ns,
		             event_value_after,
		             signal_status,
		             xret);

		if (signal_status == MTLCommandBufferStatusError) {
			U_LOG_W("Metal app-release shared-event signal command buffer failed at value %llu; blocking handoff remained authoritative",
			        (unsigned long long)signal_value);
		} else if (xret == XRT_SUCCESS && event_value_after < signal_value) {
			U_LOG_W("Metal app-release shared-event did not advance after blocking barrier: expected >=%llu got %llu",
			        (unsigned long long)signal_value,
			        (unsigned long long)event_value_after);
		}

		if ((c->signal_count % METAL_RELEASE_SYNC_LOG_WINDOW) == 0) {
			U_LOG_I("Metal app-release shared-event Stage 2: submitted=%llu latest=%llu observed=%llu; CPU wait remains active",
			        (unsigned long long)c->signal_count,
			        (unsigned long long)signal_value,
			        (unsigned long long)event_value_after);
		}

		return xret;
	}
}

static void
wrapped_swapchain_destroy(struct xrt_swapchain *xsc)
{
	void (*original_destroy)(struct xrt_swapchain *) = NULL;

	pthread_mutex_lock(&g_contexts_mutex);
	struct client_metal_release_sync_swapchain **ptr = &g_swapchains;
	while (*ptr != NULL) {
		if ((*ptr)->xsc == xsc) {
			struct client_metal_release_sync_swapchain *link = *ptr;
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
wrap_swapchain(struct client_metal_release_sync_context *c, struct xrt_swapchain *xsc)
{
	if (xsc == NULL || xsc->barrier_image == NULL || xsc->destroy == NULL) {
		return;
	}

	struct client_metal_release_sync_swapchain *link = calloc(1, sizeof(*link));
	if (link == NULL) {
		U_LOG_W("Could not allocate Metal app-release shared-event swapchain diagnostic wrapper");
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
	struct client_metal_release_sync_context *c = find_context_locked(xc);
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

	// The native swapchain creation inside the Metal client has now reached
	// comp_base_create_swapchain, which registers the Stage-1 Vulkan provider.
	if (ensure_pair(c)) {
		wrap_swapchain(c, *out_xsc);
	}

	return xret;
}

static void
wrapped_compositor_destroy(struct xrt_compositor *xc)
{
	struct client_metal_release_sync_context *c = NULL;

	pthread_mutex_lock(&g_contexts_mutex);
	struct client_metal_release_sync_context **ptr = &g_contexts;
	while (*ptr != NULL) {
		if ((*ptr)->xc == xc) {
			c = *ptr;
			*ptr = c->next;
			break;
		}
		ptr = &(*ptr)->next;
	}

	if (c != NULL) {
		for (struct client_metal_release_sync_swapchain *sc = g_swapchains; sc != NULL; sc = sc->next) {
			if (sc->owner == c) {
				U_LOG_W("Metal app-release shared-event compositor destroyed while wrapped swapchain still exists");
				break;
			}
		}
	}
	pthread_mutex_unlock(&g_contexts_mutex);

	if (c == NULL) {
		return;
	}

	void (*original_destroy)(struct xrt_compositor *) = c->original_destroy;

	trace_close(c);
	if (c->shared_event != nil) {
		[c->shared_event release];
		c->shared_event = nil;
	}
	// Destroy the backing Vulkan semaphore only after releasing its exported
	// Objective-C representation.
	xrt_compositor_semaphore_reference(&c->xcsem, NULL);
	[c->command_queue release];
	c->command_queue = nil;

	if (c->trace_mutex_initialized) {
		pthread_mutex_destroy(&c->trace_mutex);
	}
	if (c->signal_mutex_initialized) {
		pthread_mutex_destroy(&c->signal_mutex);
	}
	free(c);

	if (original_destroy != NULL) {
		original_destroy(xc);
	}
}

struct xrt_compositor_metal *
client_metal_release_sync_attach(struct xrt_compositor_metal *xcm, void *command_queue)
{
	if (xcm == NULL || command_queue == NULL || !debug_get_bool_option_metal_app_release_shared_event()) {
		return xcm;
	}

	struct client_metal_release_sync_context *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		U_LOG_W("Could not allocate Metal app-release shared-event diagnostic context; blocking handoff unchanged");
		return xcm;
	}

	if (pthread_mutex_init(&c->signal_mutex, NULL) != 0) {
		free(c);
		U_LOG_W("Could not initialize Metal app-release shared-event mutex; blocking handoff unchanged");
		return xcm;
	}
	c->signal_mutex_initialized = true;

	if (pthread_mutex_init(&c->trace_mutex, NULL) != 0) {
		pthread_mutex_destroy(&c->signal_mutex);
		free(c);
		U_LOG_W("Could not initialize Metal app-release shared-event trace mutex; blocking handoff unchanged");
		return xcm;
	}
	c->trace_mutex_initialized = true;

	c->xc = &xcm->base;
	c->command_queue = [(__bridge id<MTLCommandQueue>)command_queue retain];
	c->original_create_swapchain = xcm->base.create_swapchain;
	c->original_destroy = xcm->base.destroy;
	trace_open(c);

	pthread_mutex_lock(&g_contexts_mutex);
	c->next = g_contexts;
	g_contexts = c;
	xcm->base.create_swapchain = wrapped_create_swapchain;
	xcm->base.destroy = wrapped_compositor_destroy;
	pthread_mutex_unlock(&g_contexts_mutex);

	U_LOG_I("Metal app-release shared-event Stage 2 enabled; original blocking barrier remains authoritative");
	return xcm;
}

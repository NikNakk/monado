// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Fine-grained compositor GPU completion tracking for swapchain image reuse.
 * @ingroup comp_util
 */

#include "util/comp_swapchain_gpu_reuse.h"
#include "util/comp_swapchain_gpu_reuse_internal.h"
#include "util/comp_swapchain.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include "os/os_time.h"

#include "vk/vk_submit_helpers.h"

#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

DEBUG_GET_ONCE_BOOL_OPTION(log_swapchain_gpu_reuse, "XRT_COMPOSITOR_LOG_SWAPCHAIN_GPU_REUSE", false)

#define GPU_REUSE_LOG(...)                                                                                             \
	do {                                                                                                           \
		if (debug_get_bool_option_log_swapchain_gpu_reuse()) {                                                 \
			U_LOG_I(__VA_ARGS__);                                                                              \
		} else {                                                                                               \
			U_LOG_D(__VA_ARGS__);                                                                              \
		}                                                                                                      \
	} while (false)

#define GPU_REUSE_MAX_IMAGES_PER_SUBMIT (XRT_MAX_LAYERS * XRT_MAX_VIEWS * 2)

struct gpu_reuse_context;

struct gpu_reuse_tracker
{
	struct xrt_swapchain *xsc;
	struct comp_swapchain *sc;
	struct gpu_reuse_context *context;
	uint64_t last_gpu_use[XRT_MAX_SWAPCHAIN_IMAGES];
	uint32_t pending_consumers[XRT_MAX_SWAPCHAIN_IMAGES];
	xrt_result_t (*original_wait_image)(struct xrt_swapchain *, int64_t, uint32_t);
	void (*original_destroy)(struct xrt_swapchain *);
	struct gpu_reuse_tracker *next;
};

struct gpu_reuse_context
{
	struct vk_bundle *vk;
	VkSemaphore timeline;
	uint64_t next_value;
	uint64_t last_submitted_value;
	struct gpu_reuse_tracker *trackers;
	struct gpu_reuse_context *next;
};

struct gpu_reuse_native_claim
{
	struct xrt_swapchain *xsc;
	uint32_t image_index;
};

struct gpu_reuse_native_accum
{
	struct comp_layer_accum *cla;
	uint32_t count;
	struct gpu_reuse_native_claim claims[GPU_REUSE_MAX_IMAGES_PER_SUBMIT];
	struct gpu_reuse_native_accum *next;
};

struct gpu_reuse_submit_image
{
	struct gpu_reuse_tracker *tracker;
	uint32_t image_index;
	uint64_t previous_value;
};

struct gpu_reuse_submit
{
	struct gpu_reuse_context *context;
	uint64_t value;
	uint32_t image_count;
	struct gpu_reuse_submit_image images[GPU_REUSE_MAX_IMAGES_PER_SUBMIT];
};

static pthread_mutex_t g_gpu_reuse_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gpu_reuse_cond = PTHREAD_COND_INITIALIZER;
static struct gpu_reuse_context *g_gpu_reuse_contexts = NULL;
static struct gpu_reuse_native_accum *g_gpu_reuse_native_accums = NULL;

static struct gpu_reuse_context *
find_context_locked(struct vk_bundle *vk)
{
	for (struct gpu_reuse_context *c = g_gpu_reuse_contexts; c != NULL; c = c->next) {
		if (c->vk == vk) {
			return c;
		}
	}
	return NULL;
}

static struct gpu_reuse_tracker *
find_tracker_locked(struct xrt_swapchain *xsc)
{
	for (struct gpu_reuse_context *c = g_gpu_reuse_contexts; c != NULL; c = c->next) {
		for (struct gpu_reuse_tracker *t = c->trackers; t != NULL; t = t->next) {
			if (t->xsc == xsc) {
				return t;
			}
	}
	}
	return NULL;
}

static struct gpu_reuse_tracker *
find_tracker_in_context_locked(struct gpu_reuse_context *context, struct xrt_swapchain *xsc)
{
	if (context == NULL) {
		return NULL;
	}
	for (struct gpu_reuse_tracker *t = context->trackers; t != NULL; t = t->next) {
		if (t->xsc == xsc) {
			return t;
		}
	}
	return NULL;
}

static struct gpu_reuse_native_accum *
find_native_accum_locked(struct comp_layer_accum *cla)
{
	for (struct gpu_reuse_native_accum *a = g_gpu_reuse_native_accums; a != NULL; a = a->next) {
		if (a->cla == cla) {
			return a;
		}
	}
	return NULL;
}

static struct gpu_reuse_context *
create_context_locked(struct vk_bundle *vk)
{
#ifdef VK_KHR_timeline_semaphore
	if (vk == NULL || !vk->features.timeline_semaphore || vk->vkCreateSemaphore == NULL ||
	    vk->vkWaitSemaphores == NULL) {
		return NULL;
	}

	VkSemaphoreTypeCreateInfo type_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
	    .pNext = NULL,
	    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	    .initialValue = 0,
	};
	VkSemaphoreCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	    .pNext = &type_info,
	    .flags = 0,
	};

	VkSemaphore timeline = VK_NULL_HANDLE;
	VkResult ret = vk->vkCreateSemaphore(vk->device, &create_info, NULL, &timeline);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "Could not create swapchain GPU reuse timeline: %s", vk_result_string(ret));
		return NULL;
	}

	struct gpu_reuse_context *context = U_TYPED_CALLOC(struct gpu_reuse_context);
	if (context == NULL) {
		vk->vkDestroySemaphore(vk->device, timeline, NULL);
		return NULL;
	}

	VK_NAME_SEMAPHORE(vk, timeline, "comp swapchain GPU reuse timeline");
	context->vk = vk;
	context->timeline = timeline;
	context->next = g_gpu_reuse_contexts;
	g_gpu_reuse_contexts = context;
	return context;
#else
	(void)vk;
	return NULL;
#endif
}

static int64_t
remaining_timeout_ns(int64_t timeout_ns, int64_t start_ns)
{
	if (timeout_ns == INT64_MAX) {
		return INT64_MAX;
	}
	if (timeout_ns <= 0) {
		return 0;
	}

	int64_t now_ns = os_monotonic_get_ns();
	int64_t elapsed_ns = now_ns > start_ns ? now_ns - start_ns : 0;
	return elapsed_ns >= timeout_ns ? 0 : timeout_ns - elapsed_ns;
}

static bool
wait_pending_consumers_locked(struct gpu_reuse_tracker *tracker,
                              uint32_t image_index,
                              int64_t timeout_ns,
                              int64_t start_ns)
{
	while (tracker->pending_consumers[image_index] != 0) {
		if (timeout_ns == INT64_MAX) {
			pthread_cond_wait(&g_gpu_reuse_cond, &g_gpu_reuse_mutex);
			continue;
		}

		int64_t remaining_ns = remaining_timeout_ns(timeout_ns, start_ns);
		if (remaining_ns <= 0) {
			return false;
		}

		int64_t now_rt = os_realtime_get_ns();
		int64_t end_rt = remaining_ns > INT64_MAX - now_rt ? INT64_MAX : now_rt + remaining_ns;
		struct timespec spec;
		os_ns_to_timespec(end_rt, &spec);
		int ret = pthread_cond_timedwait(&g_gpu_reuse_cond, &g_gpu_reuse_mutex, &spec);
		if (ret == ETIMEDOUT && remaining_timeout_ns(timeout_ns, start_ns) <= 0) {
			return false;
		}
	}
	return true;
}

static xrt_result_t
tracked_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t image_index)
{
	if (image_index >= xsc->image_count) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	int64_t start_ns = os_monotonic_get_ns();

	for (;;) {
		xrt_result_t (*original_wait)(struct xrt_swapchain *, int64_t, uint32_t) = NULL;

		pthread_mutex_lock(&g_gpu_reuse_mutex);
		struct gpu_reuse_tracker *tracker = find_tracker_locked(xsc);
		if (tracker != NULL) {
			original_wait = tracker->original_wait_image;
		}
		pthread_mutex_unlock(&g_gpu_reuse_mutex);

		if (tracker == NULL || original_wait == NULL) {
			return XRT_ERROR_VULKAN;
		}

		int64_t remaining_ns = remaining_timeout_ns(timeout_ns, start_ns);
		xrt_result_t xret = original_wait(xsc, remaining_ns, image_index);
		if (xret != XRT_SUCCESS) {
			return xret;
		}

		pthread_mutex_lock(&g_gpu_reuse_mutex);
		tracker = find_tracker_locked(xsc);
		if (tracker == NULL) {
			pthread_mutex_unlock(&g_gpu_reuse_mutex);
			return XRT_SUCCESS;
		}

		if (!wait_pending_consumers_locked(tracker, image_index, timeout_ns, start_ns)) {
			pthread_mutex_unlock(&g_gpu_reuse_mutex);
			return XRT_TIMEOUT;
		}

		uint64_t value = tracker->last_gpu_use[image_index];
		VkSemaphore timeline = tracker->context->timeline;
		struct vk_bundle *vk = tracker->context->vk;
		pthread_mutex_unlock(&g_gpu_reuse_mutex);

		if (value == 0) {
			return XRT_SUCCESS;
		}

		remaining_ns = remaining_timeout_ns(timeout_ns, start_ns);
		uint64_t completed_value = 0;
		bool have_completed_value = vk->vkGetSemaphoreCounterValue != NULL &&
		                            vk->vkGetSemaphoreCounterValue(vk->device, timeline, &completed_value) == VK_SUCCESS;
		bool needs_wait = !have_completed_value || completed_value < value;
		if (needs_wait) {
			GPU_REUSE_LOG("Swapchain GPU reuse wait: swapchain=%p image=%u timeline=%llu completed=%llu timeout_ns=%lld",
			              (void *)xsc,
			              image_index,
			              (unsigned long long)value,
			              (unsigned long long)completed_value,
			              (long long)remaining_ns);
		}

		VkSemaphoreWaitInfo wait_info = {
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		    .flags = 0,
		    .semaphoreCount = 1,
		    .pSemaphores = &timeline,
		    .pValues = &value,
		};
		VkResult ret = vk->vkWaitSemaphores(vk->device, &wait_info, (uint64_t)remaining_ns);
		if (ret == VK_TIMEOUT) {
			return XRT_TIMEOUT;
		}
		if (ret != VK_SUCCESS) {
			VK_ERROR(vk,
			         "Swapchain GPU reuse vkWaitSemaphores image %u value %llu: %s",
			         image_index,
			         (unsigned long long)value,
			         vk_result_string(ret));
			return XRT_ERROR_VULKAN;
		}

		if (needs_wait) {
			GPU_REUSE_LOG("Swapchain GPU reuse wait complete: swapchain=%p image=%u timeline=%llu",
			              (void *)xsc,
			              image_index,
			              (unsigned long long)value);
		}

		/*
		 * A new compositor claim cannot normally appear after OpenXR has handed
		 * the image to the app, but recheck both domains so correctness does not
		 * depend on that external ownership invariant.
		 */
		pthread_mutex_lock(&g_gpu_reuse_mutex);
		tracker = find_tracker_locked(xsc);
		if (tracker == NULL) {
			pthread_mutex_unlock(&g_gpu_reuse_mutex);
			return XRT_SUCCESS;
		}
		bool stable = tracker->pending_consumers[image_index] == 0 && tracker->last_gpu_use[image_index] == value;
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		if (stable) {
			return XRT_SUCCESS;
		}
	}
}

static void
destroy_context_after_last_tracker(struct gpu_reuse_context *context)
{
	if (context == NULL) {
		return;
	}

#ifdef VK_KHR_timeline_semaphore
	if (context->last_submitted_value != 0) {
		VkSemaphoreWaitInfo wait_info = {
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		    .flags = 0,
		    .semaphoreCount = 1,
		    .pSemaphores = &context->timeline,
		    .pValues = &context->last_submitted_value,
		};
		VkResult ret = context->vk->vkWaitSemaphores(context->vk->device, &wait_info, UINT64_MAX);
		if (ret != VK_SUCCESS) {
			VK_ERROR(context->vk, "Waiting for final swapchain GPU reuse value: %s", vk_result_string(ret));
		}
	}
#endif

	context->vk->vkDestroySemaphore(context->vk->device, context->timeline, NULL);
	free(context);
}

static void
tracked_swapchain_destroy(struct xrt_swapchain *xsc)
{
	void (*original_destroy)(struct xrt_swapchain *) = NULL;
	struct gpu_reuse_tracker *removed = NULL;
	struct gpu_reuse_context *empty_context = NULL;

	pthread_mutex_lock(&g_gpu_reuse_mutex);
	struct gpu_reuse_tracker *tracker = find_tracker_locked(xsc);
	if (tracker != NULL) {
		struct gpu_reuse_context *context = tracker->context;
		struct gpu_reuse_tracker **tracker_ptr = &context->trackers;
		while (*tracker_ptr != NULL) {
			if (*tracker_ptr == tracker) {
				*tracker_ptr = tracker->next;
				break;
			}
			tracker_ptr = &(*tracker_ptr)->next;
		}

		original_destroy = tracker->original_destroy;
		xsc->wait_image = tracker->original_wait_image;
		xsc->destroy = tracker->original_destroy;
		removed = tracker;

		if (context->trackers == NULL) {
			struct gpu_reuse_context **context_ptr = &g_gpu_reuse_contexts;
			while (*context_ptr != NULL) {
				if (*context_ptr == context) {
					*context_ptr = context->next;
					break;
				}
				context_ptr = &(*context_ptr)->next;
			}
			empty_context = context;
		}
	}
	pthread_mutex_unlock(&g_gpu_reuse_mutex);

	free(removed);
	destroy_context_after_last_tracker(empty_context);
	if (original_destroy != NULL) {
		original_destroy(xsc);
	}
}

xrt_result_t
comp_swapchain_gpu_reuse_enable(struct xrt_swapchain *xsc)
{
	if (xsc == NULL || xsc->wait_image == NULL || xsc->destroy == NULL || xsc->image_count == 0 ||
	    xsc->image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	struct comp_swapchain *sc = comp_swapchain(xsc);
	if (sc->vk == NULL) {
		return XRT_ERROR_VULKAN;
	}

	pthread_mutex_lock(&g_gpu_reuse_mutex);
	if (find_tracker_locked(xsc) != NULL) {
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		return XRT_SUCCESS;
	}

	struct gpu_reuse_context *context = find_context_locked(sc->vk);
	if (context == NULL) {
		context = create_context_locked(sc->vk);
	}
	if (context == NULL) {
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		U_LOG_E("Swapchain GPU reuse tracking requires Vulkan timeline semaphore support");
		return XRT_ERROR_VULKAN;
	}

	struct gpu_reuse_tracker *tracker = U_TYPED_CALLOC(struct gpu_reuse_tracker);
	if (tracker == NULL) {
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		return XRT_ERROR_ALLOCATION;
	}

	tracker->xsc = xsc;
	tracker->sc = sc;
	tracker->context = context;
	tracker->original_wait_image = xsc->wait_image;
	tracker->original_destroy = xsc->destroy;
	tracker->next = context->trackers;
	context->trackers = tracker;

	xsc->wait_image = tracked_wait_image;
	xsc->destroy = tracked_swapchain_destroy;
	pthread_mutex_unlock(&g_gpu_reuse_mutex);

	U_LOG_I("Swapchain GPU reuse tracking enabled: swapchain=%p images=%u timeline=%p",
	        (void *)xsc,
	        xsc->image_count,
	        (void *)context->timeline);
	return XRT_SUCCESS;
}

bool
comp_swapchain_gpu_reuse_claim_image(struct xrt_swapchain *xsc, uint32_t image_index)
{
	if (xsc == NULL || image_index >= xsc->image_count) {
		return false;
	}

	pthread_mutex_lock(&g_gpu_reuse_mutex);
	struct gpu_reuse_tracker *tracker = find_tracker_locked(xsc);
	if (tracker == NULL) {
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		return false;
	}

	assert(tracker->pending_consumers[image_index] != UINT32_MAX);
	tracker->pending_consumers[image_index]++;
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
	return true;
}

void
comp_swapchain_gpu_reuse_release_image(struct xrt_swapchain *xsc, uint32_t image_index)
{
	if (xsc == NULL || image_index >= xsc->image_count) {
		return;
	}

	pthread_mutex_lock(&g_gpu_reuse_mutex);
	struct gpu_reuse_tracker *tracker = find_tracker_locked(xsc);
	if (tracker != NULL) {
		assert(tracker->pending_consumers[image_index] > 0);
		if (tracker->pending_consumers[image_index] > 0) {
			tracker->pending_consumers[image_index]--;
			if (tracker->pending_consumers[image_index] == 0) {
				pthread_cond_broadcast(&g_gpu_reuse_cond);
			}
		}
	}
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
}

static xrt_result_t
native_accum_claim_image_locked(struct comp_layer_accum *cla, struct xrt_swapchain *xsc, uint32_t image_index)
{
	if (xsc == NULL || image_index >= xsc->image_count) {
		return XRT_SUCCESS;
	}

	struct gpu_reuse_tracker *tracker = find_tracker_locked(xsc);
	if (tracker == NULL) {
		return XRT_SUCCESS;
	}

	struct gpu_reuse_native_accum *accum = find_native_accum_locked(cla);
	if (accum == NULL) {
		accum = U_TYPED_CALLOC(struct gpu_reuse_native_accum);
		if (accum == NULL) {
			return XRT_ERROR_ALLOCATION;
		}
		accum->cla = cla;
		accum->next = g_gpu_reuse_native_accums;
		g_gpu_reuse_native_accums = accum;
	}

	if (accum->count >= ARRAY_SIZE(accum->claims)) {
		U_LOG_E("Too many native swapchain GPU reuse claims in one compositor frame");
		return XRT_ERROR_ALLOCATION;
	}

	assert(tracker->pending_consumers[image_index] != UINT32_MAX);
	tracker->pending_consumers[image_index]++;

	struct gpu_reuse_native_claim *claim = &accum->claims[accum->count++];
	xrt_swapchain_reference(&claim->xsc, xsc);
	claim->image_index = image_index;
	return XRT_SUCCESS;
}

static struct gpu_reuse_native_accum *
detach_native_accum_locked(struct comp_layer_accum *cla)
{
	struct gpu_reuse_native_accum **ptr = &g_gpu_reuse_native_accums;
	while (*ptr != NULL) {
		if ((*ptr)->cla != cla) {
			ptr = &(*ptr)->next;
			continue;
		}

		struct gpu_reuse_native_accum *accum = *ptr;
		*ptr = accum->next;
		accum->next = NULL;

		for (uint32_t i = 0; i < accum->count; i++) {
			struct gpu_reuse_native_claim *claim = &accum->claims[i];
			struct gpu_reuse_tracker *tracker = find_tracker_locked(claim->xsc);
			if (tracker == NULL || claim->image_index >= claim->xsc->image_count) {
				continue;
			}
			assert(tracker->pending_consumers[claim->image_index] > 0);
			if (tracker->pending_consumers[claim->image_index] > 0) {
				tracker->pending_consumers[claim->image_index]--;
			}
		}
		pthread_cond_broadcast(&g_gpu_reuse_cond);
		return accum;
	}
	return NULL;
}

static void
drop_native_accum_refs(struct gpu_reuse_native_accum *accum)
{
	if (accum == NULL) {
		return;
	}
	for (uint32_t i = 0; i < accum->count; i++) {
		xrt_swapchain_reference(&accum->claims[i].xsc, NULL);
	}
	free(accum);
}

void
comp_swapchain_gpu_reuse_native_accum_release(struct comp_layer_accum *cla)
{
	if (cla == NULL) {
		return;
	}

	pthread_mutex_lock(&g_gpu_reuse_mutex);
	struct gpu_reuse_native_accum *accum = detach_native_accum_locked(cla);
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
	drop_native_accum_refs(accum);
}

void
comp_swapchain_gpu_reuse_native_accum_begin(struct comp_layer_accum *cla)
{
	/* Release an error/early-return frame that never reached renderer submit. */
	comp_swapchain_gpu_reuse_native_accum_release(cla);
}

xrt_result_t
comp_swapchain_gpu_reuse_native_accum_claim_layer(struct comp_layer_accum *cla, const struct comp_layer *layer)
{
	if (cla == NULL || layer == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	const struct xrt_layer_data *data = &layer->data;
	xrt_result_t xret = XRT_SUCCESS;

	pthread_mutex_lock(&g_gpu_reuse_mutex);
	switch (data->type) {
	case XRT_LAYER_PROJECTION:
		for (uint32_t i = 0; i < data->view_count && xret == XRT_SUCCESS; i++) {
			xret = native_accum_claim_image_locked(cla, layer->sc_array[i], data->proj.v[i].sub.image_index);
		}
		break;
	case XRT_LAYER_PROJECTION_DEPTH:
		for (uint32_t i = 0; i < data->view_count && xret == XRT_SUCCESS; i++) {
			xret = native_accum_claim_image_locked(cla, layer->sc_array[i], data->depth.v[i].sub.image_index);
			if (xret == XRT_SUCCESS) {
				xret = native_accum_claim_image_locked(
				    cla, layer->sc_array[i + data->view_count], data->depth.d[i].sub.image_index);
			}
		}
		break;
	case XRT_LAYER_QUAD:
		xret = native_accum_claim_image_locked(cla, layer->sc_array[0], data->quad.sub.image_index);
		break;
	case XRT_LAYER_CUBE:
		xret = native_accum_claim_image_locked(cla, layer->sc_array[0], data->cube.sub.image_index);
		break;
	case XRT_LAYER_CYLINDER:
		xret = native_accum_claim_image_locked(cla, layer->sc_array[0], data->cylinder.sub.image_index);
		break;
	case XRT_LAYER_EQUIRECT1:
		xret = native_accum_claim_image_locked(cla, layer->sc_array[0], data->equirect1.sub.image_index);
		break;
	case XRT_LAYER_EQUIRECT2:
		xret = native_accum_claim_image_locked(cla, layer->sc_array[0], data->equirect2.sub.image_index);
		break;
	case XRT_LAYER_PASSTHROUGH: break;
	}
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
	return xret;
}

static bool
submit_add_image_locked(struct gpu_reuse_submit *submit, struct xrt_swapchain *xsc, uint32_t image_index)
{
	if (xsc == NULL || image_index >= xsc->image_count) {
		return true;
	}

	struct gpu_reuse_tracker *tracker = find_tracker_in_context_locked(submit->context, xsc);
	if (tracker == NULL) {
		return true;
	}

	for (uint32_t i = 0; i < submit->image_count; i++) {
		if (submit->images[i].tracker == tracker && submit->images[i].image_index == image_index) {
			return true;
		}
	}

	if (submit->image_count >= ARRAY_SIZE(submit->images)) {
		U_LOG_E("Too many tracked swapchain images in one compositor submit");
		return false;
	}

	if (submit->value == 0) {
		if (submit->context->next_value == UINT64_MAX) {
			U_LOG_E("Swapchain GPU reuse timeline value exhausted");
			return false;
		}
		submit->value = ++submit->context->next_value;
	}

	struct gpu_reuse_submit_image *image = &submit->images[submit->image_count++];
	image->tracker = tracker;
	image->image_index = image_index;
	image->previous_value = tracker->last_gpu_use[image_index];
	tracker->last_gpu_use[image_index] = submit->value;

	GPU_REUSE_LOG("Swapchain GPU reuse submit assignment: swapchain=%p image=%u timeline=%llu",
	              (void *)xsc,
	              image_index,
	              (unsigned long long)submit->value);
	return true;
}

static bool
submit_add_layer_locked(struct gpu_reuse_submit *submit, const struct comp_layer *layer)
{
	const struct xrt_layer_data *data = &layer->data;
	switch (data->type) {
	case XRT_LAYER_PROJECTION:
		for (uint32_t i = 0; i < data->view_count; i++) {
			if (!submit_add_image_locked(submit, layer->sc_array[i], data->proj.v[i].sub.image_index)) {
				return false;
			}
		}
		break;
	case XRT_LAYER_PROJECTION_DEPTH:
		for (uint32_t i = 0; i < data->view_count; i++) {
			if (!submit_add_image_locked(submit, layer->sc_array[i], data->depth.v[i].sub.image_index) ||
			    !submit_add_image_locked(
			        submit, layer->sc_array[i + data->view_count], data->depth.d[i].sub.image_index)) {
				return false;
			}
		}
		break;
	case XRT_LAYER_QUAD:
		return submit_add_image_locked(submit, layer->sc_array[0], data->quad.sub.image_index);
	case XRT_LAYER_CUBE:
		return submit_add_image_locked(submit, layer->sc_array[0], data->cube.sub.image_index);
	case XRT_LAYER_CYLINDER:
		return submit_add_image_locked(submit, layer->sc_array[0], data->cylinder.sub.image_index);
	case XRT_LAYER_EQUIRECT1:
		return submit_add_image_locked(submit, layer->sc_array[0], data->equirect1.sub.image_index);
	case XRT_LAYER_EQUIRECT2:
		return submit_add_image_locked(submit, layer->sc_array[0], data->equirect2.sub.image_index);
	case XRT_LAYER_PASSTHROUGH: break;
	}
	return true;
}

static void
submit_rollback_locked(struct gpu_reuse_submit *submit)
{
	for (uint32_t i = 0; i < submit->image_count; i++) {
		struct gpu_reuse_submit_image *image = &submit->images[i];
		if (image->tracker->last_gpu_use[image->image_index] == submit->value) {
			image->tracker->last_gpu_use[image->image_index] = image->previous_value;
		}
	}
}

VkResult
comp_swapchain_gpu_reuse_vk_cmd_submit_locked(struct comp_layer_accum *cla,
                                              struct vk_bundle *vk,
                                              struct vk_bundle_queue *queue,
                                              uint32_t count,
                                              const VkSubmitInfo *infos,
                                              VkFence fence)
{
	if (cla == NULL || vk == NULL || queue == NULL || infos == NULL || count != 1) {
		VkResult ret = vk_cmd_submit_locked(vk, queue, count, infos, fence);
		comp_swapchain_gpu_reuse_native_accum_release(cla);
		return ret;
	}

#ifndef VK_KHR_timeline_semaphore
	VkResult ret = vk_cmd_submit_locked(vk, queue, count, infos, fence);
	comp_swapchain_gpu_reuse_native_accum_release(cla);
	return ret;
#else
	struct gpu_reuse_submit submit = {0};

	pthread_mutex_lock(&g_gpu_reuse_mutex);
	submit.context = find_context_locked(vk);
	if (submit.context == NULL) {
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		VkResult ret = vk_cmd_submit_locked(vk, queue, count, infos, fence);
		comp_swapchain_gpu_reuse_native_accum_release(cla);
		return ret;
	}

	bool collected = true;
	for (uint32_t i = 0; i < cla->layer_count; i++) {
		if (!submit_add_layer_locked(&submit, &cla->layers[i])) {
			collected = false;
			break;
		}
	}

	if (!collected) {
		submit_rollback_locked(&submit);
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		comp_swapchain_gpu_reuse_native_accum_release(cla);
		return VK_ERROR_TOO_MANY_OBJECTS;
	}

	if (submit.image_count == 0) {
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		VkResult ret = vk_cmd_submit_locked(vk, queue, count, infos, fence);
		comp_swapchain_gpu_reuse_native_accum_release(cla);
		return ret;
	}

	const VkSubmitInfo *original = &infos[0];
	if (original->waitSemaphoreCount > VK_SEMAPHORE_LIST_MAX_COUNT ||
	    original->signalSemaphoreCount >= VK_SEMAPHORE_LIST_MAX_COUNT + 1) {
		U_LOG_E("Renderer semaphore list too large for swapchain GPU reuse timeline");
		submit_rollback_locked(&submit);
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		comp_swapchain_gpu_reuse_native_accum_release(cla);
		return VK_ERROR_TOO_MANY_OBJECTS;
	}

	const VkTimelineSemaphoreSubmitInfoKHR *original_timeline = NULL;
	if (original->pNext != NULL) {
		const VkBaseInStructure *base = (const VkBaseInStructure *)original->pNext;
		if (base->sType != VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR || base->pNext != NULL) {
			U_LOG_E("Unexpected renderer VkSubmitInfo pNext chain while adding swapchain GPU reuse timeline");
			submit_rollback_locked(&submit);
			pthread_mutex_unlock(&g_gpu_reuse_mutex);
			comp_swapchain_gpu_reuse_native_accum_release(cla);
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		original_timeline = (const VkTimelineSemaphoreSubmitInfoKHR *)base;
	}

	VkSemaphore signal_semaphores[VK_SEMAPHORE_LIST_MAX_COUNT + 1] = {0};
	uint64_t signal_values[VK_SEMAPHORE_LIST_MAX_COUNT + 1] = {0};
	uint64_t wait_values[VK_SEMAPHORE_LIST_MAX_COUNT] = {0};

	for (uint32_t i = 0; i < original->waitSemaphoreCount; i++) {
		if (original_timeline != NULL && i < original_timeline->waitSemaphoreValueCount) {
			wait_values[i] = original_timeline->pWaitSemaphoreValues[i];
		}
	}
	for (uint32_t i = 0; i < original->signalSemaphoreCount; i++) {
		signal_semaphores[i] = original->pSignalSemaphores[i];
		if (original_timeline != NULL && i < original_timeline->signalSemaphoreValueCount) {
			signal_values[i] = original_timeline->pSignalSemaphoreValues[i];
		}
	}

	uint32_t signal_count = original->signalSemaphoreCount;
	signal_semaphores[signal_count] = submit.context->timeline;
	signal_values[signal_count] = submit.value;
	signal_count++;

	VkTimelineSemaphoreSubmitInfoKHR timeline_info = {
	    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR,
	    .pNext = NULL,
	    .waitSemaphoreValueCount = original->waitSemaphoreCount,
	    .pWaitSemaphoreValues = wait_values,
	    .signalSemaphoreValueCount = signal_count,
	    .pSignalSemaphoreValues = signal_values,
	};
	VkSubmitInfo submit_info = *original;
	submit_info.pNext = &timeline_info;
	submit_info.signalSemaphoreCount = signal_count;
	submit_info.pSignalSemaphores = signal_semaphores;

	/*
	 * Keep publication serialized through the queue submit. wait_image() cannot
	 * observe the new last_gpu_use value until the submission that will signal it
	 * has actually been accepted by Vulkan.
	 */
	VkResult ret = vk_cmd_submit_locked(vk, queue, 1, &submit_info, fence);
	if (ret == VK_SUCCESS) {
		submit.context->last_submitted_value = submit.value;
	} else {
		submit_rollback_locked(&submit);
	}
	pthread_mutex_unlock(&g_gpu_reuse_mutex);

	/* Native bridge claim is no longer needed once publication+submit is complete. */
	comp_swapchain_gpu_reuse_native_accum_release(cla);
	return ret;
#endif
}

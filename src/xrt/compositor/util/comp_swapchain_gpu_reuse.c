// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Fine-grained compositor GPU completion tracking for swapchain image reuse.
 * @ingroup comp_util
 */

#include "util/comp_swapchain_gpu_reuse.h"
#include "util/comp_swapchain_gpu_reuse_internal.h"
#include "util/comp_swapchain.h"
#include "util/comp_semaphore.h"
#include "util/u_debug.h"
#include "util/u_gpu_trace.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include "os/os_time.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

DEBUG_GET_ONCE_BOOL_OPTION(log_swapchain_gpu_reuse, "XRT_COMPOSITOR_LOG_SWAPCHAIN_GPU_REUSE", false)

#define GPU_REUSE_MAX_IMAGES_PER_SUBMIT (XRT_MAX_LAYERS * XRT_MAX_VIEWS * 2)

struct gpu_reuse_context;
struct comp_gpu_client_budget
{
	uint64_t trace_id;
	uint64_t bytes;
	uint32_t objects;
	uint32_t references;
};
static __thread struct {
	struct comp_gpu_client_budget *budget;
	uint64_t bytes;
} g_reservation;
// Zero selects a share of the device-local heap, see global_byte_limit().
DEBUG_GET_ONCE_NUM_OPTION(gpu_global_mb, "XRT_MACOS_GPU_GLOBAL_MB", 0)
DEBUG_GET_ONCE_NUM_OPTION(gpu_client_mb, "XRT_MACOS_GPU_CLIENT_MB", 0)
DEBUG_GET_ONCE_NUM_OPTION(gpu_client_objects, "XRT_MACOS_GPU_CLIENT_OBJECTS", 64)
DEBUG_GET_ONCE_NUM_OPTION(gpu_global_objects, "XRT_MACOS_GPU_GLOBAL_OBJECTS", 256)


struct gpu_reuse_tracker
{
	struct xrt_swapchain *xsc;
	struct comp_swapchain *sc;
	struct gpu_reuse_context *context;
	uint64_t last_gpu_use[XRT_MAX_SWAPCHAIN_IMAGES];
	struct xrt_compositor_semaphore *producer[XRT_MAX_SWAPCHAIN_IMAGES];
	uint64_t producer_value[XRT_MAX_SWAPCHAIN_IMAGES];
	uint64_t bytes;
	struct comp_gpu_client_budget *budget;
	uint64_t id;
	bool quarantined;
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

struct gpu_reuse_renderer_tls
{
	struct comp_layer_accum *cla;
	struct gpu_reuse_submit submit;
	struct vk_semaphore_list_signal signal_sems;
	struct vk_semaphore_list_wait wait_sems;
	const VkSubmitInfo *expected_submit_info;
	const VkSubmitInfo *rejected_submit_info;
	VkResult rejected_result;
	bool submit_active;
};

static pthread_mutex_t g_gpu_reuse_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gpu_reuse_cond = PTHREAD_COND_INITIALIZER;
static struct gpu_reuse_context *g_gpu_reuse_contexts = NULL;
static struct gpu_reuse_native_accum *g_gpu_reuse_native_accums = NULL;
/*
 * Limit all live plus quarantined tracked storage, and small-object growth.
 * Byte limits default to a share of the device-local heap once a device has
 * been seen, and to these fallbacks before that.
 */
#define GPU_REUSE_FALLBACK_GLOBAL_BYTES (UINT64_C(1024) * 1024 * 1024)
#define GPU_REUSE_FALLBACK_CLIENT_BYTES (UINT64_C(512) * 1024 * 1024)
//! Bound for size arithmetic and environment overrides (1 TiB).
#define GPU_REUSE_MAX_BYTES (UINT64_C(1) << 40)
#define GPU_REUSE_MAX_TRACKERS 256
static uint64_t g_device_local_heap_bytes;
static uint64_t g_tracked_bytes;
static uint32_t g_tracker_count;
static uint64_t g_tracker_serial;
static __thread struct gpu_reuse_renderer_tls g_renderer = {0};

static bool tracker_completed_locked(struct gpu_reuse_tracker *tracker);
static void reclaim_tracker_locked(struct gpu_reuse_tracker **ptr);

static uint32_t
budget_objects(long value, uint32_t fallback)
{
	return value > 0 && value <= GPU_REUSE_MAX_TRACKERS ? (uint32_t)value : fallback;
}

struct comp_gpu_client_budget *
comp_gpu_client_budget_create(void)
{
	struct comp_gpu_client_budget *budget = U_TYPED_CALLOC(struct comp_gpu_client_budget);
	if (budget != NULL) {
		budget->references = 1;
		budget->trace_id = u_gpu_trace_new_scope();
	}
	return budget;
}

uint64_t
comp_gpu_client_budget_trace_id(struct comp_gpu_client_budget *budget)
{
	return budget != NULL ? budget->trace_id : 0;
}

static void
budget_unref_locked(struct comp_gpu_client_budget *budget)
{
	if (budget != NULL && --budget->references == 0) free(budget);
}

void
comp_gpu_client_budget_destroy(struct comp_gpu_client_budget **ptr)
{
	pthread_mutex_lock(&g_gpu_reuse_mutex);
	budget_unref_locked(*ptr);
	*ptr = NULL;
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
}

static uint64_t
mb_option_bytes(long mb)
{
	return mb > 0 && (uint64_t)mb <= GPU_REUSE_MAX_BYTES / (1024 * 1024) ? (uint64_t)mb * 1024 * 1024 : 0;
}

//! Caller holds the registry lock.
static uint64_t
global_byte_limit(void)
{
	uint64_t bytes = mb_option_bytes(debug_get_num_option_gpu_global_mb());
	if (bytes != 0) {
		return bytes;
	}
	return g_device_local_heap_bytes != 0 ? g_device_local_heap_bytes / 4 * 3 : GPU_REUSE_FALLBACK_GLOBAL_BYTES;
}

//! Caller holds the registry lock.
static uint64_t
client_byte_limit(void)
{
	uint64_t bytes = mb_option_bytes(debug_get_num_option_gpu_client_mb());
	if (bytes == 0) {
		bytes = g_device_local_heap_bytes != 0 ? g_device_local_heap_bytes / 2 : GPU_REUSE_FALLBACK_CLIENT_BYTES;
	}
	uint64_t global = global_byte_limit();
	return bytes < global ? bytes : global;
}

static void
note_device_locked(struct vk_bundle *vk)
{
	uint64_t largest = 0;
	const VkPhysicalDeviceMemoryProperties *props = &vk->device_memory_props;
	for (uint32_t i = 0; i < props->memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++) {
		if ((props->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 &&
		    props->memoryHeaps[i].size > largest) {
			largest = props->memoryHeaps[i].size;
		}
	}
	// Track the latest device; one without a device-local heap means unknown.
	if (largest == g_device_local_heap_bytes) {
		return;
	}
	g_device_local_heap_bytes = largest;
	if (largest != 0) {
		U_LOG_I("GPU reuse budget: device-local heap %llu MiB, global limit %llu MiB, per-client %llu MiB",
		        (unsigned long long)(largest >> 20), (unsigned long long)(global_byte_limit() >> 20),
		        (unsigned long long)(client_byte_limit() >> 20));
	}
}

void
comp_swapchain_gpu_reuse_note_device(struct vk_bundle *vk)
{
	pthread_mutex_lock(&g_gpu_reuse_mutex);
	note_device_locked(vk);
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
}

/*!
 * Bytes per block, with 1x1 blocks for uncompressed formats.
 * Unknown formats use 16 bytes per texel, an upper bound for every
 * uncompressed format an application can request.
 */
static uint64_t
format_bytes_per_block(VkFormat format, uint32_t *block_extent)
{
	*block_extent = 1;
	switch (format) {
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8_SRGB:
	case VK_FORMAT_S8_UINT: return 1;
	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R8G8_SRGB:
	case VK_FORMAT_R16_UNORM:
	case VK_FORMAT_R16_SFLOAT:
	case VK_FORMAT_R5G6B5_UNORM_PACK16:
	case VK_FORMAT_B5G6R5_UNORM_PACK16:
	case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
	case VK_FORMAT_D16_UNORM: return 2;
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
	case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
	case VK_FORMAT_R16G16_UNORM:
	case VK_FORMAT_R16G16_SFLOAT:
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT: return 4;
	case VK_FORMAT_R16G16B16A16_UNORM:
	case VK_FORMAT_R16G16B16A16_SNORM:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT: return 8; // Metal pads Depth32Float_Stencil8 to 8 bytes.
	case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
	case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
	case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
	case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
	case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: *block_extent = 4; return 8;
	case VK_FORMAT_BC3_UNORM_BLOCK:
	case VK_FORMAT_BC3_SRGB_BLOCK:
	case VK_FORMAT_BC7_UNORM_BLOCK:
	case VK_FORMAT_BC7_SRGB_BLOCK: *block_extent = 4; return 16;
	default: return 16;
	}
}

static bool
budget_fits_locked(struct comp_gpu_client_budget *budget, uint64_t bytes)
{
	uint64_t global = global_byte_limit();
	return g_tracker_count < budget_objects(debug_get_num_option_gpu_global_objects(), 256) &&
	       g_tracked_bytes <= global && bytes <= global - g_tracked_bytes &&
	       (budget == NULL || (budget->objects < budget_objects(debug_get_num_option_gpu_client_objects(), 64) &&
	                           budget->bytes <= client_byte_limit() && bytes <= client_byte_limit() - budget->bytes));
}

xrt_result_t
comp_gpu_client_budget_reserve(struct comp_gpu_client_budget *budget,
                               const struct xrt_swapchain_create_info *info,
                               uint32_t image_count,
                               const struct xrt_image_native *images)
{
	if (budget == NULL || g_reservation.budget != NULL || image_count == 0 ||
	    image_count > XRT_MAX_SWAPCHAIN_IMAGES) return XRT_ERROR_INVALID_ARGUMENT;
	if (info->width == 0 || info->height == 0 || info->mip_count == 0)
		return XRT_ERROR_ALLOCATION;
	uint32_t max_mips = 0;
	for (uint32_t extent = info->width > info->height ? info->width : info->height; extent != 0; extent >>= 1) {
		max_mips++;
	}
	if (info->mip_count > max_mips)
		return XRT_ERROR_ALLOCATION;

	uint32_t block_extent;
	uint64_t block_bytes = format_bytes_per_block((VkFormat)info->format, &block_extent);
	uint64_t bytes = 0;
	uint32_t width = info->width;
	uint32_t height = info->height;
	for (uint32_t mip = 0; mip < info->mip_count; mip++) {
		// Round each dimension independently, including the sub-block mip tail.
		uint64_t blocks_wide = ((uint64_t)width + block_extent - 1) / block_extent;
		uint64_t blocks_high = ((uint64_t)height + block_extent - 1) / block_extent;
		if (blocks_wide > GPU_REUSE_MAX_BYTES / block_bytes)
			return XRT_ERROR_ALLOCATION;
		uint64_t level_bytes = blocks_wide * block_bytes;
		if (blocks_high > GPU_REUSE_MAX_BYTES / level_bytes)
			return XRT_ERROR_ALLOCATION;
		level_bytes *= blocks_high;
		if (level_bytes > GPU_REUSE_MAX_BYTES - bytes)
			return XRT_ERROR_ALLOCATION;
		bytes += level_bytes;
		width = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
	}
	uint64_t factors[] = {info->array_size, info->face_count, info->sample_count, image_count};
	for (uint32_t i = 0; i < ARRAY_SIZE(factors); i++) {
		if (factors[i] == 0 || factors[i] > GPU_REUSE_MAX_BYTES / bytes) return XRT_ERROR_ALLOCATION;
		bytes *= factors[i];
	}
	// Cover row/page alignment. Supplied transport sizes are an additional lower bound.
	uint64_t overhead = (uint64_t)image_count * 65536;
	if (bytes > GPU_REUSE_MAX_BYTES - overhead) return XRT_ERROR_ALLOCATION;
	bytes += overhead;
	if (images != NULL) {
		uint64_t supplied = 0;
		for (uint32_t i = 0; i < image_count; i++) {
			if (images[i].size > GPU_REUSE_MAX_BYTES - supplied) return XRT_ERROR_ALLOCATION;
			supplied += images[i].size;
		}
		if (supplied > bytes) bytes = supplied;
	}
	pthread_mutex_lock(&g_gpu_reuse_mutex);
	// Poll before admission too: the last client may have stopped the render loop.
	for (struct gpu_reuse_context *c = g_gpu_reuse_contexts; c != NULL; c = c->next) {
		struct gpu_reuse_tracker **ptr = &c->trackers;
		while (*ptr != NULL) {
			if ((*ptr)->quarantined && tracker_completed_locked(*ptr)) reclaim_tracker_locked(ptr);
			else ptr = &(*ptr)->next;
		}
	}
	if (!budget_fits_locked(budget, bytes)) {
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		U_LOG_W("GPU allocation rejected before import: bytes=%llu", (unsigned long long)bytes);
		return XRT_ERROR_ALLOCATION;
	}
	g_tracked_bytes += bytes; g_tracker_count++;
	budget->bytes += bytes; budget->objects++; budget->references++;
	g_reservation.budget = budget; g_reservation.bytes = bytes;
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
	return XRT_SUCCESS;
}

void
comp_gpu_client_budget_finish(void)
{
	pthread_mutex_lock(&g_gpu_reuse_mutex);
	if (g_reservation.budget != NULL) {
		struct comp_gpu_client_budget *budget = g_reservation.budget;
		g_tracked_bytes -= g_reservation.bytes; g_tracker_count--;
		budget->bytes -= g_reservation.bytes; budget->objects--;
		budget_unref_locked(budget);
		g_reservation.budget = NULL; g_reservation.bytes = 0;
	}
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
}

static void
reuse_log(const char *what, struct xrt_swapchain *xsc, uint32_t image_index, uint64_t value, int64_t extra_ns)
{
	if (!debug_get_bool_option_log_swapchain_gpu_reuse()) {
		return;
	}

	U_LOG_I("Swapchain GPU reuse %s: swapchain=%p image=%u timeline=%llu extra_ns=%lld", what, (void *)xsc,
	        image_index, (unsigned long long)value, (long long)extra_ns);
}

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
		if (ret != 0 && ret != ETIMEDOUT) {
			U_LOG_E("Swapchain GPU reuse condition wait failed: %d", ret);
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

	/*
	 * The caller's timeout is honoured, including an infinite one. A service
	 * IPC worker bounds its own calls (see ipc_handle_swapchain_wait_image), so
	 * a client whose GPU work never completes cannot pin that thread. A timeout
	 * preserves ownership; it never authorizes consumer completion.
	 */
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

		/* Consumer cancellation does not imply producer completion: an
		 * unresolved producer obligation must never authorize reuse. Wait for
		 * it within the remaining timeout, outside the registry lock, then
		 * re-evaluate from the top since state may have changed meanwhile. */
		if (tracker->producer[image_index] != NULL &&
		    xrt_compositor_semaphore_wait(tracker->producer[image_index], tracker->producer_value[image_index],
		                                  0) != XRT_SUCCESS) {
			struct xrt_compositor_semaphore *producer = NULL;
			xrt_compositor_semaphore_reference(&producer, tracker->producer[image_index]);
			uint64_t producer_value = tracker->producer_value[image_index];
			pthread_mutex_unlock(&g_gpu_reuse_mutex);

			xrt_result_t producer_result = xrt_compositor_semaphore_wait(
			    producer, producer_value, remaining_timeout_ns(timeout_ns, start_ns));
			xrt_compositor_semaphore_reference(&producer, NULL);
			if (producer_result != XRT_SUCCESS) {
				return producer_result;
			}
			continue;
		}
		uint64_t trace_id = tracker->id;
		uint64_t trace_owner_id = tracker->budget != NULL ? tracker->budget->trace_id : 0;
		uint64_t value = tracker->last_gpu_use[image_index];
		VkSemaphore timeline = tracker->context->timeline;
		struct vk_bundle *vk = tracker->context->vk;
		pthread_mutex_unlock(&g_gpu_reuse_mutex);

		if (value == 0) {
			return XRT_SUCCESS;
		}

		remaining_ns = remaining_timeout_ns(timeout_ns, start_ns);
		int64_t wait_start_ns = os_monotonic_get_ns();
		reuse_log("wait", xsc, image_index, value, remaining_ns);

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
			VK_ERROR(vk, "Swapchain GPU reuse vkWaitSemaphores image %u value %llu: %s", image_index,
			         (unsigned long long)value, vk_result_string(ret));
			return XRT_ERROR_VULKAN;
		}

		if (u_gpu_trace_enabled()) {
			int64_t completed_ns = os_monotonic_get_ns();
			u_gpu_trace_owner_event("consumer_gpu_observed", trace_id, trace_owner_id, -1, image_index,
			                        value, completed_ns, completed_ns - wait_start_ns);
		}
		reuse_log("wait complete", xsc, image_index, value, os_monotonic_get_ns() - wait_start_ns);

		pthread_mutex_lock(&g_gpu_reuse_mutex);
		tracker = find_tracker_locked(xsc);
		if (tracker == NULL) {
			pthread_mutex_unlock(&g_gpu_reuse_mutex);
			return XRT_SUCCESS;
		}
		bool stable = tracker->pending_consumers[image_index] == 0 &&
		              tracker->last_gpu_use[image_index] == value &&
		              (tracker->producer[image_index] == NULL ||
		               xrt_compositor_semaphore_wait(tracker->producer[image_index],
		                                             tracker->producer_value[image_index], 0) == XRT_SUCCESS);
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		if (stable) {
			return XRT_SUCCESS;
		}
	}
}

static bool
tracker_completed_locked(struct gpu_reuse_tracker *tracker)
{
	struct vk_bundle *vk = tracker->context->vk;
	for (uint32_t i = 0; i < tracker->xsc->image_count; i++) {
		if (tracker->pending_consumers[i] != 0) {
			return false;
		}
		if (tracker->producer[i] != NULL &&
		    xrt_compositor_semaphore_wait(tracker->producer[i], tracker->producer_value[i], 0) != XRT_SUCCESS) {
			return false;
		}
		uint64_t value = tracker->last_gpu_use[i];
		if (value != 0) {
			VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
			                            .semaphoreCount = 1,
			                            .pSemaphores = &tracker->context->timeline,
			                            .pValues = &value};
			if (vk->vkWaitSemaphores(vk->device, &wait, 0) != VK_SUCCESS) {
				return false;
			}
		}
	}
	return true;
}

/* Caller holds the registry lock. original_destroy only enqueues the swapchain
 * on its existing compositor garbage collector; it does not free GPU storage. */
static void
reclaim_tracker_locked(struct gpu_reuse_tracker **ptr)
{
	struct gpu_reuse_tracker *t = *ptr;
	uint64_t owner_trace_id = t->budget != NULL ? t->budget->trace_id : 0;
	*ptr = t->next;
	for (uint32_t i = 0; i < t->xsc->image_count; i++) {
		xrt_compositor_semaphore_reference(&t->producer[i], NULL);
	}
	g_tracked_bytes -= t->bytes;
	g_tracker_count--;
	if (t->budget != NULL) {
		t->budget->bytes -= t->bytes; t->budget->objects--;
		budget_unref_locked(t->budget);
	}
	t->sc->gpu_reuse_safe_destroy = true;
	t->xsc->wait_image = t->original_wait_image;
	t->xsc->destroy = t->original_destroy;
	U_LOG_I("GPU reuse reclaim: swapchain=%p bytes=%llu retained_bytes=%llu objects=%u id=%llu", (void *)t->xsc,
	        (unsigned long long)t->bytes, (unsigned long long)g_tracked_bytes, g_tracker_count, (unsigned long long)t->id);
	if (u_gpu_trace_enabled())
		u_gpu_trace_owner_event("storage_reclaimed", t->id, owner_trace_id, -1, UINT32_MAX, 0,
		                        os_monotonic_get_ns(), 0);
	t->original_destroy(t->xsc);
	free(t);
}

bool
comp_swapchain_gpu_reuse_device_in_use(struct vk_bundle *vk)
{
	pthread_mutex_lock(&g_gpu_reuse_mutex);
	struct gpu_reuse_context *context = find_context_locked(vk);
	bool in_use = context != NULL && context->trackers != NULL;
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
	return in_use;
}

void
comp_swapchain_gpu_reuse_collect(struct vk_bundle *vk, bool device_teardown)
{
	pthread_mutex_lock(&g_gpu_reuse_mutex);
	struct gpu_reuse_context **cp = &g_gpu_reuse_contexts;
	while (*cp != NULL) {
		struct gpu_reuse_context *c = *cp;
		if (c->vk != vk) {
			cp = &c->next;
			continue;
		}
		struct gpu_reuse_tracker **tp = &c->trackers;
		while (*tp != NULL) {
			if ((*tp)->quarantined && tracker_completed_locked(*tp)) {
				reclaim_tracker_locked(tp);
			} else {
				tp = &(*tp)->next;
			}
		}
		if (c->trackers == NULL) {
			vk->vkDestroySemaphore(vk->device, c->timeline, NULL);
			*cp = c->next;
			free(c);
		} else if (device_teardown) {
			/* Producer access may outlive the Vulkan device. Keep native storage
			 * until process exit, detach so a reused vk_bundle address is safe.
			 * Budget remains charged. Never access this device again. */
			U_LOG_W("GPU reuse device teardown retains unresolved resources until process exit");
			*cp = c->next;
			c->vk = NULL;
		} else {
			cp = &c->next;
		}
	}
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
}

static void
tracked_swapchain_destroy(struct xrt_swapchain *xsc)
{
	pthread_mutex_lock(&g_gpu_reuse_mutex);
	struct gpu_reuse_tracker *tracker = find_tracker_locked(xsc);
	if (tracker != NULL) {
		tracker->quarantined = true;
		if (u_gpu_trace_enabled())
			u_gpu_trace_owner_event("storage_quarantined", tracker->id,
			                        tracker->budget != NULL ? tracker->budget->trace_id : 0, -1, UINT32_MAX,
			                        0, os_monotonic_get_ns(), 0);
		U_LOG_I("GPU reuse quarantine: swapchain=%p bytes=%llu retained_bytes=%llu objects=%u id=%llu", (void *)xsc,
		        (unsigned long long)tracker->bytes, (unsigned long long)g_tracked_bytes, g_tracker_count, (unsigned long long)tracker->id);
	}
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
	/* Existing compositor GC polls it with timeout zero on subsequent frames. */
}

xrt_result_t
comp_swapchain_gpu_reuse_set_producer(struct xrt_swapchain *xsc,
                                      uint32_t index,
                                      struct xrt_compositor_semaphore *sem,
                                      uint64_t value)
{
	pthread_mutex_lock(&g_gpu_reuse_mutex);
	struct gpu_reuse_tracker *t = find_tracker_locked(xsc);
	if (t != NULL && index < xsc->image_count) {
		if (!comp_semaphore_matches_vk(sem, t->context->vk) || t->quarantined) {
			pthread_mutex_unlock(&g_gpu_reuse_mutex);
			return XRT_ERROR_VULKAN;
		}
		/* Replacing a dependency must not destroy a semaphore referenced by
		 * an earlier compositor submission. Normal Metal clients keep one
		 * event per session. Reject a changed payload until completion. */
		if (t->producer[index] != NULL && t->producer[index] != sem && !tracker_completed_locked(t)) {
			pthread_mutex_unlock(&g_gpu_reuse_mutex);
			return XRT_ERROR_VULKAN;
		}
		xrt_compositor_semaphore_reference(&t->producer[index], sem);
		t->producer_value[index] = value;
		if (u_gpu_trace_enabled())
			u_gpu_trace_owner_event("producer_obligation", t->id,
			                        t->budget != NULL ? t->budget->trace_id : 0, -1, index, value,
			                        os_monotonic_get_ns(), 0);
	}
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
	return XRT_SUCCESS;
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

	uint64_t bytes = 0;
	for (uint32_t i = 0; i < sc->vkic.image_count; i++) {
		VkMemoryRequirements requirements = {0};
		sc->vk->vkGetImageMemoryRequirements(sc->vk->device, sc->vkic.images[i].handle, &requirements);
		if (requirements.size > GPU_REUSE_MAX_BYTES - bytes) {
			return XRT_ERROR_ALLOCATION;
		}
		bytes += requirements.size;
	}
	pthread_mutex_lock(&g_gpu_reuse_mutex);
	if (find_tracker_locked(xsc) != NULL) {
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		return XRT_SUCCESS;
	}
	note_device_locked(sc->vk);

	struct comp_gpu_client_budget *budget = g_reservation.budget;
	uint64_t reserved = g_reservation.bytes;
	uint64_t global = global_byte_limit();
	uint64_t client = client_byte_limit();
	bool fits = budget != NULL ? (bytes <= reserved ||
	                              (g_tracked_bytes <= global && bytes - reserved <= global - g_tracked_bytes &&
	                               budget->bytes <= client && bytes - reserved <= client - budget->bytes))
	                           : budget_fits_locked(NULL, bytes);
	if (!fits) {
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		U_LOG_E("GPU reuse allocation budget exhausted (live plus quarantined storage)");
		return XRT_ERROR_ALLOCATION;
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

	tracker->id = ++g_tracker_serial;
	tracker->bytes = bytes;
	if (budget != NULL) {
		g_tracked_bytes = g_tracked_bytes - reserved + bytes;
		budget->bytes = budget->bytes - reserved + bytes;
		tracker->budget = budget;
		g_reservation.budget = NULL; g_reservation.bytes = 0;
	} else {
		g_tracked_bytes += bytes; g_tracker_count++;
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

	U_LOG_I("Swapchain GPU reuse tracking enabled: swapchain=%p images=%u timeline=%p", (void *)xsc,
	        xsc->image_count, (void *)context->timeline);
	return XRT_SUCCESS;
}

xrt_result_t
comp_gpu_client_budget_adopt(struct xrt_swapchain *xsc)
{
	return g_reservation.budget == NULL ? XRT_SUCCESS : comp_swapchain_gpu_reuse_enable(xsc);
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
	if (u_gpu_trace_enabled())
		u_gpu_trace_owner_event("consumer_claimed", tracker->id,
		                        tracker->budget != NULL ? tracker->budget->trace_id : 0, -1, image_index,
		                        tracker->last_gpu_use[image_index], os_monotonic_get_ns(), 0);
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
			if (u_gpu_trace_enabled())
				u_gpu_trace_owner_event("consumer_claim_released", tracker->id,
				                        tracker->budget != NULL ? tracker->budget->trace_id : 0, -1,
				                        image_index, tracker->last_gpu_use[image_index],
				                        os_monotonic_get_ns(), 0);
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
			xret =
			    native_accum_claim_image_locked(cla, layer->sc_array[i], data->proj.v[i].sub.image_index);
		}
		break;
	case XRT_LAYER_PROJECTION_DEPTH:
		for (uint32_t i = 0; i < data->view_count && xret == XRT_SUCCESS; i++) {
			xret =
			    native_accum_claim_image_locked(cla, layer->sc_array[i], data->depth.v[i].sub.image_index);
			if (xret == XRT_SUCCESS) {
				xret = native_accum_claim_image_locked(cla, layer->sc_array[i + data->view_count],
				                                       data->depth.d[i].sub.image_index);
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

	struct gpu_reuse_tracker *tracker = find_tracker_locked(xsc);
	if (tracker == NULL) {
		return true;
	}
	if (submit->context == NULL) {
		submit->context = tracker->context;
	} else if (submit->context != tracker->context) {
		U_LOG_E("Tracked swapchains from different Vulkan devices reached one compositor submit");
		return false;
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
	if (u_gpu_trace_enabled())
		u_gpu_trace_owner_event("consumer_submit_assignment", tracker->id,
		                        tracker->budget != NULL ? tracker->budget->trace_id : 0, -1, image_index,
		                        submit->value, os_monotonic_get_ns(), 0);
	reuse_log("submit assignment", xsc, image_index, submit->value, 0);
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
			    !submit_add_image_locked(submit, layer->sc_array[i + data->view_count],
			                             data->depth.d[i].sub.image_index)) {
				return false;
			}
		}
		break;
	case XRT_LAYER_QUAD: return submit_add_image_locked(submit, layer->sc_array[0], data->quad.sub.image_index);
	case XRT_LAYER_CUBE: return submit_add_image_locked(submit, layer->sc_array[0], data->cube.sub.image_index);
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

static void
renderer_submit_abort_locked(void)
{
	if (g_renderer.submit_active) {
		submit_rollback_locked(&g_renderer.submit);
		g_renderer.submit_active = false;
		g_renderer.expected_submit_info = NULL;
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
	}
}

void
comp_swapchain_gpu_reuse_renderer_enter(struct comp_layer_accum *cla)
{
	/* A previous draw must have left no armed queue submit behind. */
	if (g_renderer.submit_active) {
		renderer_submit_abort_locked();
	}
	g_renderer.cla = cla;
	g_renderer.rejected_submit_info = NULL;
	g_renderer.rejected_result = VK_SUCCESS;
}

void
comp_swapchain_gpu_reuse_renderer_leave(struct comp_layer_accum *cla)
{
	if (g_renderer.submit_active) {
		renderer_submit_abort_locked();
	}
	g_renderer.expected_submit_info = NULL;
	g_renderer.rejected_submit_info = NULL;
	g_renderer.rejected_result = VK_SUCCESS;
	g_renderer.cla = NULL;

	/* Covers both successful rendering and all pre-submit/early-return paths. */
	comp_swapchain_gpu_reuse_native_accum_release(cla);
}

void
comp_swapchain_gpu_reuse_submit_info_builder_prepare(struct vk_submit_info_builder *builder,
                                                     const struct vk_semaphore_list_wait *wait_semaphores,
                                                     const VkCommandBuffer *command_buffers,
                                                     uint32_t command_buffer_count,
                                                     const struct vk_semaphore_list_signal *signal_semaphores,
                                                     const void *next)
{
	if (g_renderer.cla == NULL || g_renderer.submit_active) {
		vk_submit_info_builder_prepare(builder, wait_semaphores, command_buffers, command_buffer_count,
		                               signal_semaphores, next);
		return;
	}

	U_ZERO(&g_renderer.submit);
	pthread_mutex_lock(&g_gpu_reuse_mutex);

	bool collected = true;
	for (uint32_t i = 0; i < g_renderer.cla->layer_count; i++) {
		if (!submit_add_layer_locked(&g_renderer.submit, &g_renderer.cla->layers[i])) {
			collected = false;
			break;
		}
	}

	if (!collected) {
		submit_rollback_locked(&g_renderer.submit);
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		vk_submit_info_builder_prepare(builder, wait_semaphores, command_buffers, command_buffer_count,
		                               signal_semaphores, next);
		g_renderer.rejected_submit_info = &builder->submit_info;
		g_renderer.rejected_result = VK_ERROR_TOO_MANY_OBJECTS;
		return;
	}

	if (g_renderer.submit.image_count == 0) {
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		vk_submit_info_builder_prepare(builder, wait_semaphores, command_buffers, command_buffer_count,
		                               signal_semaphores, next);
		return;
	}

	U_ZERO(&g_renderer.signal_sems);
	if (signal_semaphores != NULL) {
		g_renderer.signal_sems = *signal_semaphores;
	}
	if (g_renderer.signal_sems.count >= VK_SEMAPHORE_LIST_MAX_COUNT) {
		U_LOG_E("No renderer signal-semaphore slot available for swapchain GPU reuse timeline");
		submit_rollback_locked(&g_renderer.submit);
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		vk_submit_info_builder_prepare(builder, wait_semaphores, command_buffers, command_buffer_count,
		                               signal_semaphores, next);
		g_renderer.rejected_submit_info = &builder->submit_info;
		g_renderer.rejected_result = VK_ERROR_TOO_MANY_OBJECTS;
		return;
	}

	U_ZERO(&g_renderer.wait_sems);
	if (wait_semaphores != NULL) {
		g_renderer.wait_sems = *wait_semaphores;
	}
	bool producer_ready = true;
	for (uint32_t i = 0; i < g_renderer.submit.image_count; i++) {
		struct gpu_reuse_submit_image *image = &g_renderer.submit.images[i];
		struct xrt_compositor_semaphore *producer = image->tracker->producer[image->image_index];
		if (producer == NULL) {
			continue;
		}
		uint64_t value = image->tracker->producer_value[image->image_index];
		/* Stage A: zero-time readiness verification; never wait on a future
		 * payload on the GPU. Stage B still carries exactly this value. */
		if (xrt_compositor_semaphore_wait(producer, value, 0) != XRT_SUCCESS) {
			producer_ready = false;
			break;
		}
		VkSemaphore semaphore = comp_semaphore(producer)->semaphore;
		uint32_t k = 0;
		for (; k < g_renderer.wait_sems.count; k++) {
			if (g_renderer.wait_sems.semaphores[k] == semaphore) {
				break;
			}
		}
		if (k < g_renderer.wait_sems.count) {
			if (g_renderer.wait_sems.values[k] < value) {
				g_renderer.wait_sems.values[k] = value;
			}
			g_renderer.wait_sems.stages[k] |= VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		} else if (k < VK_SEMAPHORE_LIST_MAX_COUNT) {
			vk_semaphore_list_wait_add_timeline(&g_renderer.wait_sems, semaphore, value,
			                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		} else {
			producer_ready = false;
			break;
		}
		reuse_log("producer ready wait", image->tracker->xsc, image->image_index, value, 0);
	}
	if (!producer_ready) {
		submit_rollback_locked(&g_renderer.submit);
		pthread_mutex_unlock(&g_gpu_reuse_mutex);
		vk_submit_info_builder_prepare(builder, wait_semaphores, command_buffers, command_buffer_count,
		                               signal_semaphores, next);
		g_renderer.rejected_submit_info = &builder->submit_info;
		g_renderer.rejected_result = VK_NOT_READY;
		return;
	}
	vk_semaphore_list_signal_add_timeline(&g_renderer.signal_sems, g_renderer.submit.context->timeline,
	                                      g_renderer.submit.value);
	vk_submit_info_builder_prepare(builder, &g_renderer.wait_sems, command_buffers, command_buffer_count,
	                               &g_renderer.signal_sems, next);

	/*
	 * Keep the publication mutex held until this exact VkSubmitInfo has been
	 * accepted by Vulkan, so wait_image() cannot observe last_gpu_use=V before
	 * the submission that will signal V exists on the queue.
	 */
	g_renderer.expected_submit_info = &builder->submit_info;
	g_renderer.submit_active = true;
}

VkResult
comp_swapchain_gpu_reuse_vk_cmd_submit_locked(
    struct vk_bundle *vk, struct vk_bundle_queue *queue, uint32_t count, const VkSubmitInfo *infos, VkFence fence)
{
	if (vk == NULL || queue == NULL || infos == NULL) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	if (g_renderer.rejected_submit_info != NULL && count == 1 && infos == g_renderer.rejected_submit_info) {
		VkResult ret = g_renderer.rejected_result;
		g_renderer.rejected_submit_info = NULL;
		g_renderer.rejected_result = VK_SUCCESS;
		return ret;
	}

	if (!g_renderer.submit_active || count != 1 || infos != g_renderer.expected_submit_info) {
		return vk_cmd_submit_locked(vk, queue, count, infos, fence);
	}

	VkResult ret = vk_cmd_submit_locked(vk, queue, count, infos, fence);
	if (ret == VK_SUCCESS) {
		g_renderer.submit.context->last_submitted_value = g_renderer.submit.value;
	} else {
		submit_rollback_locked(&g_renderer.submit);
	}

	g_renderer.submit_active = false;
	g_renderer.expected_submit_info = NULL;
	pthread_mutex_unlock(&g_gpu_reuse_mutex);
	return ret;
}

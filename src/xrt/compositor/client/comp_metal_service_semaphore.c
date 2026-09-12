// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Service-mode Metal shared-event semaphore pair creation.
 * @ingroup comp_client
 */

#include "client/comp_metal_service_semaphore.h"
#include "shared/ipc_metal_xpc.h"
#include "util/u_logging.h"

#include <pthread.h>

static pthread_mutex_t g_service_compositor_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct xrt_compositor *g_service_compositor = NULL;
static void *g_service_metal_device = NULL;

void
client_metal_service_semaphore_register_compositor(struct xrt_compositor *xc, void *metal_device)
{
	pthread_mutex_lock(&g_service_compositor_mutex);
	g_service_compositor = xc;
	g_service_metal_device = metal_device;
	pthread_mutex_unlock(&g_service_compositor_mutex);
}

xrt_result_t
client_metal_service_semaphore_create_pair(struct xrt_compositor_semaphore **out_xcsem,
                                           void **out_mtl_shared_event)
{
	if (out_xcsem == NULL || out_mtl_shared_event == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	*out_xcsem = NULL;
	*out_mtl_shared_event = NULL;

	pthread_mutex_lock(&g_service_compositor_mutex);
	struct xrt_compositor *xc = g_service_compositor;
	void *metal_device = g_service_metal_device;
	pthread_mutex_unlock(&g_service_compositor_mutex);
	if (xc == NULL || metal_device == NULL) {
		return XRT_ERROR_IPC_COMPOSITOR_NOT_CREATED;
	}

	ipc_metal_xpc_begin_shared_event_request(metal_device);

	xrt_graphics_sync_handle_t native_handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	struct xrt_compositor_semaphore *xcsem = NULL;
	xrt_result_t xret = xrt_comp_create_semaphore(xc, &native_handle, &xcsem);

	void *raw_event = NULL;
	bool got_event = ipc_metal_xpc_end_shared_event_request(&raw_event);
	if (xret != XRT_SUCCESS || xcsem == NULL || !got_event || raw_event == NULL) {
		if (xcsem != NULL) {
			xrt_compositor_semaphore_reference(&xcsem, NULL);
		}
		U_LOG_W("Metal service Stage 4 semaphore pair unavailable: create=%d event=%s",
		        xret,
		        got_event && raw_event != NULL ? "yes" : "no");
		return xret != XRT_SUCCESS ? xret : XRT_ERROR_IPC_FAILURE;
	}

	if (xrt_graphics_sync_handle_is_valid(native_handle)) {
		U_LOG_W("Metal service Stage 4 unexpectedly received a native semaphore handle; XPC shared-event path remains active");
	}

	*out_xcsem = xcsem;
	*out_mtl_shared_event = raw_event;
	return XRT_SUCCESS;
}

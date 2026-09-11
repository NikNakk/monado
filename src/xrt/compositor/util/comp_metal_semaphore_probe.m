// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS Metal shared-event semaphore diagnostic probe.
 * @ingroup comp_util
 */

#import <Metal/Metal.h>

#include "util/comp_metal_semaphore_probe.h"
#include "util/comp_semaphore.h"
#include "util/u_logging.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

void
comp_metal_semaphore_probe(struct vk_bundle *vk)
{
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	static bool attempted = false;

	pthread_mutex_lock(&mutex);
	if (attempted) {
		pthread_mutex_unlock(&mutex);
		return;
	}
	attempted = true;
	pthread_mutex_unlock(&mutex);

	struct xrt_compositor_semaphore *xcsem = NULL;
	void *raw_shared_event = NULL;
	xrt_result_t xret = comp_semaphore_create_metal_shared_event(vk, &xcsem, &raw_shared_event);
	if (xret != XRT_SUCCESS) {
		U_LOG_W("Metal timeline shared-event probe unavailable: result=%d; blocking app release handoff remains active",
		        xret);
		return;
	}

	@autoreleasepool {
		// vkExportMetalObjectsEXT does not transfer Objective-C ownership.
		// Retain the export while inspecting it, then release it before the
		// backing Vulkan semaphore is destroyed below.
		id<MTLSharedEvent> shared_event = [(__bridge id<MTLSharedEvent>)raw_shared_event retain];
		if (shared_event == nil) {
			U_LOG_W("Metal timeline shared-event probe returned a nil MTLSharedEvent after export");
		} else {
			U_LOG_I("Metal timeline shared-event probe succeeded: event=%p initial_value=%llu; blocking app release handoff unchanged",
			        (__bridge void *)shared_event,
			        (unsigned long long)shared_event.signaledValue);
			[shared_event release];
		}
	}

	xrt_compositor_semaphore_reference(&xcsem, NULL);
}

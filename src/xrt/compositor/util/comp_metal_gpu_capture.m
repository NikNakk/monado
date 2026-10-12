// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experiment: Metal GPU capture around the first client-frame submissions.
 * @ingroup comp_util
 */

#import <Metal/Metal.h>

#include "util/comp_metal_gpu_capture.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "vk/vk_helpers.h"

#include <pthread.h>
#include <stdbool.h>
#include <unistd.h>

DEBUG_GET_ONCE_OPTION(gpu_capture_dir, "XRT_MACOS_GPU_CAPTURE_DIR", NULL)

//! Producer-waiting submissions to capture; the startup hang is on the first.
#define CAPTURE_SUBMISSIONS 4

static pthread_mutex_t g_capture_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_capture_started = false;
static bool g_capture_done = false;
static uint32_t g_capture_submissions = 0;

static void
end_locked(const char *reason)
{
	if (!g_capture_started || g_capture_done) {
		g_capture_done = true;
		return;
	}
	g_capture_done = true;
	[[MTLCaptureManager sharedCaptureManager] stopCapture];
	U_LOG_W("Metal GPU capture stopped (%s) after %u producer-waiting submissions", reason, g_capture_submissions);
}

static void
start_locked(struct vk_bundle *vk, const char *dir)
{
	g_capture_started = true;
	VkExportMetalDeviceInfoEXT device_info = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
	VkExportMetalObjectsInfoEXT objects_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &device_info,
	};
	if (vk->vkExportMetalObjectsEXT == NULL) {
		U_LOG_E("Metal GPU capture: vkExportMetalObjectsEXT unavailable");
		g_capture_done = true;
		return;
	}
	vk->vkExportMetalObjectsEXT(vk->device, &objects_info);
	id<MTLDevice> device = (__bridge id<MTLDevice>)device_info.mtlDevice;

	@autoreleasepool {
		MTLCaptureManager *manager = [MTLCaptureManager sharedCaptureManager];
		if (device == nil || ![manager supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
			U_LOG_E("Metal GPU capture unavailable (device=%p); set MTL_CAPTURE_ENABLED=1",
			        (__bridge void *)device);
			g_capture_done = true;
			return;
		}
		NSString *path = [NSString stringWithFormat:@"%s/monado_%d.gputrace", dir, (int)getpid()];
		MTLCaptureDescriptor *descriptor = [[MTLCaptureDescriptor alloc] init];
		descriptor.captureObject = device;
		descriptor.destination = MTLCaptureDestinationGPUTraceDocument;
		descriptor.outputURL = [NSURL fileURLWithPath:path];
		NSError *error = nil;
		BOOL started = [manager startCaptureWithDescriptor:descriptor error:&error];
		[descriptor release];
		if (!started) {
			U_LOG_E("Metal GPU capture failed to start: %s", [[error localizedDescription] UTF8String]);
			g_capture_done = true;
			return;
		}
		U_LOG_W("Metal GPU capture started: %s", [path UTF8String]);
	}
}

void
comp_metal_gpu_capture_note_producer_submit(struct vk_bundle *vk)
{
	const char *dir = debug_get_option_gpu_capture_dir();
	if (dir == NULL || dir[0] == '\0' || vk == NULL) {
		return;
	}

	pthread_mutex_lock(&g_capture_mutex);
	if (!g_capture_done) {
		if (!g_capture_started) {
			start_locked(vk, dir);
		}
		if (g_capture_started && !g_capture_done && g_capture_submissions++ >= CAPTURE_SUBMISSIONS) {
			end_locked("submission limit");
		}
	}
	pthread_mutex_unlock(&g_capture_mutex);
}

void
comp_metal_gpu_capture_end(const char *reason)
{
	pthread_mutex_lock(&g_capture_mutex);
	end_locked(reason);
	pthread_mutex_unlock(&g_capture_mutex);
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experiment: Metal command-queue GPU priority for the macOS compositor.
 * @ingroup comp_util
 */

#import <Metal/Metal.h>
#import <objc/message.h>

#include "util/comp_metal_gpu_priority.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "vk/vk_helpers.h"

DEBUG_GET_ONCE_NUM_OPTION(macos_gpu_queue_priority, "XRT_MACOS_GPU_QUEUE_PRIORITY", -1)

/*
 * Private IOGPU selectors. Foreground priorities outside 0..2 abort the
 * process in a Metal assertion, so values are validated before any call.
 * A standalone two-process test on Apple M5 found 0 highest, 1 the default
 * and 2 near-starved under a saturating competitor.
 */
typedef long (*get_priority_fn)(id, SEL);
typedef void (*set_priority_fn)(id, SEL, long);

static bool
read_priority(id queue, const char *name, long *out)
{
	SEL sel = sel_registerName(name);
	if (queue == nil || ![queue respondsToSelector:sel]) {
		return false;
	}
	*out = ((get_priority_fn)objc_msgSend)(queue, sel);
	return true;
}

static bool
write_priority(id queue, const char *name, long value)
{
	SEL sel = sel_registerName(name);
	if (queue == nil || value < 0 || value > 2 || ![queue respondsToSelector:sel]) {
		return false;
	}
	((set_priority_fn)objc_msgSend)(queue, sel, value);
	return true;
}

static void
apply_to_queue(id queue, const char *label, long requested)
{
	long fg = -1, bg = -1;
	bool readable =
	    read_priority(queue, "getGPUPriority", &fg) & read_priority(queue, "getBackgroundGPUPriority", &bg);
	if (!readable) {
		U_LOG_W("Metal GPU priority unavailable on %s queue %p", label, (__bridge void *)queue);
		return;
	}
	if (requested < 0) {
		U_LOG_I("Metal GPU priority, %s queue: foreground=%ld background=%ld (unchanged)", label, fg, bg);
		return;
	}
	bool set_fg = write_priority(queue, "setGPUPriority:", requested);
	bool set_bg = write_priority(queue, "setBackgroundGPUPriority:", requested);
	long fg_after = -1, bg_after = -1;
	read_priority(queue, "getGPUPriority", &fg_after);
	read_priority(queue, "getBackgroundGPUPriority", &bg_after);
	U_LOG_W("Metal GPU priority, %s queue: requested %ld; foreground %ld -> %ld%s, background %ld -> %ld%s", label,
	        requested, fg, fg_after, set_fg ? "" : " (not set)", bg, bg_after, set_bg ? "" : " (not set)");
}

void
comp_metal_gpu_priority_apply(struct vk_bundle *vk, void *presenter_queue)
{
	long requested = debug_get_num_option_macos_gpu_queue_priority();
	if (requested > 2) {
		U_LOG_E(
		    "XRT_MACOS_GPU_QUEUE_PRIORITY=%ld is invalid (0 highest .. 2 lowest); leaving priorities unchanged",
		    requested);
		requested = -1;
	}

	@autoreleasepool {
		if (vk != NULL && vk->vkExportMetalObjectsEXT != NULL && vk->main_queue != NULL) {
			VkExportMetalCommandQueueInfoEXT queue_info = {
			    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT,
			    .queue = vk->main_queue->queue,
			};
			VkExportMetalObjectsInfoEXT export_info = {
			    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
			    .pNext = &queue_info,
			};
			vk->vkExportMetalObjectsEXT(vk->device, &export_info);
			apply_to_queue((__bridge id)(void *)queue_info.mtlCommandQueue, "compositor (MoltenVK)",
			               requested);
		}
		if (presenter_queue != NULL) {
			apply_to_queue((__bridge id)presenter_queue, "presenter", requested);
		}
	}
}

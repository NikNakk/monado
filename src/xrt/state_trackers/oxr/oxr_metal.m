// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Metal requirement and validation helpers.
 * @author OpenAI
 * @ingroup oxr_main
 */

#import <Metal/Metal.h>

#include "oxr_objects.h"
#include "oxr_logger.h"


XrResult
oxr_metal_get_requirements(struct oxr_logger *log,
                           struct oxr_system *sys,
                           XrGraphicsRequirementsMetalKHR *graphicsRequirements)
{
	if (!sys->suggested_metal_device_valid) {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil) {
			return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "MTLCreateSystemDefaultDevice returned nil");
		}

		sys->suggested_metal_device = (__bridge void *)device;
		sys->suggested_metal_device_valid = true;
	}

	graphicsRequirements->metalDevice = sys->suggested_metal_device;
	return XR_SUCCESS;
}

XrResult
oxr_metal_check_command_queue(struct oxr_logger *log, struct oxr_system *sys, void *command_queue)
{
	if (command_queue == NULL) {
		return oxr_error(log, XR_ERROR_GRAPHICS_DEVICE_INVALID,
		                 "XrGraphicsBindingMetalKHR::commandQueue cannot be NULL");
	}

	if (!sys->suggested_metal_device_valid || sys->suggested_metal_device == NULL) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "Has not called xrGetMetalGraphicsRequirementsKHR");
	}

	id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)command_queue;
	id<MTLDevice> device = [queue device];
	if (device == nil) {
		return oxr_error(log, XR_ERROR_GRAPHICS_DEVICE_INVALID,
		                 "XrGraphicsBindingMetalKHR::commandQueue has no backing MTLDevice");
	}

	if ((__bridge void *)device != sys->suggested_metal_device) {
		return oxr_error(log, XR_ERROR_GRAPHICS_DEVICE_INVALID,
		                 "XrGraphicsBindingMetalKHR::commandQueue device must match "
		                 "xrGetMetalGraphicsRequirementsKHR");
	}

	return XR_SUCCESS;
}

void
oxr_metal_cleanup_system(struct oxr_system *sys)
{
	if (!sys->suggested_metal_device_valid || sys->suggested_metal_device == NULL) {
		return;
	}

	[(id)sys->suggested_metal_device release];
	sys->suggested_metal_device = NULL;
	sys->suggested_metal_device_valid = false;
}

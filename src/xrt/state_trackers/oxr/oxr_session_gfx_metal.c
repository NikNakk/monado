// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds Metal specific session functions.
 * @author OpenAI
 * @ingroup oxr_main
 * @ingroup comp_client
 */

#include "xrt/xrt_gfx_metal.h"

#include "oxr_objects.h"
#include "oxr_logger.h"


XrResult
oxr_session_populate_metal(struct oxr_logger *log,
                           struct oxr_system *sys,
                           XrGraphicsBindingMetalKHR const *next,
                           struct oxr_session *sess)
{
	struct xrt_compositor_native *xcn = sess->xcn;
	struct xrt_compositor_metal *xcmetal =
	    xrt_gfx_metal_provider_create(xcn, sys->suggested_metal_device, next->commandQueue);
	if (xcmetal == NULL) {
		return oxr_error(log, XR_ERROR_INITIALIZATION_FAILED, "Failed to create a metal client compositor");
	}

	sess->compositor = &xcmetal->base;
	sess->create_swapchain = oxr_swapchain_metal_create;

	return XR_SUCCESS;
}

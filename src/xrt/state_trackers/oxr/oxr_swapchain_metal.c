// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds Metal swapchain related functions.
 * @author OpenAI
 * @ingroup oxr_main
 * @ingroup comp_client
 */

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_swapchain_common.h"


static XrResult
metal_enumerate_images(struct oxr_logger *log,
                       struct oxr_swapchain *sc,
                       uint32_t count,
                       XrSwapchainImageBaseHeader *images)
{
	if (count == 0) {
		return XR_SUCCESS;
	}

	if (images[0].type != XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "Unsupported XrSwapchainImageBaseHeader type");
	}

	struct xrt_swapchain_metal *xsc = (struct xrt_swapchain_metal *)sc->swapchain;
	XrSwapchainImageMetalKHR *metal_images = (XrSwapchainImageMetalKHR *)images;

	for (uint32_t i = 0; i < count; i++) {
		if (metal_images[i].type != XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR) {
			return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "Images array contains mixed types");
		}

		metal_images[i].texture = xsc->images[i];
	}

	return oxr_session_success_result(sc->sess);
}

XrResult
oxr_swapchain_metal_create(struct oxr_logger *log,
                           struct oxr_session *sess,
                           const XrSwapchainCreateInfo *createInfo,
                           struct oxr_swapchain **out_swapchain)
{
	struct oxr_swapchain *sc;
	XrResult ret = oxr_swapchain_common_create(log, sess, createInfo, &sc);
	if (ret != XR_SUCCESS) {
		return ret;
	}

	sc->enumerate_images = metal_enumerate_images;

	*out_swapchain = sc;
	return XR_SUCCESS;
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental XR_MNDX_display_distortion entrypoints.
 */

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"
#include "oxr_logger.h"
#include "oxr_objects.h"
#include "oxr_roles.h"

#include "util/u_trace_marker.h"

#ifdef OXR_HAVE_MNDX_display_distortion

static XrResult
get_display(struct oxr_logger *log, struct oxr_system *sys, struct xrt_device **out_head)
{
	struct xrt_device *head = GET_STATIC_XDEV_BY_ROLE(sys, head);
	if (head == NULL || head->hmd == NULL || head->hmd->view_count == 0 ||
	    head->hmd->view_count > XR_MNDX_DISPLAY_DISTORTION_MAX_VIEWS || head->compute_distortion == NULL) {
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED, "The system has no describable display distortion");
	}
	for (uint32_t i = 0; i < head->hmd->view_count; i++) {
		const struct xrt_matrix_2x2 *rot = &head->hmd->views[i].rot;
		if (rot->v[0] != 1.0f || rot->v[1] != 0.0f || rot->v[2] != 0.0f || rot->v[3] != 1.0f) {
			return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED, "View %u is rotated on the display", i);
		}
	}
	*out_head = head;
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetDisplayDistortionPropertiesMNDX(XrInstance instance,
                                         XrSystemId systemId,
                                         XrDisplayDistortionPropertiesMNDX *properties)
{
	OXR_TRACE_MARKER();

	struct oxr_instance *inst;
	struct oxr_logger log;
	OXR_VERIFY_INSTANCE_AND_INIT_LOG(&log, instance, inst, "xrGetDisplayDistortionPropertiesMNDX");
	OXR_VERIFY_EXTENSION(&log, inst, MNDX_display_distortion);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, properties, XR_TYPE_DISPLAY_DISTORTION_PROPERTIES_MNDX);
	struct oxr_system *sys = NULL;
	XrResult sys_ret = oxr_system_get_by_id(&log, inst, systemId, &sys);
	if (sys_ret != XR_SUCCESS) {
		return sys_ret;
	}

	struct xrt_device *head = NULL;
	XrResult ret = get_display(&log, sys, &head);
	if (ret != XR_SUCCESS) {
		return ret;
	}
	const struct xrt_hmd_parts *hmd = head->hmd;
	properties->displaySize.width = hmd->screens[0].w_pixels;
	properties->displaySize.height = hmd->screens[0].h_pixels;
	properties->nominalRefreshRate = hmd->screens[0].nominal_frame_interval_ns > 0
	                                     ? (float)(1e9 / (double)hmd->screens[0].nominal_frame_interval_ns)
	                                     : 0.0f;
	properties->viewCount = hmd->view_count;
	for (uint32_t i = 0; i < hmd->view_count; i++) {
		XrDisplayDistortionViewMNDX *view = &properties->views[i];
		view->viewport.offset.x = hmd->views[i].viewport.x_pixels;
		view->viewport.offset.y = hmd->views[i].viewport.y_pixels;
		view->viewport.extent.width = hmd->views[i].viewport.w_pixels;
		view->viewport.extent.height = hmd->views[i].viewport.h_pixels;
		view->fov.angleLeft = hmd->distortion.fov[i].angle_left;
		view->fov.angleRight = hmd->distortion.fov[i].angle_right;
		view->fov.angleUp = hmd->distortion.fov[i].angle_up;
		view->fov.angleDown = hmd->distortion.fov[i].angle_down;
	}
	return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrComputeDisplayDistortionMNDX(XrInstance instance,
                                   XrSystemId systemId,
                                   uint32_t viewIndex,
                                   uint32_t pointCount,
                                   const XrVector2f *points,
                                   XrVector2f *red,
                                   XrVector2f *green,
                                   XrVector2f *blue)
{
	OXR_TRACE_MARKER();

	struct oxr_instance *inst;
	struct oxr_logger log;
	OXR_VERIFY_INSTANCE_AND_INIT_LOG(&log, instance, inst, "xrComputeDisplayDistortionMNDX");
	OXR_VERIFY_EXTENSION(&log, inst, MNDX_display_distortion);
	struct oxr_system *sys = NULL;
	XrResult sys_ret = oxr_system_get_by_id(&log, inst, systemId, &sys);
	if (sys_ret != XR_SUCCESS) {
		return sys_ret;
	}
	if (pointCount > 0) {
		OXR_VERIFY_ARG_NOT_NULL(&log, points);
		OXR_VERIFY_ARG_NOT_NULL(&log, red);
		OXR_VERIFY_ARG_NOT_NULL(&log, green);
		OXR_VERIFY_ARG_NOT_NULL(&log, blue);
	}

	struct xrt_device *head = NULL;
	XrResult ret = get_display(&log, sys, &head);
	if (ret != XR_SUCCESS) {
		return ret;
	}
	if (viewIndex >= head->hmd->view_count) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE, "(viewIndex == %u) is not a view of the display",
		                 viewIndex);
	}
	for (uint32_t i = 0; i < pointCount; i++) {
		struct xrt_uv_triplet result;
		xrt_result_t xret = xrt_device_compute_distortion(head, viewIndex, points[i].x, points[i].y, &result);
		if (xret != XRT_SUCCESS) {
			return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE, "Distortion evaluation failed (%d)", (int)xret);
		}
		red[i] = (XrVector2f){result.r.x, result.r.y};
		green[i] = (XrVector2f){result.g.x, result.g.y};
		blue[i] = (XrVector2f){result.b.x, result.b.y};
	}
	return XR_SUCCESS;
}

#endif // OXR_HAVE_MNDX_display_distortion

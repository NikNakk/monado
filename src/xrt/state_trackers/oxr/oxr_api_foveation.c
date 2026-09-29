// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief XR_FB_swapchain_update_state and XR_FB_foveation entrypoints.
 */

#include "oxr_api_funcs.h"
#include "oxr_api_verify.h"
#include "oxr_foveation_policy.h"
#include "oxr_handle.h"
#include "oxr_logger.h"
#include "oxr_objects.h"
#include "oxr_roles.h"

#include "util/u_trace_marker.h"

#include <stdlib.h>


#ifdef OXR_HAVE_FB_foveation
static XrResult
oxr_foveation_profile_destroy(struct oxr_logger *log, struct oxr_handle_base *hb)
{
	(void)log;
	struct oxr_foveation_profile *profile = (struct oxr_foveation_profile *)hb;
	XrFoveationProfileFB xr_profile = oxr_foveation_profile_to_openxr(profile);

	/*
	 * Swapchains retain a copy of the effective policy, never a dependency on
	 * this object. Clear only the informational source handle so
	 * xrGetSwapchainStateFB never returns a handle that has been destroyed.
	 */
	if (profile->sess != NULL) {
		for (size_t i = 0; i < XRT_MAX_HANDLE_CHILDREN; ++i) {
			struct oxr_handle_base *child = profile->sess->handle.children[i];
			if (child == NULL || child->debug != OXR_XR_DEBUG_SWAPCHAIN) {
				continue;
			}

			struct oxr_swapchain *sc = (struct oxr_swapchain *)child;
			if (sc->foveation_source_profile == xr_profile) {
				sc->foveation_source_profile = XR_NULL_HANDLE;
			}
		}
	}

	free(profile);
	return XR_SUCCESS;
}

static XrResult
foveation_parse_error(struct oxr_logger *log, enum oxr_foveation_parse_result result)
{
	switch (result) {
	case OXR_FOVEATION_PARSE_SUCCESS: return XR_SUCCESS;
	case OXR_FOVEATION_PARSE_INVALID_LEVEL:
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "Invalid XrFoveationLevelFB");
	case OXR_FOVEATION_PARSE_INVALID_DYNAMIC:
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE, "Invalid XrFoveationDynamicFB");
	case OXR_FOVEATION_PARSE_UNSUPPORTED_CONFIGURATION:
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "Foveation profile chain requires XR_FB_foveation_configuration");
	case OXR_FOVEATION_PARSE_UNSUPPORTED_EYE_TRACKED:
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "Eye-tracked profile requires XR_META_foveation_eye_tracked");
	case OXR_FOVEATION_PARSE_INVALID_EYE_TRACKED_FLAGS:
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "XrFoveationEyeTrackedProfileCreateInfoMETA::flags must be zero");
	default: return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, "Unknown foveation parse result");
	}
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrCreateFoveationProfileFB(XrSession session,
                               const XrFoveationProfileCreateInfoFB *createInfo,
                               XrFoveationProfileFB *profile)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrCreateFoveationProfileFB");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, createInfo, XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB);
	OXR_VERIFY_ARG_NOT_NULL(&log, profile);

	struct oxr_instance *inst = sess->sys->inst;
	struct u_foveation_request request = {};
	enum oxr_foveation_parse_result parsed =
	    oxr_foveation_request_from_fb(createInfo,
#ifdef OXR_HAVE_FB_foveation_configuration
	                                  inst->extensions.FB_foveation_configuration,
#else
	                                  false,
#endif
#ifdef OXR_HAVE_META_foveation_eye_tracked
	                                  inst->extensions.META_foveation_eye_tracked,
#else
	                                  false,
#endif
	                                  &request);
	if (parsed != OXR_FOVEATION_PARSE_SUCCESS) {
		return foveation_parse_error(&log, parsed);
	}

	struct oxr_foveation_profile *fp = NULL;
	OXR_ALLOCATE_HANDLE_OR_RETURN(&log, fp, OXR_XR_DEBUG_FOVEATION_PROFILE,
	                              oxr_foveation_profile_destroy, &sess->handle);
	fp->sess = sess;
	fp->request = request;

	*profile = oxr_foveation_profile_to_openxr(fp);
	return oxr_session_success_result(sess);
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrDestroyFoveationProfileFB(XrFoveationProfileFB profile)
{
	OXR_TRACE_MARKER();

	struct oxr_foveation_profile *fp;
	struct oxr_logger log;
	OXR_VERIFY_FOVEATION_PROFILE_AND_INIT_LOG(&log, profile, fp, "xrDestroyFoveationProfileFB");

	return oxr_handle_destroy(&log, &fp->handle);
}
#endif // OXR_HAVE_FB_foveation


#ifdef OXR_HAVE_FB_swapchain_update_state
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrUpdateSwapchainFB(XrSwapchain swapchain, const XrSwapchainStateBaseHeaderFB *state)
{
	OXR_TRACE_MARKER();

	struct oxr_swapchain *sc;
	struct oxr_logger log;
	OXR_VERIFY_SWAPCHAIN_AND_INIT_LOG(&log, swapchain, sc, "xrUpdateSwapchainFB");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sc->sess);
	OXR_VERIFY_ARG_NOT_NULL(&log, state);

#ifdef OXR_HAVE_FB_foveation
	if (state->type == XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB) {
		struct oxr_instance *inst = sc->sess->sys->inst;
		if (!inst->extensions.FB_foveation) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "XrSwapchainStateFoveationFB requires XR_FB_foveation");
		}
		if (!sc->foveation_capable) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "Swapchain was not created with XrSwapchainCreateInfoFoveationFB");
		}

		const XrSwapchainStateFoveationFB *foveation = (const XrSwapchainStateFoveationFB *)state;
		if (foveation->flags != 0) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "XrSwapchainStateFoveationFB::flags must be zero");
		}

		struct oxr_foveation_profile *fp;
		OXR_VERIFY_FOVEATION_PROFILE_AND_INIT_LOG(&log, foveation->profile, fp,
		                                          "xrUpdateSwapchainFB");
		if (fp->sess != sc->sess) {
			return oxr_error(&log, XR_ERROR_HANDLE_INVALID,
			                 "Foveation profile belongs to a different session");
		}

		/*
		 * This copy is the important lifetime boundary: the profile may be
		 * destroyed immediately after this call without changing the
		 * effective swapchain foveation parameters.
		 */
		struct xrt_foveation_state xrt_state = {};
		if (!oxr_foveation_request_to_xrt(&fp->request, &xrt_state)) {
			return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
			                 "Failed to resolve foveation profile to backend-neutral state");
		}

		/*
		 * Fixed FB foveation defines verticalOffset in degrees. Resolve it
		 * against the actual HMD view FOVs here so every graphics backend
		 * receives the same per-view NDC centre. Eye-tracked META profiles
		 * are populated later from runtime-owned gaze instead.
		 */
		if (xrt_state.enabled && !xrt_state.eye_tracked) {
			struct xrt_device *head = GET_STATIC_XDEV_BY_ROLE(sc->sess->sys, head);
			if (head == NULL || head->hmd == NULL || head->hmd->view_count == 0) {
				return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
				                 "No HMD view FOVs available for fixed foveation");
			}
			uint32_t view_count = (uint32_t)head->hmd->view_count;
			if (view_count > XRT_MAX_VIEWS) {
				view_count = XRT_MAX_VIEWS;
			}
			if (!oxr_foveation_resolve_fixed_centres(
			        head->hmd->distortion.fov, view_count, &xrt_state)) {
				return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
				                 "Failed to resolve fixed foveation centres");
			}
		}

		/*
		 * A concrete graphics client may consume the resolved state here.
		 * Backends that have no application-rendering transport leave this
		 * callback NULL; the extension remains default-OFF until an enabled
		 * build has a complete graphics-API path.
		 */
		if (sc->swapchain != NULL && sc->swapchain->set_foveation != NULL) {
			xrt_result_t xret = xrt_swapchain_set_foveation(sc->swapchain, &xrt_state);
			if (xret != XRT_SUCCESS) {
				return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
				                 "Graphics backend rejected foveation state (%d)", (int)xret);
			}
		}

		sc->foveation_request = fp->request;
		sc->has_foveation_state = true;
		sc->foveation_source_profile = foveation->profile;
		return oxr_session_success_result(sc->sess);
	}
#endif

	return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
	                 "Unsupported XrSwapchainStateBaseHeaderFB structure type %d", state->type);
}

XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetSwapchainStateFB(XrSwapchain swapchain, XrSwapchainStateBaseHeaderFB *state)
{
	OXR_TRACE_MARKER();

	struct oxr_swapchain *sc;
	struct oxr_logger log;
	OXR_VERIFY_SWAPCHAIN_AND_INIT_LOG(&log, swapchain, sc, "xrGetSwapchainStateFB");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sc->sess);
	OXR_VERIFY_ARG_NOT_NULL(&log, state);

#ifdef OXR_HAVE_FB_foveation
	if (state->type == XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB) {
		struct oxr_instance *inst = sc->sess->sys->inst;
		if (!inst->extensions.FB_foveation) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "XrSwapchainStateFoveationFB requires XR_FB_foveation");
		}
		if (!sc->foveation_capable) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "Swapchain was not created with XrSwapchainCreateInfoFoveationFB");
		}

		XrSwapchainStateFoveationFB *foveation = (XrSwapchainStateFoveationFB *)state;
		foveation->flags = 0;
		/*
		 * The effective parameters are stored independently. If the source
		 * profile has since been destroyed, return XR_NULL_HANDLE rather than
		 * exposing a stale/invalid handle.
		 */
		foveation->profile = sc->foveation_source_profile;
		return oxr_session_success_result(sc->sess);
	}
#endif

	return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
	                 "Unsupported XrSwapchainStateBaseHeaderFB structure type %d", state->type);
}
#endif // OXR_HAVE_FB_swapchain_update_state


#ifdef OXR_HAVE_MNDX_foveation_metal
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetFoveationMetalStateMNDX(XrSwapchain swapchain,
                                 uint32_t viewIndex,
                                 uint32_t arrayLayer,
                                 XrFoveationMetalStateMNDX *state)
{
	OXR_TRACE_MARKER();

	struct oxr_swapchain *sc;
	struct oxr_logger log;
	OXR_VERIFY_SWAPCHAIN_AND_INIT_LOG(&log, swapchain, sc, "xrGetFoveationMetalStateMNDX");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sc->sess);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(&log, state, XR_TYPE_FOVEATION_METAL_STATE_MNDX);

	if (sc->sess->gfx_ext != OXR_SESSION_GRAPHICS_EXT_METAL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "xrGetFoveationMetalStateMNDX requires a Metal session");
	}
	if (arrayLayer >= sc->array_layer_count) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "arrayLayer %u is outside swapchain array size %u",
		                 arrayLayer, sc->array_layer_count);
	}

	struct xrt_device *head = GET_STATIC_XDEV_BY_ROLE(sc->sess->sys, head);
	if (head == NULL || head->hmd == NULL || viewIndex >= head->hmd->view_count) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "viewIndex %u is not valid for this system", viewIndex);
	}
	if (sc->swapchain == NULL || sc->swapchain->set_foveation == NULL) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "Metal swapchain has no foveation transport");
	}

	struct xrt_swapchain_metal *xscm = xrt_swapchain_metal(sc->swapchain);
	struct xrt_metal_foveation_state native = {};
	xrt_result_t xret =
	    xrt_swapchain_metal_get_foveation_state(xscm, viewIndex, arrayLayer, &native);
	if (xret == XRT_ERROR_NOT_IMPLEMENTED) {
		return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "Metal foveation state is not currently available");
	}
	if (xret != XRT_SUCCESS) {
		return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
		                 "Metal foveation state query failed (%d)", (int)xret);
	}

	state->foveationEnabled = native.enabled ? XR_TRUE : XR_FALSE;
	state->rasterizationRateMap = native.rasterization_rate_map;
	state->physicalWidth = native.physical_width;
	state->physicalHeight = native.physical_height;
	state->revision = native.revision;
	return oxr_session_success_result(sc->sess);
}
#endif // OXR_HAVE_MNDX_foveation_metal

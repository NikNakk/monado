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
#include "math/m_api.h"
#include "os/os_time.h"

#include <stdlib.h>


#ifdef OXR_HAVE_META_foveation_eye_tracked
static XrResult
eye_tracking_acquire(struct oxr_logger *log, struct oxr_session *sess)
{
	struct xrt_device *eyes = GET_STATIC_XDEV_BY_ROLE(sess->sys, eyes);
	if (eyes == NULL || !eyes->supported.eye_gaze || sess->sys->xso->semantic.view == NULL) {
		return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED,
		                 "Eye-tracked foveation requires an internal eye-gaze device and view space");
	}

	if (sess->eye_tracked_foveation.active_swapchain_count == 0) {
		xrt_result_t xret =
		    xrt_system_devices_feature_inc(sess->sys->xsysd, XRT_DEVICE_FEATURE_EYE_TRACKING);
		if (xret != XRT_SUCCESS) {
			return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED,
			                 "Could not enable runtime-owned eye tracking (%d)", (int)xret);
		}
		sess->eye_tracked_foveation.feature_acquired = true;

		xret = xrt_space_overseer_create_pose_space(
		    sess->sys->xso, eyes, XRT_INPUT_GENERIC_EYE_GAZE_POSE,
		    &sess->eye_tracked_foveation.gaze_space);
		if (xret != XRT_SUCCESS || sess->eye_tracked_foveation.gaze_space == NULL) {
			(void)xrt_system_devices_feature_dec(
			    sess->sys->xsysd, XRT_DEVICE_FEATURE_EYE_TRACKING);
			sess->eye_tracked_foveation.feature_acquired = false;
			return oxr_error(log, XR_ERROR_FEATURE_UNSUPPORTED,
			                 "Could not create runtime-private eye-gaze space (%d)", (int)xret);
		}
	}

	sess->eye_tracked_foveation.active_swapchain_count++;
	return XR_SUCCESS;
}

static void
eye_tracking_release(struct oxr_session *sess)
{
	if (sess == NULL || sess->eye_tracked_foveation.active_swapchain_count == 0) {
		return;
	}

	sess->eye_tracked_foveation.active_swapchain_count--;
	if (sess->eye_tracked_foveation.active_swapchain_count != 0) {
		return;
	}

	xrt_space_reference(&sess->eye_tracked_foveation.gaze_space, NULL);
	if (sess->eye_tracked_foveation.feature_acquired) {
		(void)xrt_system_devices_feature_dec(
		    sess->sys->xsysd, XRT_DEVICE_FEATURE_EYE_TRACKING);
		sess->eye_tracked_foveation.feature_acquired = false;
	}
	sess->eye_tracked_foveation.valid = false;
}

static bool
sample_eye_tracked_centres(struct oxr_session *sess, struct xrt_foveation_state *state)
{
	if (sess == NULL || state == NULL || sess->eye_tracked_foveation.gaze_space == NULL ||
	    sess->sys->xso->semantic.view == NULL) {
		return false;
	}

	struct xrt_device *head = GET_STATIC_XDEV_BY_ROLE(sess->sys, head);
	if (head == NULL || head->hmd == NULL || head->hmd->view_count == 0) {
		return false;
	}

	const struct xrt_pose identity = XRT_POSE_IDENTITY;
	struct xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
	xrt_result_t xret = xrt_space_overseer_locate_space(
	    sess->sys->xso,
	    sess->sys->xso->semantic.view,
	    &identity,
	    os_monotonic_get_ns(),
	    sess->eye_tracked_foveation.gaze_space,
	    &identity,
	    &relation);
	if (xret != XRT_SUCCESS) {
		return false;
	}

	const enum xrt_space_relation_flags required =
	    (enum xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	if ((relation.relation_flags & required) != required) {
		return false;
	}

	const struct xrt_vec3 forward = {0.0f, 0.0f, -1.0f};
	struct xrt_vec3 direction = {};
	math_quat_rotate_vec3(&relation.pose.orientation, &forward, &direction);

	uint32_t view_count = (uint32_t)head->hmd->view_count;
	if (view_count > XRT_MAX_VIEWS) {
		view_count = XRT_MAX_VIEWS;
	}

	return oxr_foveation_resolve_gaze_centres(
	    &direction, head->hmd->distortion.fov, view_count,
	    state->vertical_offset_degrees, state);
}

static void
store_eye_tracked_state(struct oxr_session *sess,
                        const struct xrt_foveation_state *state,
                        bool gaze_valid)
{
	for (uint32_t i = 0; i < XR_FOVEATION_CENTER_SIZE_META; ++i) {
		if (i < state->view_count && state->views[i].center_valid) {
			sess->eye_tracked_foveation.center[i].x = state->views[i].center.x;
			sess->eye_tracked_foveation.center[i].y = state->views[i].center.y;
		} else {
			sess->eye_tracked_foveation.center[i] = (XrVector2f){0.0f, 0.0f};
		}
	}
	sess->eye_tracked_foveation.valid = gaze_valid;
	sess->eye_tracked_foveation.revision++;
}
#endif // OXR_HAVE_META_foveation_eye_tracked


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

#ifdef OXR_HAVE_META_foveation_eye_tracked
		const bool old_eye_tracked =
		    sc->has_foveation_state && sc->foveation_request.enabled &&
		    sc->foveation_request.eye_tracked;
		const bool new_eye_tracked = fp->request.enabled && fp->request.eye_tracked;
		bool acquired_eye_tracking = false;
		bool gaze_valid = false;
#endif

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

		if (xrt_state.enabled) {
			if (sc->swapchain == NULL || sc->swapchain->set_foveation == NULL ||
			    (sc->swapchain->foveation_capabilities & XRT_FOVEATION_CAPABILITY_FIXED) == 0) {
				return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
				                 "Graphics swapchain has no foveation rendering transport");
			}
			if (xrt_state.eye_tracked &&
			    (sc->swapchain->foveation_capabilities & XRT_FOVEATION_CAPABILITY_EYE_TRACKED) == 0) {
				return oxr_error(&log, XR_ERROR_FEATURE_UNSUPPORTED,
				                 "Graphics swapchain cannot consume eye-tracked foveation centres");
			}
		}

#ifdef OXR_HAVE_META_foveation_eye_tracked
		if (new_eye_tracked && !old_eye_tracked) {
			XrResult acquire_result = eye_tracking_acquire(&log, sc->sess);
			if (XR_FAILED(acquire_result)) {
				return acquire_result;
			}
			acquired_eye_tracking = true;
		}
#endif

		/*
		 * Resolve the renderer-facing centre entirely inside the runtime.
		 * META uses the private eye-gaze space above; applications do not
		 * need XR_EXT_eye_gaze_interaction. If tracking is temporarily
		 * invalid, render with a safe fixed-centre fallback while reporting
		 * the META state as invalid.
		 */
		if (xrt_state.enabled) {
			struct xrt_device *head = GET_STATIC_XDEV_BY_ROLE(sc->sess->sys, head);
			if (head == NULL || head->hmd == NULL || head->hmd->view_count == 0) {
#ifdef OXR_HAVE_META_foveation_eye_tracked
				if (acquired_eye_tracking) {
					eye_tracking_release(sc->sess);
				}
#endif
				return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
				                 "No HMD view FOVs available for foveation");
			}

			uint32_t view_count = (uint32_t)head->hmd->view_count;
			if (view_count > XRT_MAX_VIEWS) {
				view_count = XRT_MAX_VIEWS;
			}

#ifdef OXR_HAVE_META_foveation_eye_tracked
			if (xrt_state.eye_tracked) {
				gaze_valid = sample_eye_tracked_centres(sc->sess, &xrt_state);
				if (!gaze_valid &&
				    !oxr_foveation_resolve_fixed_centres(
				        head->hmd->distortion.fov, view_count, &xrt_state)) {
					if (acquired_eye_tracking) {
						eye_tracking_release(sc->sess);
					}
					return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
					                 "Failed to resolve eye-tracked foveation fallback");
				}
			} else
#endif
			if (!oxr_foveation_resolve_fixed_centres(
			        head->hmd->distortion.fov, view_count, &xrt_state)) {
#ifdef OXR_HAVE_META_foveation_eye_tracked
				if (acquired_eye_tracking) {
					eye_tracking_release(sc->sess);
				}
#endif
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
#ifdef OXR_HAVE_META_foveation_eye_tracked
				if (acquired_eye_tracking) {
					eye_tracking_release(sc->sess);
				}
#endif
				return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
				                 "Graphics backend rejected foveation state (%d)", (int)xret);
			}
		}

		sc->foveation_request = fp->request;
		sc->has_foveation_state = true;
		sc->foveation_source_profile = foveation->profile;

#ifdef OXR_HAVE_META_foveation_eye_tracked
		if (new_eye_tracked) {
			store_eye_tracked_state(sc->sess, &xrt_state, gaze_valid);
		}
		if (old_eye_tracked && !new_eye_tracked) {
			eye_tracking_release(sc->sess);
		}
#endif
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


#ifdef OXR_HAVE_META_foveation_eye_tracked
XRAPI_ATTR XrResult XRAPI_CALL
oxr_xrGetFoveationEyeTrackedStateMETA(XrSession session,
                                      XrFoveationEyeTrackedStateMETA *foveationState)
{
	OXR_TRACE_MARKER();

	struct oxr_session *sess;
	struct oxr_logger log;
	OXR_VERIFY_SESSION_AND_INIT_LOG(&log, session, sess, "xrGetFoveationEyeTrackedStateMETA");
	OXR_VERIFY_SESSION_NOT_LOST(&log, sess);
	OXR_VERIFY_ARG_TYPE_AND_NOT_NULL(
	    &log, foveationState, XR_TYPE_FOVEATION_EYE_TRACKED_STATE_META);

	if (foveationState->next != NULL) {
		return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
		                 "XrFoveationEyeTrackedStateMETA::next must be NULL");
	}

	for (uint32_t i = 0; i < XR_FOVEATION_CENTER_SIZE_META; ++i) {
		foveationState->foveationCenter[i] = sess->eye_tracked_foveation.center[i];
	}
	foveationState->flags = sess->eye_tracked_foveation.valid
	                            ? XR_FOVEATION_EYE_TRACKED_STATE_VALID_BIT_META
	                            : 0;

	return oxr_session_success_result(sess);
}
#endif // OXR_HAVE_META_foveation_eye_tracked


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
	xrt_result_t xret = XRT_ERROR_NOT_IMPLEMENTED;

	XrFoveationMetalPackedStateMNDX *packed = NULL;
	if (state->next != NULL) {
		XrBaseOutStructure *next = (XrBaseOutStructure *)state->next;
		if (next->type != XR_TYPE_FOVEATION_METAL_PACKED_STATE_MNDX) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "Unsupported XrFoveationMetalStateMNDX::next type %d",
			                 next->type);
		}
		packed = (XrFoveationMetalPackedStateMNDX *)state->next;
		if (packed->next != NULL || packed->viewCount == 0 ||
		    packed->viewCount > XRT_MAX_VIEWS || packed->views == NULL) {
			return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
			                 "Invalid packed Metal foveation view list");
		}

		struct xrt_metal_foveation_view_layout layouts[XRT_MAX_VIEWS] = {0};
		for (uint32_t i = 0; i < packed->viewCount; ++i) {
			const XrFoveationMetalViewMNDX *view = &packed->views[i];
			if (view->viewIndex >= head->hmd->view_count ||
			    view->imageRect.offset.x < 0 || view->imageRect.offset.y < 0 ||
			    view->imageRect.extent.width <= 0 || view->imageRect.extent.height <= 0 ||
			    (uint64_t)view->imageRect.offset.x + (uint32_t)view->imageRect.extent.width > sc->width ||
			    (uint64_t)view->imageRect.offset.y + (uint32_t)view->imageRect.extent.height > sc->height) {
				return oxr_error(&log, XR_ERROR_VALIDATION_FAILURE,
				                 "Invalid packed Metal foveation view %u", i);
			}
			layouts[i] = (struct xrt_metal_foveation_view_layout){
			    .view_index = view->viewIndex,
			    .offset_x = view->imageRect.offset.x,
			    .offset_y = view->imageRect.offset.y,
			    .width = (uint32_t)view->imageRect.extent.width,
			    .height = (uint32_t)view->imageRect.extent.height,
			};
		}
		xret = xrt_swapchain_metal_get_packed_foveation_state(
		    xscm, layouts, packed->viewCount, arrayLayer, &native);
	} else {
		xret = xrt_swapchain_metal_get_foveation_state(
		    xscm, viewIndex, arrayLayer, &native);
	}
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

	if (packed != NULL) {
		if (native.sample_count > XR_MNDX_FOVEATION_METAL_RATE_SAMPLE_COUNT ||
		    native.compositor_map.boundary_count > XR_MNDX_FOVEATION_METAL_MAP_BOUNDARY_COUNT) {
			return oxr_error(&log, XR_ERROR_RUNTIME_FAILURE,
			                 "Runtime Metal foveation map exceeds transport limits");
		}
		packed->horizontalSampleCount = native.sample_count;
		packed->verticalSampleCount = native.sample_count;
		memcpy(packed->horizontalSampleRates, native.horizontal_rates,
		       native.sample_count * sizeof(float));
		memcpy(packed->verticalSampleRates, native.vertical_rates,
		       native.sample_count * sizeof(float));
		packed->boundaryCount = native.compositor_map.boundary_count;
		memcpy(packed->x, native.compositor_map.x,
		       native.compositor_map.boundary_count * sizeof(float));
		memcpy(packed->y, native.compositor_map.y,
		       native.compositor_map.boundary_count * sizeof(float));
	}
	return oxr_session_success_result(sc->sess);
}
#endif // OXR_HAVE_MNDX_foveation_metal

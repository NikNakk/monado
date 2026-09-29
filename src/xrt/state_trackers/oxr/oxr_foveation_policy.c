// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "oxr_foveation_policy.h"

#include <stddef.h>
#include <math.h>


static enum oxr_foveation_parse_result
convert_level(XrFoveationLevelFB level, enum u_foveation_level *out_level)
{
	switch (level) {
	case XR_FOVEATION_LEVEL_NONE_FB: *out_level = U_FOVEATION_LEVEL_NONE; return OXR_FOVEATION_PARSE_SUCCESS;
	case XR_FOVEATION_LEVEL_LOW_FB: *out_level = U_FOVEATION_LEVEL_LOW; return OXR_FOVEATION_PARSE_SUCCESS;
	case XR_FOVEATION_LEVEL_MEDIUM_FB: *out_level = U_FOVEATION_LEVEL_MEDIUM; return OXR_FOVEATION_PARSE_SUCCESS;
	case XR_FOVEATION_LEVEL_HIGH_FB: *out_level = U_FOVEATION_LEVEL_HIGH; return OXR_FOVEATION_PARSE_SUCCESS;
	default: return OXR_FOVEATION_PARSE_INVALID_LEVEL;
	}
}

enum oxr_foveation_parse_result
oxr_foveation_request_from_fb(const XrFoveationProfileCreateInfoFB *create_info,
                              bool allow_configuration,
                              bool allow_eye_tracked,
                              struct u_foveation_request *out_request)
{
	if (create_info == NULL || out_request == NULL) {
		return OXR_FOVEATION_PARSE_UNSUPPORTED_CONFIGURATION;
	}

	/*
	 * XR_FB_foveation requires the bare base create-info to mean a profile
	 * that applies no foveation.
	 */
	if (create_info->next == NULL) {
		if (!u_foveation_request_from_level(
		        U_FOVEATION_LEVEL_NONE, false, false, 0.0f, out_request)) {
			return OXR_FOVEATION_PARSE_UNSUPPORTED_CONFIGURATION;
		}
		return OXR_FOVEATION_PARSE_SUCCESS;
	}

	const XrBaseInStructure *next = (const XrBaseInStructure *)create_info->next;
	if (next->type != XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB) {
		return OXR_FOVEATION_PARSE_UNSUPPORTED_CONFIGURATION;
	}
	if (!allow_configuration) {
		return OXR_FOVEATION_PARSE_UNSUPPORTED_CONFIGURATION;
	}

	const XrFoveationLevelProfileCreateInfoFB *level_info =
	    (const XrFoveationLevelProfileCreateInfoFB *)next;

	enum u_foveation_level level;
	enum oxr_foveation_parse_result result = convert_level(level_info->level, &level);
	if (result != OXR_FOVEATION_PARSE_SUCCESS) {
		return result;
	}

	bool dynamic = false;
	switch (level_info->dynamic) {
	case XR_FOVEATION_DYNAMIC_DISABLED_FB: dynamic = false; break;
	case XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB: dynamic = true; break;
	default: return OXR_FOVEATION_PARSE_INVALID_DYNAMIC;
	}

	bool eye_tracked = false;
	if (level_info->next != NULL) {
		const XrBaseInStructure *level_next = (const XrBaseInStructure *)level_info->next;
		if (level_next->type != XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META) {
			return OXR_FOVEATION_PARSE_UNSUPPORTED_CONFIGURATION;
		}
		if (!allow_eye_tracked) {
			return OXR_FOVEATION_PARSE_UNSUPPORTED_EYE_TRACKED;
		}

		const XrFoveationEyeTrackedProfileCreateInfoMETA *eye_info =
		    (const XrFoveationEyeTrackedProfileCreateInfoMETA *)level_next;
		if (eye_info->flags != 0) {
			return OXR_FOVEATION_PARSE_INVALID_EYE_TRACKED_FLAGS;
		}
		if (eye_info->next != NULL) {
			return OXR_FOVEATION_PARSE_UNSUPPORTED_CONFIGURATION;
		}
		eye_tracked = true;
	}

	if (!u_foveation_request_from_level(
	        level, dynamic, eye_tracked, level_info->verticalOffset, out_request)) {
		return OXR_FOVEATION_PARSE_INVALID_LEVEL;
	}

	return OXR_FOVEATION_PARSE_SUCCESS;
}


bool
oxr_foveation_request_to_xrt(const struct u_foveation_request *request,
                             struct xrt_foveation_state *out_state)
{
	if (request == NULL || out_state == NULL) {
		return false;
	}

	struct xrt_foveation_state state = {
	    .enabled = request->enabled,
	    .dynamic = request->dynamic,
	    .eye_tracked = request->eye_tracked,
	    .center_rate = 1.0f,
	    .middle_rate = 1.0f,
	    .peripheral_rate = 1.0f,
	    .center_half_extent = 0.0f,
	    .middle_half_extent = 0.0f,
	    .vertical_offset_degrees = request->vertical_offset_degrees,
	    .view_count = 0,
	};

	if (request->enabled) {
		const struct u_foveation_profile *profile =
		    u_foveation_profile_get(request->profile_index);
		if (profile == NULL) {
			return false;
		}

		state.center_rate = profile->center_rate;
		state.middle_rate = profile->middle_rate;
		state.peripheral_rate = profile->peripheral_rate;
		state.center_half_extent = profile->center_half_extent;
		state.middle_half_extent = profile->middle_half_extent;
	}

	*out_state = state;
	return true;
}


bool
oxr_foveation_resolve_fixed_centres(const struct xrt_fov *fovs,
                                    uint32_t view_count,
                                    struct xrt_foveation_state *state)
{
	if (fovs == NULL || state == NULL || view_count == 0 || view_count > XRT_MAX_VIEWS) {
		return false;
	}

	const float offset_rad = state->vertical_offset_degrees * 0.01745329251994329577f;
	for (uint32_t i = 0; i < view_count; ++i) {
		const float down = tanf(fovs[i].angle_down);
		const float up = tanf(fovs[i].angle_up);
		const float tangent_height = up - down;
		if (!(tangent_height > 0.0f) || !isfinite(tangent_height)) {
			return false;
		}

		/*
		 * NDC y=0 is the midpoint in tangent space. Convert that midpoint to
		 * an angle, add FB's degree offset, then project back into NDC.
		 */
		const float centre_tangent_y = 0.5f * (up + down);
		const float centre_angle = atanf(centre_tangent_y);
		const float shifted_tangent_y = tanf(centre_angle + offset_rad);
		float y_ndc = 2.0f * ((shifted_tangent_y - down) / tangent_height) - 1.0f;
		if (!isfinite(y_ndc)) {
			return false;
		}
		if (y_ndc < -1.0f) {
			y_ndc = -1.0f;
		} else if (y_ndc > 1.0f) {
			y_ndc = 1.0f;
		}

		state->views[i].center.x = 0.0f;
		state->views[i].center.y = y_ndc;
		state->views[i].center_valid = true;
	}

	state->view_count = view_count;
	return true;
}


bool
oxr_foveation_resolve_gaze_centres(const struct xrt_vec3 *view_direction,
                                   const struct xrt_fov *fovs,
                                   uint32_t view_count,
                                   float vertical_offset_degrees,
                                   struct xrt_foveation_state *state)
{
	if (view_direction == NULL || fovs == NULL || state == NULL ||
	    view_count == 0 || view_count > XRT_MAX_VIEWS) {
		return false;
	}

	const float horizontal = sqrtf(view_direction->x * view_direction->x +
	                               view_direction->z * view_direction->z);
	if (!(horizontal > 0.000001f) || !isfinite(horizontal) ||
	    !isfinite(view_direction->x) || !isfinite(view_direction->y) ||
	    !isfinite(view_direction->z) || view_direction->z >= -0.000001f) {
		return false;
	}

	const float yaw = atan2f(view_direction->x, -view_direction->z);
	const float pitch = atan2f(view_direction->y, horizontal) +
	                    vertical_offset_degrees * 0.01745329251994329577f;
	const float tangent_x = tanf(yaw);
	const float tangent_y = tanf(pitch);
	if (!isfinite(tangent_x) || !isfinite(tangent_y)) {
		return false;
	}

	for (uint32_t i = 0; i < view_count; ++i) {
		const float left = tanf(fovs[i].angle_left);
		const float right = tanf(fovs[i].angle_right);
		const float down = tanf(fovs[i].angle_down);
		const float up = tanf(fovs[i].angle_up);
		const float width = right - left;
		const float height = up - down;
		if (!(width > 0.0f) || !(height > 0.0f) || !isfinite(width) || !isfinite(height)) {
			return false;
		}

		float x_ndc = 2.0f * ((tangent_x - left) / width) - 1.0f;
		float y_ndc = 2.0f * ((tangent_y - down) / height) - 1.0f;
		if (!isfinite(x_ndc) || !isfinite(y_ndc)) {
			return false;
		}

		x_ndc = fmaxf(-1.0f, fminf(1.0f, x_ndc));
		y_ndc = fmaxf(-1.0f, fminf(1.0f, y_ndc));

		state->views[i].center.x = x_ndc;
		state->views[i].center.y = y_ndc;
		state->views[i].center_valid = true;
	}

	state->view_count = view_count;
	return true;
}

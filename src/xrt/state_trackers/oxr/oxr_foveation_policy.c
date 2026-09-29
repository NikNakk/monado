// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "oxr_foveation_policy.h"

#include <stddef.h>


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

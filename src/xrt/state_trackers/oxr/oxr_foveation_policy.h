// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief OpenXR FB/META foveation structures mapped to generic runtime policy.
 */

#pragma once

#include "foveation/u_foveation.h"
#include "xrt/xrt_openxr_includes.h"
#include "xrt/xrt_compositor.h"

#ifdef __cplusplus
extern "C" {
#endif

enum oxr_foveation_parse_result
{
	OXR_FOVEATION_PARSE_SUCCESS = 0,
	OXR_FOVEATION_PARSE_INVALID_LEVEL,
	OXR_FOVEATION_PARSE_INVALID_DYNAMIC,
	OXR_FOVEATION_PARSE_UNSUPPORTED_CONFIGURATION,
	OXR_FOVEATION_PARSE_UNSUPPORTED_EYE_TRACKED,
	OXR_FOVEATION_PARSE_INVALID_EYE_TRACKED_FLAGS,
};

/*!
 * Convert the standard FB/META foveation profile-create chain into Monado's
 * graphics-API-independent foveation policy.
 *
 * The base XR_FB_foveation structure with no configuration chained to it
 * explicitly means "no foveation".
 *
 * @param create_info Base XR_FB_foveation profile creation structure.
 * @param allow_configuration Whether XR_FB_foveation_configuration is enabled.
 * @param allow_eye_tracked Whether XR_META_foveation_eye_tracked is enabled.
 * @param out_request Resulting generic policy request.
 */
enum oxr_foveation_parse_result
oxr_foveation_request_from_fb(const XrFoveationProfileCreateInfoFB *create_info,
                              bool allow_configuration,
                              bool allow_eye_tracked,
                              struct u_foveation_request *out_request);

/*!
 * Resolve policy-helper state into the XRT graphics-backend contract.
 * Eye-tracked centres are intentionally not derived here: the runtime fills
 * them later from its private tracking path.
 */
bool
oxr_foveation_request_to_xrt(const struct u_foveation_request *request,
                             struct xrt_foveation_state *out_state);

#ifdef __cplusplus
}
#endif

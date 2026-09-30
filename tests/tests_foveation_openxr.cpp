// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "catch_amalgamated.hpp"

#include <oxr/oxr_foveation_policy.h>


TEST_CASE("FB foveation base profile means no foveation")
{
	XrFoveationProfileCreateInfoFB create_info{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
	struct u_foveation_request request
	{
	};

	CHECK(oxr_foveation_request_from_fb(&create_info, false, false, &request) == OXR_FOVEATION_PARSE_SUCCESS);
	CHECK_FALSE(request.enabled);
	CHECK_FALSE(request.dynamic);
	CHECK_FALSE(request.eye_tracked);
}

TEST_CASE("FB foveation configuration maps to generic policy")
{
	struct case_data
	{
		XrFoveationLevelFB level;
		int profile;
		bool enabled;
	};

	const case_data cases[] = {
	    {XR_FOVEATION_LEVEL_NONE_FB, U_FOVEATION_PROFILE_REFERENCE, false},
	    {XR_FOVEATION_LEVEL_LOW_FB, U_FOVEATION_PROFILE_REFERENCE, true},
	    {XR_FOVEATION_LEVEL_MEDIUM_FB, U_FOVEATION_PROFILE_STRONG, true},
	    {XR_FOVEATION_LEVEL_HIGH_FB, U_FOVEATION_PROFILE_AGGRESSIVE, true},
	};

	for (const auto &entry : cases) {
		XrFoveationLevelProfileCreateInfoFB level_info{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
		level_info.level = entry.level;
		level_info.verticalOffset = 2.25f;
		level_info.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;

		XrFoveationProfileCreateInfoFB create_info{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
		create_info.next = &level_info;

		struct u_foveation_request request
		{
		};
		CAPTURE(entry.level);
		REQUIRE(oxr_foveation_request_from_fb(&create_info, true, false, &request) ==
		        OXR_FOVEATION_PARSE_SUCCESS);
		CHECK(request.enabled == entry.enabled);
		CHECK(request.profile_index == entry.profile);
		CHECK_FALSE(request.dynamic);
		CHECK_FALSE(request.eye_tracked);
		CHECK(request.vertical_offset_degrees == Catch::Approx(2.25f));
	}
}

TEST_CASE("FB dynamic and META eye-tracked policy stays runtime-owned")
{
	XrFoveationEyeTrackedProfileCreateInfoMETA eye_info{XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META};

	XrFoveationLevelProfileCreateInfoFB level_info{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
	level_info.next = &eye_info;
	level_info.level = XR_FOVEATION_LEVEL_HIGH_FB;
	level_info.verticalOffset = -1.5f;
	level_info.dynamic = XR_FOVEATION_DYNAMIC_LEVEL_ENABLED_FB;

	XrFoveationProfileCreateInfoFB create_info{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
	create_info.next = &level_info;

	struct u_foveation_request request
	{
	};
	REQUIRE(oxr_foveation_request_from_fb(&create_info, true, true, &request) == OXR_FOVEATION_PARSE_SUCCESS);
	CHECK(request.enabled);
	CHECK(request.profile_index == U_FOVEATION_PROFILE_AGGRESSIVE);
	CHECK(request.dynamic);
	CHECK(request.eye_tracked);
	CHECK(request.vertical_offset_degrees == Catch::Approx(-1.5f));
}

TEST_CASE("FB foveation parser enforces extension availability and valid enums")
{
	XrFoveationLevelProfileCreateInfoFB level_info{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
	level_info.level = XR_FOVEATION_LEVEL_MEDIUM_FB;
	level_info.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;

	XrFoveationProfileCreateInfoFB create_info{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
	create_info.next = &level_info;

	struct u_foveation_request request
	{
	};
	CHECK(oxr_foveation_request_from_fb(&create_info, false, false, &request) ==
	      OXR_FOVEATION_PARSE_UNSUPPORTED_CONFIGURATION);

	level_info.level = static_cast<XrFoveationLevelFB>(99);
	CHECK(oxr_foveation_request_from_fb(&create_info, true, false, &request) == OXR_FOVEATION_PARSE_INVALID_LEVEL);

	level_info.level = XR_FOVEATION_LEVEL_LOW_FB;
	level_info.dynamic = static_cast<XrFoveationDynamicFB>(99);
	CHECK(oxr_foveation_request_from_fb(&create_info, true, false, &request) ==
	      OXR_FOVEATION_PARSE_INVALID_DYNAMIC);

	level_info.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
	XrFoveationEyeTrackedProfileCreateInfoMETA eye_info{XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META};
	level_info.next = &eye_info;
	CHECK(oxr_foveation_request_from_fb(&create_info, true, false, &request) ==
	      OXR_FOVEATION_PARSE_UNSUPPORTED_EYE_TRACKED);

	eye_info.flags = 1;
	CHECK(oxr_foveation_request_from_fb(&create_info, true, true, &request) ==
	      OXR_FOVEATION_PARSE_INVALID_EYE_TRACKED_FLAGS);
}


TEST_CASE("standard foveation resolves to backend-neutral XRT state")
{
	struct u_foveation_request request
	{
	};
	REQUIRE(u_foveation_request_from_level(U_FOVEATION_LEVEL_HIGH, true, true, 1.25f, &request));

	struct xrt_foveation_state state
	{
	};
	REQUIRE(oxr_foveation_request_to_xrt(&request, &state));
	CHECK(state.enabled);
	CHECK(state.dynamic);
	CHECK(state.eye_tracked);
	CHECK(state.center_rate == Catch::Approx(1.0f));
	CHECK(state.middle_rate == Catch::Approx(0.5f));
	CHECK(state.peripheral_rate == Catch::Approx(0.25f));
	CHECK(state.center_half_extent == Catch::Approx(0.09375f));
	CHECK(state.middle_half_extent == Catch::Approx(0.21875f));
	CHECK(state.vertical_offset_degrees == Catch::Approx(1.25f));
	CHECK(state.view_count == 0);

	REQUIRE(u_foveation_request_from_level(U_FOVEATION_LEVEL_NONE, false, false, 0.0f, &request));
	REQUIRE(oxr_foveation_request_to_xrt(&request, &state));
	CHECK_FALSE(state.enabled);
	CHECK(state.center_rate == Catch::Approx(1.0f));
	CHECK(state.middle_rate == Catch::Approx(1.0f));
	CHECK(state.peripheral_rate == Catch::Approx(1.0f));
}


TEST_CASE("fixed FB vertical offset resolves relative to view centre")
{
	struct xrt_fov fovs[2] = {
	    {-0.8f, 0.8f, 0.7f, -0.7f},
	    {-0.9f, 0.7f, 0.8f, -0.6f},
	};
	struct xrt_foveation_state state
	{
	};
	state.vertical_offset_degrees = 0.0f;

	REQUIRE(oxr_foveation_resolve_fixed_centres(fovs, 2, &state));
	CHECK(state.view_count == 2);
	CHECK(state.views[0].center_valid);
	CHECK(state.views[1].center_valid);
	CHECK(state.views[0].center.x == Catch::Approx(0.0f));
	CHECK(state.views[0].center.y == Catch::Approx(0.0f).margin(1e-6f));
	CHECK(state.views[1].center.y == Catch::Approx(0.0f).margin(1e-6f));

	state.vertical_offset_degrees = 10.0f;
	REQUIRE(oxr_foveation_resolve_fixed_centres(fovs, 2, &state));
	CHECK(state.views[0].center.y > 0.0f);
	CHECK(state.views[1].center.y > 0.0f);

	state.vertical_offset_degrees = -10.0f;
	REQUIRE(oxr_foveation_resolve_fixed_centres(fovs, 2, &state));
	CHECK(state.views[0].center.y < 0.0f);
	CHECK(state.views[1].center.y < 0.0f);
}


TEST_CASE("runtime-owned gaze projects to per-view META NDC centres")
{
	struct xrt_fov fovs[2] = {
	    {-0.8f, 0.7f, 0.65f, -0.60f},
	    {-0.7f, 0.8f, 0.60f, -0.65f},
	};
	struct xrt_foveation_state state
	{
	};
	struct xrt_vec3 forward
	{
		0.0f, 0.0f, -1.0f
	};

	REQUIRE(oxr_foveation_resolve_gaze_centres(&forward, fovs, 2, 0.0f, &state));
	CHECK(state.view_count == 2);
	CHECK(state.views[0].center_valid);
	CHECK(state.views[1].center_valid);

	// Asymmetric eye FOVs mean straight-ahead gaze need not be texture-centred.
	CHECK(state.views[0].center.x != Catch::Approx(0.0f));
	CHECK(state.views[1].center.x != Catch::Approx(0.0f));

	struct xrt_vec3 right_up
	{
		0.15f, 0.10f, -1.0f
	};
	REQUIRE(oxr_foveation_resolve_gaze_centres(&right_up, fovs, 2, 0.0f, &state));
	CHECK(state.views[0].center.x > -1.0f);
	CHECK(state.views[0].center.y > -1.0f);

	const float before_y = state.views[0].center.y;
	REQUIRE(oxr_foveation_resolve_gaze_centres(&right_up, fovs, 2, 5.0f, &state));
	CHECK(state.views[0].center.y > before_y);

	struct xrt_vec3 behind
	{
		0.0f, 0.0f, 1.0f
	};
	CHECK_FALSE(oxr_foveation_resolve_gaze_centres(&behind, fovs, 2, 0.0f, &state));
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#define main diagnostic_app_main_for_test
#include "../src/xrt/targets/psvr2_openxr_test/psvr2_openxr_test.mm"
#undef main
#include "catch_amalgamated.hpp"

namespace {
XrSpaceLocationFlags flags;
bool active;
bool locate_fails;
XrTime located_time;
XrSpace located_base;
uint32_t synced_sets;

application
mock_controller_app()
{
	application app;
	app.test_generic_controller = true;
	app.controller_hand_paths = {1, 2};
	app.app_space = reinterpret_cast<XrSpace>(10);
	app.controller_grip_spaces = {reinterpret_cast<XrSpace>(1), reinterpret_cast<XrSpace>(3)};
	app.controller_aim_spaces = {reinterpret_cast<XrSpace>(2), reinterpret_cast<XrSpace>(4)};
	app.xr.get_action_state_pose = [](XrSession, const XrActionStateGetInfo *, XrActionStatePose *out) {
		out->isActive = active ? XR_TRUE : XR_FALSE;
		return XR_SUCCESS;
	};
	app.xr.locate_space = [](XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *out) {
		located_time = time;
		located_base = base;
		out->locationFlags = flags;
		out->pose.position = {static_cast<float>(reinterpret_cast<uintptr_t>(space)), 0.2f, -0.4f};
		out->pose.orientation = {0, 0.70710678f, 0, 0.70710678f};
		return locate_fails ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
	};
	active = true;
	locate_fails = false;
	located_time = 0;
	flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
	        XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	return app;
}
} // namespace

TEST_CASE("Controller visuals locate both hands in app space at display time")
{
	auto app = mock_controller_app();
	append_controller_markers(app, 123456);
	REQUIRE(app.frame_instances.size() == 4);
	CHECK(located_time == 123456);
	CHECK(located_base == app.app_space);
	CHECK(app.frame_instances[0].model.columns[3].x == Catch::Approx(1));
	CHECK(app.frame_instances[2].model.columns[3].x == Catch::Approx(3));
	// A 90-degree Y rotation points aim along -X, not the unrotated world -Z.
	CHECK(app.frame_instances[1].model.columns[3].x == Catch::Approx(1.8));
	CHECK(app.frame_instances[1].model.columns[3].z == Catch::Approx(-0.4));
	CHECK(app.frame_instances[0].color.z > app.frame_instances[0].color.x);
	CHECK(app.frame_instances[2].color.x > app.frame_instances[2].color.z);
}

TEST_CASE("Controller visuals hide unusable poses and mark valid untracked poses grey")
{
	auto app = mock_controller_app();
	SECTION("inactive action")
	{
		active = false;
	}
	SECTION("orientation only")
	{
		flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	}
	SECTION("locate failure even with supplied valid flags")
	{
		locate_fails = true;
	}
	SECTION("missing action spaces")
	{
		app.controller_grip_spaces = {};
		app.controller_aim_spaces = {};
	}
	SECTION("valid but untracked")
	{
		flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		append_controller_markers(app, 789);
		REQUIRE(app.frame_instances.size() == 4);
		CHECK(app.frame_instances[0].color.x == app.frame_instances[0].color.y);
		CHECK(app.frame_instances[0].color.y == app.frame_instances[0].color.z);
		app.frame_instances.clear();
		flags = 0;
	}
	append_controller_markers(app, 790);
	CHECK(app.frame_instances.empty());
}

TEST_CASE("Controller and gaze action sets stay active together")
{
	auto app = mock_controller_app();
	app.controller_action_set = reinterpret_cast<XrActionSet>(1);
	app.gaze_action_set = reinterpret_cast<XrActionSet>(2);
	app.xr.sync_actions = [](XrSession, const XrActionsSyncInfo *info) {
		synced_sets = info->countActiveActionSets;
		CHECK(info->activeActionSets[0].actionSet == reinterpret_cast<XrActionSet>(1));
		CHECK(info->activeActionSets[1].actionSet == reinterpret_cast<XrActionSet>(2));
		return XR_SUCCESS;
	};
	CHECK(sync_diagnostic_actions(app) == XR_SUCCESS);
	CHECK(synced_sets == 2);
}

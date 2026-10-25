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

TEST_CASE("Controller input panels remain visible without controller poses")
{
	auto app = mock_controller_app();
	app.controller_pose_flags = {};
	app.controller_inputs[0].active = {true, true, true, true, true};
	app.controller_inputs[0].primary = true;
	app.controller_inputs[0].trigger = 0.75f;
	XrPosef head = {{0, 0, 0, 1}, {1, 2, 3}};
	append_controller_input_panels(app, head);
	REQUIRE_FALSE(app.frame_instances.empty());
	CHECK(app.frame_instances.size() < app.renderer.max_instances);
	CHECK(app.frame_instances[0].model.columns[3].x == Catch::Approx(0.82));
	CHECK(app.frame_instances[0].model.columns[3].y == Catch::Approx(1.80));
	CHECK(app.frame_instances[0].model.columns[3].z == Catch::Approx(2.18));
	bool green = false;
	bool red = false;
	for (const auto &instance : app.frame_instances) {
		green |= instance.color.y > 0.9f && instance.color.x < 0.2f;
		red |= instance.color.x > 0.9f && instance.color.y < 0.2f;
	}
	CHECK(green); // Active left button/trigger, despite no optical pose.
	CHECK(red);   // Inactive right actions and missing poses.
	app.frame_instances.clear();
	app.test_generic_controller = false;
	append_controller_input_panels(app, head);
	CHECK(app.frame_instances.empty());
}

TEST_CASE("Controller survey progresses through missing positions and pauses without consuming phases")
{
	controller_survey_state state;
	int64_t now = 1;
	CHECK(std::string(state.update(now, true, true, false, false)) == "started");
	for (int i = 0; i < 31; ++i)
		state.update(now += 100000000, true, false, false, false);
	CHECK(state.phase == controller_survey_phase::hold);
	const double elapsed = state.elapsed;
	state.update(now += 100000000, true, false, true, false);
	REQUIRE(state.paused);
	state.update(now += 100000000, true, false, false, false);
	for (int i = 0; i < 60; ++i)
		state.update(now += 100000000, true, false, false, false);
	CHECK(state.elapsed == elapsed);
	state.update(now += 100000000, true, false, true, false);
	CHECK_FALSE(state.paused);
	state.update(now += 100000000, true, false, false, false);
	const double before_focus_loss = state.elapsed;
	state.update(now += 2000000000, false, false, false, false);
	state.update(now += 2000000000, true, false, false, false);
	CHECK(state.elapsed == before_focus_loss);
	for (int i = 0; i < 51; ++i)
		state.update(now += 100000000, true, false, false, false);
	CHECK(state.phase == controller_survey_phase::recover);
	for (int i = 0; i < 51; ++i)
		state.update(now += 100000000, true, false, false, false);
	CHECK(state.phase == controller_survey_phase::ready);
	CHECK(state.trial == 1);
	// Holding a trigger cannot silently start the next trial.
	state.trigger_down = true;
	state.update(now += 100000000, true, true, false, false);
	CHECK(state.phase == controller_survey_phase::ready);
	for (size_t trial = 1; trial < 28; ++trial) {
		state.update(now += 100000000, true, false, false, true);
		state.update(now += 100000000, true, false, false, false);
	}
	CHECK(state.phase == controller_survey_phase::complete);
}

TEST_CASE("Survey captions and hidden-target guidance fit the existing instance buffer")
{
	auto app = mock_controller_app();
	app.session_state = XR_SESSION_STATE_FOCUSED;
	app.view_space = reinterpret_cast<XrSpace>(42);
	XrPosef head = {{0, 0, 0, 1}, {0, 1.6f, 0}};
	for (size_t trial = 0; trial < 28; ++trial) {
		for (auto phase : {controller_survey_phase::ready, controller_survey_phase::move,
		                   controller_survey_phase::hold, controller_survey_phase::recover}) {
			app.controller_survey.state.trial = trial;
			app.controller_survey.state.phase = phase;
			app.frame_instances.clear();
			append_controller_markers(app, 123);
			append_controller_survey(app, 123, head);
			CAPTURE(trial, phase, app.frame_instances.size());
			CHECK(app.frame_instances.size() > 100);
			CHECK(app.frame_instances.size() < app.renderer.max_instances);
			// The longest focus-wait caption and lost-position status must also fit.
			app.session_state = XR_SESSION_STATE_VISIBLE;
			app.controller_pose_flags = {};
			app.controller_samples = {};
			app.frame_instances.clear();
			append_controller_survey(app, 123, head);
			CHECK(app.frame_instances.size() < app.renderer.max_instances);
			app.session_state = XR_SESSION_STATE_FOCUSED;
		}
	}
}

TEST_CASE("Survey saves missing pose samples rather than substituting zeros or dropping frames")
{
	char temporary[] = "/tmp/monado-survey-test-XXXXXX";
	REQUIRE(mkdtemp(temporary) != nullptr);
	const auto directory = std::filesystem::path(temporary) / "capture";
	{
		auto app = mock_controller_app();
		app.controller_survey.open(directory.c_str());
		app.session_state = XR_SESSION_STATE_FOCUSED;
		app.view_space = reinterpret_cast<XrSpace>(42);
		app.xr.locate_space = [](XrSpace space, XrSpace, XrTime, XrSpaceLocation *out) {
			out->pose = {{0, 0, 0, 1}, {0, 1.6f, 0}};
			out->locationFlags =
			    space == reinterpret_cast<XrSpace>(42)
			        ? XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
			              XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT
			        : 0;
			return XR_SUCCESS;
		};
		app.controller_survey.state.phase = controller_survey_phase::hold;
		append_controller_markers(app, 999);
		append_controller_survey(app, 999, {{0, 0, 0, 1}, {0, 1.6f, 0}});
		CHECK(app.controller_survey.counts[0][0] == 1);
		CHECK(app.controller_survey.counts[0][2] == 0);
		REQUIRE(app.controller_survey.close());
		FILE *file = fopen((directory / "poses.csv").c_str(), "r");
		REQUIRE(file != nullptr);
		char header[2048], row[2048];
		REQUIRE(fgets(header, sizeof(header), file) != nullptr);
		size_t rows = 0;
		while (fgets(row, sizeof(row), file)) {
			CHECK(std::count(row, row + strlen(row), ',') ==
			      std::count(header, header + strlen(header), ','));
			CHECK(std::string(row).find(",nan,nan,nan,") != std::string::npos);
			++rows;
		}
		fclose(file);
		CHECK(rows == 4); // Both hands, grip and aim, including the lost selected hand.
	}
	controller_survey_capture duplicate;
	CHECK_THROWS(duplicate.open(directory.c_str()));
	std::filesystem::remove_all(temporary);
}

TEST_CASE("Survey reports summary write failure")
{
	char temporary[] = "/tmp/monado-survey-io-test-XXXXXX";
	REQUIRE(mkdtemp(temporary) != nullptr);
	{
		controller_survey_capture capture;
		const auto directory = std::filesystem::path(temporary) / "capture";
		capture.open(directory.c_str());
		std::filesystem::create_directory(directory / "summary.csv");
		CHECK_FALSE(capture.close());
	}
	std::filesystem::remove_all(temporary);
}

TEST_CASE("Survey preview and forward cue stay fixed while the wearer glances aside")
{
	auto app = mock_controller_app();
	app.session_state = XR_SESSION_STATE_FOCUSED;
	app.view_space = reinterpret_cast<XrSpace>(42);
	static XrPosef test_head;
	test_head = {{0, 0, 0, 1}, {0, 1.6f, 0}};
	app.xr.locate_space = [](XrSpace, XrSpace, XrTime, XrSpaceLocation *out) {
		out->pose = test_head;
		out->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
		                     XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
		return XR_SUCCESS;
	};
	app.controller_survey.state.trial = 5; // Left outside target.
	append_controller_survey(app, 1, test_head);
	const auto fixed_forward = app.survey_anchor.forward;
	const auto fixed_origin = app.survey_anchor.origin;
	const auto first_corner = app.frame_instances[0].model.columns[3];
	test_head.orientation = {0, .38268343f, 0, .92387953f}; // 45-degree glance.
	app.controller_inputs[0].active[2] = true;
	app.controller_inputs[0].trigger = 1;
	app.frame_instances.clear();
	append_controller_survey(app, 2, test_head);
	CHECK(simd_distance(app.survey_anchor.forward, fixed_forward) < .00001f);
	CHECK(simd_distance(app.survey_anchor.origin, fixed_origin) < .00001f);
	CHECK(simd_distance(app.frame_instances[0].model.columns[3], first_corner) < .00001f);
	CHECK(app.controller_survey.state.phase == controller_survey_phase::ready);
	// Returning to forward with a held trigger must not accidentally start.
	test_head.orientation = {0, 0, 0, 1};
	app.frame_instances.clear();
	append_controller_survey(app, 3, test_head);
	CHECK(app.controller_survey.state.phase == controller_survey_phase::ready);
	app.controller_inputs[0].trigger = 0;
	app.frame_instances.clear();
	append_controller_survey(app, 4, test_head);
	app.controller_inputs[0].trigger = 1;
	app.frame_instances.clear();
	append_controller_survey(app, 5, test_head);
	CHECK(app.controller_survey.state.phase == controller_survey_phase::move);
}

TEST_CASE("Survey can capture either hand alone while retaining stable trial IDs")
{
	char temporary[] = "/tmp/monado-survey-hand-test-XXXXXX";
	REQUIRE(mkdtemp(temporary) != nullptr);
	for (const char *hand : {"left", "right"}) {
		controller_survey_capture capture;
		const auto directory = std::filesystem::path(temporary) / hand;
		capture.open(directory.c_str(), hand, true);
		CHECK(capture.state.hand() == (std::string(hand) == "left" ? 0 : 1));
		for (int i = 0; i < 14; ++i)
			capture.state.next();
		CHECK(capture.state.phase == controller_survey_phase::complete);
		CHECK(capture.close());
	}
	std::filesystem::remove_all(temporary);
}

TEST_CASE("Survey reference reset preserves the trial and returns directly to readiness")
{
	controller_survey_state state;
	state.trial = 7;
	state.phase = controller_survey_phase::hold;
	state.elapsed = 3;
	state.paused = true;
	state.reset_reference();
	CHECK(state.trial == 7);
	CHECK(state.phase == controller_survey_phase::ready);
	CHECK_FALSE(state.paused);
	CHECK(state.elapsed == 0);
}

TEST_CASE("Survey keeps a solid hand target box visible when controller poses are lost")
{
	auto app = mock_controller_app();
	app.session_state = XR_SESSION_STATE_FOCUSED;
	app.view_space = reinterpret_cast<XrSpace>(42);
	active = false;
	append_controller_markers(app, 123);
	REQUIRE(app.frame_instances.empty());
	append_controller_survey(app, 123, {{0, 0, 0, 1}, {0, 1.6f, 0}});
	REQUIRE_FALSE(app.frame_instances.empty());
	CHECK(simd_length(app.frame_instances[0].model.columns[0]) == Catch::Approx(.045));
	CHECK(simd_length(app.frame_instances[0].model.columns[1]) == Catch::Approx(.045));
	CHECK(simd_length(app.frame_instances[0].model.columns[2]) == Catch::Approx(.045));
}

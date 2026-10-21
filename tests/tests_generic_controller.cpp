// Copyright 2026, Nick Kennedy.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Generic controller regressions against the upstream binding resolver.
 */

#include "catch_amalgamated.hpp"
#include "b_io_cache.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_space.h"
#include <oxr/actions/oxr_io_cache.h>
#include <oxr/actions/oxr_binding_profile.h>
#include <oxr/actions/oxr_binding_registry.h>
#include <oxr/actions/oxr_input_transform.h>
#include <oxr/actions/oxr_resolver_for_bindings.h>
#include <oxr/actions/oxr_session_action_context.h>
#include <oxr/oxr_logger.h>
#include <oxr/oxr_objects.h>
#include <oxr/path/oxr_path_store.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct GenericFixture
{
	struct oxr_logger log = {};
	struct oxr_path_store store = {};
	struct oxr_binding_registry *registry = nullptr;
	struct oxr_session_action_context context = {};
	struct xrt_system_devices system = {};
	struct xrt_space_overseer overseer = {};

	void
	refresh(struct xrt_device *dev)
	{
		oxr_io_cache_fini(&context.io_cache);
		system.static_xdev_count = 1;
		system.static_xdevs[0] = dev;
		system.static_roles.head = dev;
		system.static_roles.eyes = dev;
		system.get_roles = [](struct xrt_system_devices *, struct xrt_system_roles *roles) {
			*roles = XRT_SYSTEM_ROLES_INIT;
			roles->generation_id = 1;
			roles->left = 0;
			roles->right = 0;
			return XRT_SUCCESS;
		};
		system.create_io_cache = b_io_cache_create;
		if (dev->update_inputs == nullptr) {
			dev->update_inputs = [](struct xrt_device *) { return XRT_SUCCESS; };
		}
		REQUIRE(oxr_io_cache_init(&context.io_cache, &system, &overseer) == XR_SUCCESS);
	}

	GenericFixture()
	{
		oxr_log_init(&log, "generic-test");
		REQUIRE(oxr_path_store_init(&store) == XR_SUCCESS);
		REQUIRE(oxr_binding_registry_create(&log, &store, &registry) == XR_SUCCESS);
		REQUIRE(oxr_priority_group_cache_init(&context.priority_group_cache) == XR_SUCCESS);
	}
	~GenericFixture()
	{
		oxr_io_cache_fini(&context.io_cache);
		oxr_priority_group_cache_fini(&context.priority_group_cache);
		oxr_binding_registry_unref(registry);
		oxr_path_store_fini(&store);
	}
	XrPath
	path(const char *str)
	{
		XrPath value = XR_NULL_PATH;
		REQUIRE(oxr_path_store_get_or_create(&store, str, strlen(str), &value) == XR_SUCCESS);
		return value;
	}
	struct oxr_suggested_profile
	profile(const char *name)
	{
		const struct oxr_binding_profile *binding = nullptr;
		REQUIRE(oxr_binding_registry_find_profile(registry, path(name), &binding));
		struct oxr_suggested_profile result = {};
		result.path = binding->path;
		result.templ = binding->templ;
		return result;
	}
	struct oxr_profile_resolver_info
	resolve(struct xrt_device *dev, std::vector<struct oxr_suggested_profile *> profiles)
	{
		refresh(dev);
		struct oxr_suggested_profile_array array = {profiles.data(), profiles.size()};
		struct oxr_profile_resolver_info info = {};
		struct oxr_sink_logger slog = {};
		oxr_resolver_populate_profile(&log, &slog, registry, &array, &context.io_cache,
		                              &context.io_cache.info.devices[0], XRT_DEVICE_INVALID, &info);
		oxr_slog_cancel(&slog);
		return info;
	}
};

TEST_CASE("KHR generic controller fallback selection")
{
	GenericFixture f;
	auto generic = f.profile("/interaction_profiles/khr/generic_controller");
	auto simple = f.profile("/interaction_profiles/khr/simple_controller");
	auto index = f.profile("/interaction_profiles/valve/index_controller");
	auto touch = f.profile("/interaction_profiles/oculus/touch_controller");

	SECTION("native profiles win regardless of suggestion order")
	{
		struct xrt_device dev = {};
		dev.name = GENERATE(XRT_DEVICE_INDEX_CONTROLLER, XRT_DEVICE_TOUCH_CONTROLLER);
		auto *specific = dev.name == XRT_DEVICE_INDEX_CONTROLLER ? &index : &touch;
		CHECK(f.resolve(&dev, {&generic, specific}).profile == specific);
		CHECK(f.resolve(&dev, {specific, &generic}).profile == specific);
	}
	SECTION("generic mappings do not need driver fallback tables")
	{
		struct xrt_device dev = {};
		dev.name = GENERATE(XRT_DEVICE_INDEX_CONTROLLER, XRT_DEVICE_TOUCH_CONTROLLER,
		                    XRT_DEVICE_TOUCH_CONTROLLER_RIFT_CV1, XRT_DEVICE_TOUCH_CONTROLLER_QUEST_1_RIFT_S,
		                    XRT_DEVICE_TOUCH_CONTROLLER_QUEST_2, XRT_DEVICE_TOUCH_PRO_CONTROLLER,
		                    XRT_DEVICE_TOUCH_PLUS_CONTROLLER, XRT_DEVICE_PSSENSE);
		CAPTURE(dev.name);
		auto info = f.resolve(&dev, {&simple, &generic});
		CHECK(info.profile == &generic);
		CHECK(info.rebinding_profile != nullptr);
		CHECK(info.compatibility_profile == nullptr);
	}
	SECTION("Sense keeps more specific compatibility suggestions")
	{
		struct xrt_binding_profile fallbacks[2] = {};
		fallbacks[0].name = XRT_DEVICE_SIMPLE_CONTROLLER;
		fallbacks[1].name = GENERATE(XRT_DEVICE_INDEX_CONTROLLER, XRT_DEVICE_TOUCH_CONTROLLER);
		struct xrt_device dev = {};
		dev.name = XRT_DEVICE_PSSENSE;
		dev.binding_profiles = fallbacks;
		dev.binding_profile_count = 2;
		auto *specific = fallbacks[1].name == XRT_DEVICE_INDEX_CONTROLLER ? &index : &touch;
		CHECK(f.resolve(&dev, {&simple, &generic, specific}).profile == specific);
		CHECK(f.resolve(&dev, {specific, &generic, &simple}).profile == specific);
		CHECK(f.resolve(&dev, {&simple, &generic}).profile == &generic);
	}
	SECTION("Index compatibility remains opt-in")
	{
		struct xrt_device dev = {};
		dev.name = XRT_DEVICE_PSSENSE;
		CHECK(f.resolve(&dev, {&index}).profile == nullptr);
	}
	SECTION("unmapped hardware retains its simple fallback")
	{
		struct xrt_binding_profile fallback = {};
		fallback.name = XRT_DEVICE_SIMPLE_CONTROLLER;
		struct xrt_device dev = {};
		dev.name = XRT_DEVICE_PSMV;
		dev.binding_profiles = &fallback;
		dev.binding_profile_count = 1;
		CHECK(f.resolve(&dev, {&generic, &simple}).profile == &simple);
	}
}

TEST_CASE("Sense generic inputs resolve through generated rebindings")
{
	GenericFixture f;
	auto generic = f.profile("/interaction_profiles/khr/generic_controller");
	struct xrt_device dev = {};
	dev.name = XRT_DEVICE_PSSENSE;
	auto info = f.resolve(&dev, {&generic});
	REQUIRE(info.rebinding_profile != nullptr);

	const bool left = GENERATE(true, false);
	const char *component =
	    GENERATE("primary/click", "secondary/click", "squeeze/value", "trigger/value", "thumbstick/click",
	             "thumbstick/x", "grip/pose", "aim/pose", "grip_surface/pose");
	const bool palm = GENERATE(true, false);
	CAPTURE(left, component, palm);
	enum xrt_input_name expected = XRT_INPUT_NAME_INVALID;
	XrActionType action_type = XR_ACTION_TYPE_BOOLEAN_INPUT;
	if (strcmp(component, "primary/click") == 0) {
		expected = left ? XRT_INPUT_PSSENSE_SQUARE_CLICK : XRT_INPUT_PSSENSE_CROSS_CLICK;
	} else if (strcmp(component, "secondary/click") == 0) {
		expected = left ? XRT_INPUT_PSSENSE_TRIANGLE_CLICK : XRT_INPUT_PSSENSE_CIRCLE_CLICK;
	} else if (strcmp(component, "squeeze/value") == 0) {
		expected = XRT_INPUT_PSSENSE_SQUEEZE_CLICK;
		action_type = XR_ACTION_TYPE_FLOAT_INPUT;
	} else if (strcmp(component, "trigger/value") == 0) {
		expected = XRT_INPUT_PSSENSE_TRIGGER_VALUE;
		action_type = XR_ACTION_TYPE_FLOAT_INPUT;
	} else if (strcmp(component, "thumbstick/click") == 0) {
		expected = XRT_INPUT_PSSENSE_THUMBSTICK_CLICK;
	} else if (strcmp(component, "thumbstick/x") == 0) {
		expected = XRT_INPUT_PSSENSE_THUMBSTICK;
		action_type = XR_ACTION_TYPE_FLOAT_INPUT;
	} else {
		action_type = XR_ACTION_TYPE_POSE_INPUT;
		expected =
		    strcmp(component, "aim/pose") == 0 ? XRT_INPUT_PSSENSE_AIM_POSE : XRT_INPUT_PSSENSE_GRIP_POSE;
		if (strcmp(component, "grip_surface/pose") == 0 && palm) {
			expected = XRT_INPUT_GENERIC_PALM_POSE;
		}
	}
	struct xrt_input inputs[2] = {};
	inputs[0].name = expected;
	inputs[1].name = XRT_INPUT_PSSENSE_GRIP_POSE;
	dev.inputs = inputs;
	dev.input_count = 2;
	char str[128];
	snprintf(str, sizeof(str), "/user/hand/%s/input/%s", left ? "left" : "right", component);
	const struct oxr_binding_point *point = nullptr;
	REQUIRE(oxr_binding_profile_find_binding_point(info.binding_profile, f.path(str), &point));
	struct oxr_suggested_binding binding = {1, point->path, point};
	generic.bindings = &binding;
	generic.binding_count = 1;
	struct oxr_action_ref ref = {};
	ref.action_type = action_type;
	struct oxr_action_set_attachment set = {};
	set.sess_context = &f.context;
	struct oxr_action_attachment action = {};
	action.act_ref = &ref;
	action.act_key = 1;
	action.act_set_attached = &set;
	struct oxr_action_cache cache = {};
	struct oxr_sink_logger slog = {};
	f.refresh(&dev);
	oxr_resolver_populate_input(&f.log, &slog, &f.context.io_cache, &info, &f.context.io_cache.info.devices[0],
	                            f.path(left ? "/user/hand/left" : "/user/hand/right"), &action, &cache);
	oxr_slog_cancel(&slog);
	REQUIRE(cache.input_count == 1);
	CHECK(cache.inputs[0].input->name == expected);
	if (strcmp(component, "squeeze/value") == 0) {
		for (bool pressed : {false, true}) {
			struct oxr_input_value_tagged raw = {};
			struct oxr_input_value_tagged converted = {};
			raw.type = XRT_INPUT_TYPE_BOOLEAN;
			raw.value.boolean = pressed;
			CHECK(oxr_input_transform_process(cache.inputs[0].transforms, cache.inputs[0].transform_count,
			                                  &raw, &converted));
			CHECK(converted.value.vec1.x == (pressed ? 1.0f : 0.0f));
		}
	}
	oxr_input_transform_destroy(&cache.inputs[0].transforms);
	free(cache.inputs);
}

TEST_CASE("Sense generic haptics resolve on both hands")
{
	GenericFixture f;
	auto generic = f.profile("/interaction_profiles/khr/generic_controller");
	struct xrt_device dev = {};
	dev.name = XRT_DEVICE_PSSENSE;
	struct xrt_output output = {};
	output.name = XRT_OUTPUT_NAME_PSSENSE_VIBRATION;
	dev.outputs = &output;
	dev.output_count = 1;
	auto info = f.resolve(&dev, {&generic});
	const char *hand = GENERATE("left", "right");
	char str[128];
	snprintf(str, sizeof(str), "/user/hand/%s/output/haptic", hand);
	const struct oxr_binding_point *point = nullptr;
	REQUIRE(oxr_binding_profile_find_binding_point(info.binding_profile, f.path(str), &point));
	struct oxr_suggested_binding binding = {1, point->path, point};
	generic.bindings = &binding;
	generic.binding_count = 1;
	struct oxr_action_ref ref = {};
	ref.action_type = XR_ACTION_TYPE_VIBRATION_OUTPUT;
	struct oxr_action_attachment action = {};
	action.act_ref = &ref;
	action.act_key = 1;
	struct oxr_action_cache cache = {};
	struct oxr_sink_logger slog = {};
	snprintf(str, sizeof(str), "/user/hand/%s", hand);
	oxr_resolver_populate_output(&f.log, &slog, &f.context.io_cache, &info, &f.context.io_cache.info.devices[0],
	                             f.path(str), &action, &cache);
	oxr_slog_cancel(&slog);
	REQUIRE(cache.output_count == 1);
	CHECK(cache.outputs[0].name == XRT_OUTPUT_NAME_PSSENSE_VIBRATION);
	free(cache.outputs);
}

TEST_CASE("Unconverted profiles resolve native cached inputs without changing their types")
{
	struct Mapping
	{
		xrt_device_name device;
		xrt_device_name profile;
		const char *profile_path;
		const char *user_path;
		const char *component;
		xrt_input_name source;
		xrt_input_name native;
		XrActionType type;
	};
	const Mapping mappings[] = {
	    {XRT_DEVICE_HAND_CTRL_EMU, XRT_DEVICE_SIMPLE_CONTROLLER, "/interaction_profiles/khr/simple_controller",
	     "/user/hand/left", "/input/select/click", XRT_INPUT_SIMPLE_SELECT_CLICK,
	     XRT_INPUT_HAND_CTRL_EMU_PINCH_VALUE, XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {XRT_DEVICE_HAND_CTRL_EMU, XRT_DEVICE_SIMPLE_CONTROLLER, "/interaction_profiles/khr/simple_controller",
	     "/user/hand/right", "/input/grip/pose", XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_HAND_CTRL_EMU_GRIP_POSE,
	     XR_ACTION_TYPE_POSE_INPUT},
	    {XRT_DEVICE_VIVE_PRO2, XRT_DEVICE_VIVE_PRO, "/interaction_profiles/htc/vive_pro", "/user/head",
	     "/input/system/click", XRT_INPUT_VIVE_PRO_SYSTEM_CLICK, XRT_INPUT_VIVE_PRO_SYSTEM_CLICK,
	     XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {XRT_DEVICE_PSVR2, XRT_DEVICE_EYE_GAZE_INTERACTION, "/interaction_profiles/ext/eye_gaze_interaction",
	     "/user/eyes_ext", "/input/gaze_ext/pose", XRT_INPUT_GENERIC_EYE_GAZE_POSE, XRT_INPUT_GENERIC_EYE_GAZE_POSE,
	     XR_ACTION_TYPE_POSE_INPUT},
	    {XRT_DEVICE_PSSENSE, XRT_DEVICE_INDEX_CONTROLLER, "/interaction_profiles/valve/index_controller",
	     "/user/hand/left", "/input/a/click", XRT_INPUT_INDEX_A_CLICK, XRT_INPUT_PSSENSE_SQUARE_CLICK,
	     XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {XRT_DEVICE_PSSENSE, XRT_DEVICE_INDEX_CONTROLLER, "/interaction_profiles/valve/index_controller",
	     "/user/hand/right", "/input/a/click", XRT_INPUT_INDEX_A_CLICK, XRT_INPUT_PSSENSE_CROSS_CLICK,
	     XR_ACTION_TYPE_BOOLEAN_INPUT},
	    {XRT_DEVICE_PSSENSE, XRT_DEVICE_INDEX_CONTROLLER, "/interaction_profiles/valve/index_controller",
	     "/user/hand/right", "/input/squeeze/value", XRT_INPUT_INDEX_SQUEEZE_VALUE,
	     XRT_INPUT_PSSENSE_SQUEEZE_PROXIMITY_FLOAT, XR_ACTION_TYPE_FLOAT_INPUT},
	};
	const int index = GENERATE(0, 1, 2, 3, 4, 5, 6);
	const auto &mapping = mappings[index];
	CAPTURE(index);
	GenericFixture f;
	auto profile = f.profile(mapping.profile_path);
	xrt_device dev = {};
	dev.name = mapping.device;
	xrt_input native = {};
	native.name = mapping.native;
	native.active = true;
	native.timestamp = 987;
	dev.inputs = &native;
	dev.input_count = 1;
	xrt_binding_input_pair pair = {mapping.source, mapping.native};
	xrt_binding_profile fallback = {};
	fallback.name = mapping.profile;
	fallback.inputs = &pair;
	fallback.input_count = 1;
	dev.binding_profiles = &fallback;
	dev.binding_profile_count = 1;
	auto info = f.resolve(&dev, {&profile});
	REQUIRE(info.profile == &profile);
	REQUIRE(info.compatibility_profile != nullptr);
	char str[160];
	snprintf(str, sizeof(str), "%s%s", mapping.user_path, mapping.component);
	const oxr_binding_point *point = nullptr;
	REQUIRE(oxr_binding_profile_find_binding_point(info.binding_profile, f.path(str), &point));
	oxr_suggested_binding binding = {1, point->path, point};
	profile.bindings = &binding;
	profile.binding_count = 1;
	oxr_action_ref ref = {};
	ref.action_type = mapping.type;
	oxr_action_set_attachment set = {};
	set.sess_context = &f.context;
	oxr_action_attachment action = {};
	action.act_ref = &ref;
	action.act_key = 1;
	action.act_set_attached = &set;
	oxr_action_cache cache = {};
	oxr_sink_logger slog = {};
	oxr_resolver_populate_input(&f.log, &slog, &f.context.io_cache, &info, &f.context.io_cache.info.devices[0],
	                            f.path(mapping.user_path), &action, &cache);
	oxr_slog_cancel(&slog);
	REQUIRE(cache.input_count == 1);
	CHECK(cache.inputs[0].input->name == mapping.native);
	CHECK(cache.inputs[0].input->timestamp == 987);
	if (index == 0) {
		for (float sample : {0.25f, 0.75f}) {
			oxr_input_value_tagged raw = {};
			raw.type = XRT_INPUT_TYPE_VEC1_ZERO_TO_ONE;
			raw.value.vec1.x = sample;
			oxr_input_value_tagged result = {};
			REQUIRE(oxr_input_transform_process(cache.inputs[0].transforms, cache.inputs[0].transform_count,
			                                    &raw, &result));
			CHECK(result.value.boolean == (sample > 0.5f));
		}
	}
	oxr_input_transform_destroy(&cache.inputs[0].transforms);
	free(cache.inputs);
}

TEST_CASE("Sense Index haptics use the service-selected compatibility mapping")
{
	GenericFixture f;
	auto index = f.profile("/interaction_profiles/valve/index_controller");
	xrt_device dev = {};
	dev.name = XRT_DEVICE_PSSENSE;
	xrt_output native = {};
	native.name = XRT_OUTPUT_NAME_PSSENSE_VIBRATION;
	dev.outputs = &native;
	dev.output_count = 1;
	xrt_binding_output_pair pair = {XRT_OUTPUT_NAME_INDEX_HAPTIC, XRT_OUTPUT_NAME_PSSENSE_VIBRATION};
	xrt_binding_profile fallback = {};
	fallback.name = XRT_DEVICE_INDEX_CONTROLLER;
	fallback.outputs = &pair;
	fallback.output_count = 1;
	const bool enabled = GENERATE(false, true);
	dev.binding_profiles = &fallback;
	dev.binding_profile_count = enabled ? 1 : 0;
	auto info = f.resolve(&dev, {&index});
	if (!enabled) {
		CHECK(info.profile == nullptr);
		return;
	}
	REQUIRE(info.compatibility_profile != nullptr);
	const char *hand = GENERATE("left", "right");
	char str[128];
	snprintf(str, sizeof(str), "/user/hand/%s/output/haptic", hand);
	const oxr_binding_point *point = nullptr;
	REQUIRE(oxr_binding_profile_find_binding_point(info.binding_profile, f.path(str), &point));
	oxr_suggested_binding binding = {1, point->path, point};
	index.bindings = &binding;
	index.binding_count = 1;
	oxr_action_ref ref = {};
	ref.action_type = XR_ACTION_TYPE_VIBRATION_OUTPUT;
	oxr_action_attachment action = {};
	action.act_key = 1;
	action.act_ref = &ref;
	oxr_action_cache cache = {};
	oxr_sink_logger slog = {};
	snprintf(str, sizeof(str), "/user/hand/%s", hand);
	oxr_resolver_populate_output(&f.log, &slog, &f.context.io_cache, &info, &f.context.io_cache.info.devices[0],
	                             f.path(str), &action, &cache);
	oxr_slog_cancel(&slog);
	REQUIRE(cache.output_count == 1);
	CHECK(cache.outputs[0].name == XRT_OUTPUT_NAME_PSSENSE_VIBRATION);
	free(cache.outputs);
}

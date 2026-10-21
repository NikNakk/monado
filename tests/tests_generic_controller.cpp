// Copyright 2026, Nick Kennedy.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Generic controller regressions against the upstream binding resolver.
 */

#include "catch_amalgamated.hpp"
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

	GenericFixture()
	{
		oxr_log_init(&log, "generic-test");
		REQUIRE(oxr_path_store_init(&store) == XR_SUCCESS);
		REQUIRE(oxr_binding_registry_create(&log, &store, &registry) == XR_SUCCESS);
		REQUIRE(oxr_priority_group_cache_init(&context.priority_group_cache) == XR_SUCCESS);
	}
	~GenericFixture()
	{
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
		struct oxr_suggested_profile_array array = {profiles.data(), profiles.size()};
		struct oxr_profile_resolver_info info = {};
		struct oxr_sink_logger slog = {};
		oxr_resolver_populate_profile(&log, &slog, registry, &array, dev, XRT_DEVICE_INVALID, &info);
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
		CHECK(info.xbp == nullptr);
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
	oxr_resolver_populate_input(&f.log, &slog, &info, &dev, f.path(left ? "/user/hand/left" : "/user/hand/right"),
	                            &action, &cache);
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
	oxr_resolver_populate_output(&f.log, &slog, &info, &dev, f.path(str), &action, &cache);
	oxr_slog_cancel(&slog);
	REQUIRE(cache.output_count == 1);
	CHECK(cache.outputs[0].name == XRT_OUTPUT_NAME_PSSENSE_VIBRATION);
	free(cache.outputs);
}

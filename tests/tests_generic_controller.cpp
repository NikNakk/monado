// Copyright 2026, Nick Kennedy.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief XR_KHR_generic_controller interaction-profile fallback tests.
 */

#include "catch_amalgamated.hpp"
#include "util/u_misc.h"

#include <cstring>

#include <xrt/xrt_device.h>

#include <oxr/actions/oxr_interaction_profile_array.h>
#include <oxr/oxr_objects.h>
#include <oxr_generated_bindings.h>

using Catch::Generators::values;


static void
initialize_template_paths(struct oxr_instance_path_cache *cache)
{
	for (size_t i = 0; i < OXR_BINDINGS_PROFILE_TEMPLATE_COUNT; i++) {
		// Tests only need stable, distinct non-null path handles.
		cache->template_paths[i] = (XrPath)(i + 1);
	}
}

static XrPath
template_path(const struct oxr_instance_path_cache *cache, const char *path)
{
	for (size_t i = 0; i < OXR_BINDINGS_PROFILE_TEMPLATE_COUNT; i++) {
		if (strcmp(profile_templates[i].path, path) == 0) {
			return cache->template_paths[i];
		}
	}

	FAIL("interaction profile template not found: " << path);
	return XR_NULL_PATH;
}

static struct oxr_interaction_profile
make_profile(const struct oxr_instance_path_cache *cache, const char *path, enum xrt_device_name name)
{
	struct oxr_interaction_profile profile = {};
	profile.path = template_path(cache, path);
	profile.xname = name;
	return profile;
}

TEST_CASE("KHR generic controller fallback selection")
{
	struct oxr_instance_path_cache cache = {};
	initialize_template_paths(&cache);

	struct oxr_interaction_profile generic =
	    make_profile(&cache, "/interaction_profiles/khr/generic_controller", XRT_DEVICE_GENERIC_CONTROLLER);
	struct oxr_interaction_profile simple =
	    make_profile(&cache, "/interaction_profiles/khr/simple_controller", XRT_DEVICE_SIMPLE_CONTROLLER);
	struct oxr_interaction_profile index =
	    make_profile(&cache, "/interaction_profiles/valve/index_controller", XRT_DEVICE_INDEX_CONTROLLER);
	struct oxr_interaction_profile touch =
	    make_profile(&cache, "/interaction_profiles/oculus/touch_controller", XRT_DEVICE_TOUCH_CONTROLLER);

	SECTION("Index hardware prefers specific bindings")
	{
		struct xrt_device xdev = {};
		xdev.name = XRT_DEVICE_INDEX_CONTROLLER;

		struct oxr_interaction_profile *profiles[] = {&generic, &index};
		struct oxr_interaction_profile_array array = {profiles, ARRAY_SIZE(profiles)};

		struct oxr_interaction_profile *selected = nullptr;
		CHECK(oxr_interaction_profile_array_find_by_device(&array, &cache, &xdev, &selected));
		CHECK(selected == &index);
	}

	SECTION("Index hardware falls back to generic")
	{
		struct xrt_device xdev = {};
		xdev.name = XRT_DEVICE_INDEX_CONTROLLER;

		struct oxr_interaction_profile *profiles[] = {&generic};
		struct oxr_interaction_profile_array array = {profiles, ARRAY_SIZE(profiles)};

		struct oxr_interaction_profile *selected = nullptr;
		CHECK(oxr_interaction_profile_array_find_by_device(&array, &cache, &xdev, &selected));
		CHECK(selected == &generic);
	}

	SECTION("Touch hardware prefers specific bindings")
	{
		struct xrt_device xdev = {};
		xdev.name = XRT_DEVICE_TOUCH_CONTROLLER;

		struct oxr_interaction_profile *profiles[] = {&generic, &touch};
		struct oxr_interaction_profile_array array = {profiles, ARRAY_SIZE(profiles)};

		struct oxr_interaction_profile *selected = nullptr;
		CHECK(oxr_interaction_profile_array_find_by_device(&array, &cache, &xdev, &selected));
		CHECK(selected == &touch);
	}

	SECTION("Touch hardware falls back to generic")
	{
		struct xrt_device xdev = {};
		xdev.name = XRT_DEVICE_TOUCH_CONTROLLER;

		struct oxr_interaction_profile *profiles[] = {&generic};
		struct oxr_interaction_profile_array array = {profiles, ARRAY_SIZE(profiles)};

		struct oxr_interaction_profile *selected = nullptr;
		CHECK(oxr_interaction_profile_array_find_by_device(&array, &cache, &xdev, &selected));
		CHECK(selected == &generic);
	}


	SECTION("promoted Touch variants fall back to generic")
	{
		auto name = GENERATE(values({
		    XRT_DEVICE_TOUCH_CONTROLLER_RIFT_CV1,
		    XRT_DEVICE_TOUCH_CONTROLLER_QUEST_1_RIFT_S,
		    XRT_DEVICE_TOUCH_CONTROLLER_QUEST_2,
		}));
		CAPTURE(name);

		struct xrt_device xdev = {};
		xdev.name = name;

		struct oxr_interaction_profile *profiles[] = {&generic};
		struct oxr_interaction_profile_array array = {profiles, ARRAY_SIZE(profiles)};

		struct oxr_interaction_profile *selected = nullptr;
		CHECK(oxr_interaction_profile_array_find_by_device(&array, &cache, &xdev, &selected));
		CHECK(selected == &generic);
	}

	SECTION("newer Touch variants fall back to generic")
	{
		auto name = GENERATE(values({
		    XRT_DEVICE_TOUCH_PRO_CONTROLLER,
		    XRT_DEVICE_TOUCH_PLUS_CONTROLLER,
		}));
		CAPTURE(name);

		struct xrt_device xdev = {};
		xdev.name = name;

		struct oxr_interaction_profile *profiles[] = {&generic};
		struct oxr_interaction_profile_array array = {profiles, ARRAY_SIZE(profiles)};

		struct oxr_interaction_profile *selected = nullptr;
		CHECK(oxr_interaction_profile_array_find_by_device(&array, &cache, &xdev, &selected));
		CHECK(selected == &generic);
	}

	SECTION("generic-capable compatibility mapping prefers generic over simple")
	{
		struct xrt_binding_profile fallbacks[2] = {};
		fallbacks[0].name = XRT_DEVICE_SIMPLE_CONTROLLER;
		fallbacks[1].name = XRT_DEVICE_INDEX_CONTROLLER;

		struct xrt_device xdev = {};
		xdev.name = XRT_DEVICE_PSSENSE;
		xdev.binding_profiles = fallbacks;
		xdev.binding_profile_count = ARRAY_SIZE(fallbacks);

		struct oxr_interaction_profile *profiles[] = {&simple, &generic};
		struct oxr_interaction_profile_array array = {profiles, ARRAY_SIZE(profiles)};

		struct oxr_interaction_profile *selected = nullptr;
		CHECK(oxr_interaction_profile_array_find_by_device(&array, &cache, &xdev, &selected));
		CHECK(selected == &generic);
	}

	SECTION("explicit Index compatibility still wins over generic")
	{
		struct xrt_binding_profile fallbacks[2] = {};
		fallbacks[0].name = XRT_DEVICE_SIMPLE_CONTROLLER;
		fallbacks[1].name = XRT_DEVICE_INDEX_CONTROLLER;

		struct xrt_device xdev = {};
		xdev.name = XRT_DEVICE_PSSENSE;
		xdev.binding_profiles = fallbacks;
		xdev.binding_profile_count = ARRAY_SIZE(fallbacks);

		struct oxr_interaction_profile *profiles[] = {&simple, &generic, &index};
		struct oxr_interaction_profile_array array = {profiles, ARRAY_SIZE(profiles)};

		struct oxr_interaction_profile *selected = nullptr;
		CHECK(oxr_interaction_profile_array_find_by_device(&array, &cache, &xdev, &selected));
		CHECK(selected == &index);
	}

	SECTION("unrelated devices retain simple fallback")
	{
		struct xrt_binding_profile fallbacks[1] = {};
		fallbacks[0].name = XRT_DEVICE_SIMPLE_CONTROLLER;

		struct xrt_device xdev = {};
		xdev.name = XRT_DEVICE_PSSENSE;
		xdev.binding_profiles = fallbacks;
		xdev.binding_profile_count = ARRAY_SIZE(fallbacks);

		struct oxr_interaction_profile *profiles[] = {&simple, &generic};
		struct oxr_interaction_profile_array array = {profiles, ARRAY_SIZE(profiles)};

		struct oxr_interaction_profile *selected = nullptr;
		CHECK(oxr_interaction_profile_array_find_by_device(&array, &cache, &xdev, &selected));
		CHECK(selected == &simple);
	}
}

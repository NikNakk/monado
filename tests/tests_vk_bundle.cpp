// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Vulkan bundle cleanup tests that do not need a GPU.
 */

#include "catch_amalgamated.hpp"
#include "vk/vk_helpers.h"

TEST_CASE("Vulkan queue mutex cleanup handles incomplete initialization", "[vulkan]")
{
	vk_bundle vk{};

	SECTION("No mutexes initialized")
	{
		REQUIRE(vk_deinit_mutex(&vk) == VK_SUCCESS);
	}

	SECTION("All mutexes initialized")
	{
		REQUIRE(vk_init_mutex(&vk) == VK_SUCCESS);
		REQUIRE(vk.initialized_queue_mutex_count == ARRAY_SIZE(vk.queues));
		REQUIRE(vk_deinit_mutex(&vk) == VK_SUCCESS);
	}

	SECTION("Only the first mutex initialized")
	{
		REQUIRE(os_mutex_init(&vk.queues[0].mutex) == 0);
		vk.initialized_queue_mutex_count = 1;
		REQUIRE(vk_deinit_mutex(&vk) == VK_SUCCESS);
	}

	REQUIRE(vk.initialized_queue_mutex_count == 0);
	REQUIRE(vk_deinit_mutex(&vk) == VK_SUCCESS);
}

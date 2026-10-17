// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "../src/xrt/compositor/main/comp_macos_display_timing.h"
#include "util/u_pacing.h"
#include <limits>

TEST_CASE("display phase projects over missed callbacks and exact boundaries", "[macos][timing]")
{
	CHECK(comp_macos_last_vblank_ns(110, 105, 10) == 100);
	CHECK(comp_macos_last_vblank_ns(140, 105, 10) == 100);
	CHECK(comp_macos_last_vblank_ns(110, 110, 10) == 110);
	CHECK(comp_macos_last_vblank_ns(80, 105, 10) == 100);
	CHECK(comp_macos_last_vblank_ns(5, 1, 10) == 0);
	CHECK(comp_macos_last_vblank_ns(0, 105, 10) == 105);
	CHECK(comp_macos_last_vblank_ns(110, 105, 0) == 105);
	const auto max = std::numeric_limits<uint64_t>::max();
	CHECK(comp_macos_last_vblank_ns(max, max - 5, 10) == max - 10);
}

TEST_CASE("refresh updates preserve compositor frame IDs and outstanding frames", "[macos][timing]")
{
	for (bool feedback : {false, true}) {
		struct u_pacing_compositor *pc = nullptr;
		struct u_pc_display_timing_config config = u_pc_display_timing_get_default_config();
		config.comp_time_fraction = 22;
		config.comp_time_max_fraction = 65;
		config.present_to_display_offset_ns = 12000000;
		config.margin_ns = 2300000;
		REQUIRE((feedback ? u_pc_display_timing_create(11111111, &config, &pc)
		                  : u_pc_fake_create(11111111, 1000000000, &pc)) == XRT_SUCCESS);
		int64_t id, wake, desired, slop, display, period, minimum;
		u_pc_predict(pc, 1000000000, &id, &wake, &desired, &slop, &display, &period, &minimum);
		CHECK(period == 11111111);
		int64_t first_id = id;
		int64_t original_budget = desired - wake;
		int64_t original_phase = desired - period;
		if (!feedback) {
			u_pc_update_present_offset(pc, first_id, 25000000);
		}
		REQUIRE(u_pc_update_period(pc, 8333333, 1001000000));
		u_pc_info_gpu(pc, first_id, 1000000000, 1000100000, 1001000000);
		u_pc_predict(pc, 1002000000, &id, &wake, &desired, &slop, &display, &period, &minimum);
		CHECK(id > first_id);
		CHECK(period == 8333333);
		CHECK(desired - wake == original_budget);
		if (feedback) {
			CHECK(display - desired == config.present_to_display_offset_ns);
		}
		if (!feedback) {
			CHECK(desired == original_phase + period);
			CHECK(display - desired == 25000000);
		}
		CHECK_FALSE(u_pc_update_period(pc, 0, 1003000000));
		CHECK_FALSE(u_pc_update_period(pc, -1, 1003000000));
		REQUIRE(u_pc_update_period(pc, 11111111, 1003000000));
		first_id = id;
		u_pc_predict(pc, 1004000000, &id, &wake, &desired, &slop, &display, &period, &minimum);
		CHECK(id > first_id);
		CHECK(period == 11111111);
		u_pc_destroy(&pc);
	}
}

#ifndef _WIN32
#include "../src/xrt/compositor/main/comp_macos_refresh.h"
#include <thread>
#include <chrono>

static xrt_result_t
apply_success(void *, float)
{
	return XRT_SUCCESS;
}

static bool
wait_pending(struct comp_macos_refresh_queue &q)
{
	auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < limit) {
		pthread_mutex_lock(&q.mutex);
		bool pending = q.pending;
		pthread_mutex_unlock(&q.mutex);
		if (pending) {
			return true;
		}
		std::this_thread::yield();
	}
	return false;
}

TEST_CASE("refresh requests apply only on the processing thread", "[macos][refresh]")
{
	struct comp_macos_refresh_queue q = {};
	REQUIRE(comp_macos_refresh_init(&q));
	comp_macos_refresh_set_active(&q, true, apply_success, nullptr);
	xrt_result_t result = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	std::jthread caller([&] { result = comp_macos_refresh_request(&q, 120, 2000000000, apply_success, nullptr); });
	REQUIRE(wait_pending(q));
	CHECK(comp_macos_refresh_request(&q, 90, 1000000, apply_success, nullptr) == XRT_ERROR_OUTPUT_REQUEST_FAILURE);
	struct Call
	{
		float hz;
		std::thread::id thread;
	} call = {};
	comp_macos_refresh_process(
	    &q,
	    [](void *ctx, float hz) {
		    auto *call = static_cast<Call *>(ctx);
		    call->hz = hz;
		    call->thread = std::this_thread::get_id();
		    return XRT_SUCCESS;
	    },
	    &call);
	caller.join();
	CHECK(result == XRT_SUCCESS);
	CHECK(call.hz == 120);
	CHECK(call.thread == std::this_thread::get_id());
	CHECK(comp_macos_refresh_request(&q, 0, 1000000, apply_success, nullptr) == XRT_ERROR_INVALID_ARGUMENT);
	CHECK(comp_macos_refresh_request(&q, 90, 1000000, apply_success, nullptr) == XRT_ERROR_OUTPUT_REQUEST_FAILURE);
	CHECK_FALSE(q.pending);
	comp_macos_refresh_fini(&q);
}

TEST_CASE("refresh shutdown cancels and joins outstanding API waiters", "[macos][refresh]")
{
	struct comp_macos_refresh_queue q = {};
	REQUIRE(comp_macos_refresh_init(&q));
	comp_macos_refresh_set_active(&q, true, apply_success, nullptr);
	xrt_result_t result = XRT_SUCCESS;
	std::jthread caller([&] { result = comp_macos_refresh_request(&q, 90, 2000000000, apply_success, nullptr); });
	REQUIRE(wait_pending(q));
	comp_macos_refresh_fini(&q);
	caller.join();
	CHECK(result == XRT_ERROR_OUTPUT_REQUEST_FAILURE);
}
TEST_CASE("idle refresh requests and session stop apply without a rendering loop", "[macos][refresh]")
{
	struct comp_macos_refresh_queue q = {};
	REQUIRE(comp_macos_refresh_init(&q));
	float applied = 0;
	auto apply = [](void *ctx, float hz) {
		*static_cast<float *>(ctx) = hz;
		return XRT_SUCCESS;
	};
	CHECK(comp_macos_refresh_request(&q, 120, 1000000, apply, &applied) == XRT_SUCCESS);
	CHECK(applied == 120);
	comp_macos_refresh_set_active(&q, true, apply, &applied);
	xrt_result_t result = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	std::jthread caller([&] { result = comp_macos_refresh_request(&q, 90, 2000000000, apply, &applied); });
	REQUIRE(wait_pending(q));
	comp_macos_refresh_set_active(&q, false, apply, &applied);
	caller.join();
	CHECK(result == XRT_SUCCESS);
	CHECK(applied == 90);
	CHECK(comp_macos_refresh_request(&q, 120, 1000000, apply, &applied) == XRT_SUCCESS);
	comp_macos_refresh_fini(&q);
}

TEST_CASE("refresh completion survives a timed wait expiring while apply holds the lock", "[macos][refresh]")
{
	struct comp_macos_refresh_queue q = {};
	REQUIRE(comp_macos_refresh_init(&q));
	comp_macos_refresh_set_active(&q, true, apply_success, nullptr);
	xrt_result_t result = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	std::jthread caller([&] { result = comp_macos_refresh_request(&q, 120, 100000000, apply_success, nullptr); });
	REQUIRE(wait_pending(q));
	comp_macos_refresh_process(
	    &q,
	    [](void *, float) {
		    std::this_thread::sleep_for(std::chrono::milliseconds(150));
		    return XRT_SUCCESS;
	    },
	    nullptr);
	caller.join();
	CHECK(result == XRT_SUCCESS);
	comp_macos_refresh_fini(&q);
}

#endif

TEST_CASE("clock continuity checks reject jumps but ignore preempted samples", "[macos][timing]")
{
	CHECK_FALSE(comp_macos_clock_bridge_changed(1000000000, 900000000, 1000001000, 100000500));
	CHECK(comp_macos_clock_bridge_changed(1000000000, 890000000, 1000001000, 100000500));
	CHECK_FALSE(comp_macos_clock_bridge_changed(1000000000, 900000000, 1100000000, 100000500));
	CHECK_FALSE(comp_macos_clock_bridge_changed(1000000000, 900000000, 999999999, 100000500));
}

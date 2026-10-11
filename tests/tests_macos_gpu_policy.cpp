// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "multi/comp_multi_private.h"
#include "util/u_time.h"

TEST_CASE("late completed frame retains its slot until a later readiness cutoff")
{
	struct multi_system_compositor system{};
	xrt_compositor_native native{};
	os_mutex_init(&system.list_and_timing_lock);
	os_thread_helper_init(&system.oth);
	u_pa_factory_create(&system.upaf);
	system.xcn = &native;
	system.last_timings.predicted_display_time_ns = os_monotonic_get_ns();
	system.last_timings.predicted_display_period_ns = 16000000;
	system.last_timings.diff_ns = 5000000;
	xrt_session_info info{};
	xrt_compositor_native *client = nullptr;
	REQUIRE(multi_compositor_create(&system, &info, nullptr, &client) == XRT_SUCCESS);
	auto *mc = multi_compositor(&client->base);
	int64_t frame, wake, gpu, display, period;
	REQUIRE(xrt_comp_predict_frame(&client->base, &frame, &wake, &gpu, &display, &period) == XRT_SUCCESS);
	REQUIRE(xrt_comp_mark_frame(&client->base, frame, XRT_COMPOSITOR_FRAME_POINT_WOKE, os_monotonic_get_ns()) == XRT_SUCCESS);
	REQUIRE(xrt_comp_begin_frame(&client->base, frame) == XRT_SUCCESS);
	xrt_layer_frame_data data{};
	data.frame_id = frame;
	int64_t now = os_monotonic_get_ns();
	data.display_time_ns = now;
	REQUIRE(xrt_comp_layer_begin(&client->base, &data) == XRT_SUCCESS);
	REQUIRE(xrt_comp_layer_commit(&client->base, XRT_GRAPHICS_SYNC_HANDLE_INVALID) == XRT_SUCCESS);
	os_mutex_lock(&mc->slot_lock);
	mc->scheduled.gpu_ready_ns = now + 10 * U_TIME_1MS_IN_NS;
	os_mutex_unlock(&mc->slot_lock);
	multi_compositor_deliver_any_frames(mc, now + 12 * U_TIME_1MS_IN_NS);
	CHECK(mc->scheduled.active);
	CHECK(mc->scheduled.data.frame_id == frame);
	CHECK_FALSE(mc->delivered.active);
	multi_compositor_deliver_any_frames(mc, now + 20 * U_TIME_1MS_IN_NS);
	CHECK_FALSE(mc->scheduled.active);
	CHECK(mc->delivered.active);
	CHECK(mc->delivered.data.frame_id == frame);
	xrt_comp_native_destroy(&client);
	u_paf_destroy(&system.upaf);
	os_thread_helper_destroy(&system.oth);
	os_mutex_destroy(&system.list_and_timing_lock);
}

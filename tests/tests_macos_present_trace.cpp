// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "util/u_gpu_trace_present.h"

TEST_CASE("Presentation trace joins compositor timeline IDs with independently numbered presents")
{
	u_gpu_trace_present state{};
	u_gpu_trace_present_init(&state, 1);
	u_gpu_trace_present_prepare(&state, 17, 2, 9);
	u_gpu_trace_present_gpu_end(&state, 9, 100, 110);
	u_gpu_trace_frame frame{1, 0, 17, 2, 9};
	u_gpu_trace_present_reported(&state, &frame, 200, 210, 2);
	auto &entry = state.entries[9];
	REQUIRE(entry.terminal);
	REQUIRE(entry.measured_ns == 100);
	REQUIRE(entry.frame.frame_id == 17);
	REQUIRE(state.depth == 2);
	u_gpu_trace_present_gpu_end(&state, 9, 50, 220);
	u_gpu_trace_present_reported(&state, &frame, 300, 310, 3);
	REQUIRE(entry.measured_ns == 100);
	REQUIRE(state.depth == 2);
}

TEST_CASE("Late GPU feedback measures real timestamps without a backdated native interval")
{
	u_gpu_trace_present state{};
	u_gpu_trace_present_init(&state, 1);
	u_gpu_trace_present_prepare(&state, 3, 0, 5);
	u_gpu_trace_frame frame{1, 0, 3, 0, 5};
	u_gpu_trace_present_reported(&state, &frame, 1000, 1100, 1);
	u_gpu_trace_present_gpu_end(&state, 5, 900, 1200);
	REQUIRE(state.entries[5].measured_ns == 100);
	REQUIRE_FALSE(state.entries[5].native_started);
	u_gpu_trace_frame older{1, 0, 2, 0, 4};
	u_gpu_trace_present_reported(&state, &older, 1200, 1300, 3);
	REQUIRE(state.depth == 1);
}

TEST_CASE("Dropped, invalid, evicted and closed frames cannot acquire false display intervals")
{
	u_gpu_trace_present state{};
	u_gpu_trace_present_init(&state, 1);
	u_gpu_trace_present_prepare(&state, 1, 0, 1);
	u_gpu_trace_present_gpu_end(&state, 1, 100, 110);
	u_gpu_trace_present_cancel(&state, 1, "superseded");
	u_gpu_trace_frame first{1, 0, 1, 0, 1};
	u_gpu_trace_present_reported(&state, &first, 200, 210, 2);
	REQUIRE(state.entries[1].measured_ns == -1);
	u_gpu_trace_present_prepare(&state, 2, 0, 2);
	u_gpu_trace_present_gpu_end(&state, 2, 500, 510);
	u_gpu_trace_frame second{1, 0, 2, 0, 2};
	u_gpu_trace_present_reported(&state, &second, 400, 410, 2);
	REQUIRE(state.entries[2].terminal);
	REQUIRE(state.entries[2].measured_ns == -1);
	u_gpu_trace_present_prepare(&state, 3, 0, 3);
	u_gpu_trace_present_prepare(&state, 131, 0, 3 + U_GPU_TRACE_PRESENT_CAPACITY);
	u_gpu_trace_present_gpu_end(&state, 3, 100, 110);
	REQUIRE(state.entries[3].gpu_end_ns == 0);
	u_gpu_trace_present_close(&state);
	u_gpu_trace_present_gpu_end(&state, 131, 100, 110);
	u_gpu_trace_present_prepare(&state, 4, 0, 4);
	REQUIRE_FALSE(state.entries[4].valid);
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tests for handing refresh rate requests to the compositor thread.
 */

#include "../src/xrt/compositor/main/comp_refresh_request.h"
#include "os/os_time.h"

#include "catch_amalgamated.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

struct Display
{
	std::atomic<int> applied{0};
	std::atomic<bool> on_compositor_thread{false};
	float hz = 0;
	xrt_result_t result = XRT_SUCCESS;
	std::thread::id compositor_thread;
	// Simulates a slow mode switch, in nanoseconds.
	uint64_t apply_delay_ns = 0;
};

xrt_result_t
apply(void *ctx, float hz)
{
	Display *d = static_cast<Display *>(ctx);
	if (d->apply_delay_ns > 0) {
		std::this_thread::sleep_for(std::chrono::nanoseconds(d->apply_delay_ns));
	}
	d->hz = hz;
	d->on_compositor_thread = std::this_thread::get_id() == d->compositor_thread;
	d->applied++;
	return d->result;
}

bool
wait_pending(comp_refresh_request &r)
{
	for (int i = 0; i < 2000; i++) {
		os_mutex_lock(&r.mutex);
		bool pending = r.pending;
		os_mutex_unlock(&r.mutex);
		if (pending) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

void
set_active(comp_refresh_request &r, bool active, Display &d)
{
	comp_refresh_request_lock(&r);
	comp_refresh_request_set_session_active_locked(&r, active, apply, &d);
	comp_refresh_request_unlock(&r);
}

constexpr uint64_t kSecond = 1000000000;

} // namespace

TEST_CASE("Refresh requests apply immediately while no session runs", "[compositor][refresh]")
{
	comp_refresh_request r = {};
	REQUIRE(comp_refresh_request_init(&r) == 0);
	Display d;
	d.compositor_thread = std::this_thread::get_id();

	CHECK(comp_refresh_request_submit(&r, 120.0f, kSecond, apply, &d) == XRT_SUCCESS);
	CHECK(d.applied == 1);
	CHECK(d.hz == 120.0f);

	d.result = XRT_ERROR_INVALID_ARGUMENT;
	CHECK(comp_refresh_request_submit(&r, 73.0f, kSecond, apply, &d) == XRT_ERROR_INVALID_ARGUMENT);

	comp_refresh_request_fini(&r);
}

TEST_CASE("Refresh requests apply on the compositor thread during a session", "[compositor][refresh]")
{
	comp_refresh_request r = {};
	REQUIRE(comp_refresh_request_init(&r) == 0);
	Display d;
	set_active(r, true, d);

	xrt_result_t result = XRT_ERROR_IPC_FAILURE;
	std::thread api([&] { result = comp_refresh_request_submit(&r, 90.0f, 2 * kSecond, apply, &d); });
	REQUIRE(wait_pending(r));
	CHECK(d.applied == 0);

	// A second request is rejected while the first is outstanding.
	CHECK(comp_refresh_request_submit(&r, 120.0f, kSecond, apply, &d) == XRT_ERROR_OUTPUT_REQUEST_FAILURE);

	d.compositor_thread = std::this_thread::get_id();
	comp_refresh_request_process(&r, apply, &d);
	api.join();
	CHECK(result == XRT_SUCCESS);
	CHECK(d.applied == 1);
	CHECK(d.hz == 90.0f);
	CHECK(d.on_compositor_thread);

	comp_refresh_request_fini(&r);
}

TEST_CASE("Ending a session applies an outstanding refresh request", "[compositor][refresh]")
{
	comp_refresh_request r = {};
	REQUIRE(comp_refresh_request_init(&r) == 0);
	Display d;
	set_active(r, true, d);

	xrt_result_t result = XRT_ERROR_IPC_FAILURE;
	std::thread api([&] { result = comp_refresh_request_submit(&r, 120.0f, 2 * kSecond, apply, &d); });
	REQUIRE(wait_pending(r));
	set_active(r, false, d);
	api.join();
	CHECK(result == XRT_SUCCESS);
	CHECK(d.applied == 1);

	// Idle again, so the next request applies on the caller's thread.
	CHECK(comp_refresh_request_submit(&r, 90.0f, kSecond, apply, &d) == XRT_SUCCESS);
	CHECK(d.applied == 2);

	comp_refresh_request_fini(&r);
}

TEST_CASE("A refresh request that times out is withdrawn", "[compositor][refresh]")
{
	comp_refresh_request r = {};
	REQUIRE(comp_refresh_request_init(&r) == 0);
	Display d;
	set_active(r, true, d);

	CHECK(comp_refresh_request_submit(&r, 120.0f, 20000000, apply, &d) == XRT_ERROR_OUTPUT_REQUEST_FAILURE);
	// Never applied late, and the queue accepts a new request.
	comp_refresh_request_process(&r, apply, &d);
	CHECK(d.applied == 0);

	xrt_result_t result = XRT_ERROR_IPC_FAILURE;
	std::thread api([&] { result = comp_refresh_request_submit(&r, 90.0f, 2 * kSecond, apply, &d); });
	REQUIRE(wait_pending(r));
	comp_refresh_request_process(&r, apply, &d);
	api.join();
	CHECK(result == XRT_SUCCESS);
	CHECK(d.applied == 1);

	comp_refresh_request_fini(&r);
}

TEST_CASE("A slow apply that outlasts the wait still reports success", "[compositor][refresh]")
{
	comp_refresh_request r = {};
	REQUIRE(comp_refresh_request_init(&r) == 0);
	Display d;
	d.apply_delay_ns = 500000000;
	set_active(r, true, d);

	xrt_result_t result = XRT_ERROR_IPC_FAILURE;
	std::thread api([&] { result = comp_refresh_request_submit(&r, 90.0f, 200000000, apply, &d); });
	REQUIRE(wait_pending(r));
	// The compositor thread holds the mutex for longer than the caller's timeout.
	comp_refresh_request_process(&r, apply, &d);
	api.join();
	CHECK(d.applied == 1);
	CHECK(result == XRT_SUCCESS);

	comp_refresh_request_fini(&r);
}

TEST_CASE("Shutdown fails an outstanding refresh request", "[compositor][refresh]")
{
	comp_refresh_request r = {};
	REQUIRE(comp_refresh_request_init(&r) == 0);
	Display d;
	set_active(r, true, d);

	xrt_result_t result = XRT_SUCCESS;
	std::thread api([&] { result = comp_refresh_request_submit(&r, 90.0f, 10 * kSecond, apply, &d); });
	REQUIRE(wait_pending(r));
	comp_refresh_request_fini(&r);
	api.join();
	CHECK(result == XRT_ERROR_OUTPUT_REQUEST_FAILURE);
	CHECK(d.applied == 0);
}

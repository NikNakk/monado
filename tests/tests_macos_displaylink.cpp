// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "multi/comp_multi_macos_displaylink.h"
#include "os/os_time.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define CHECK(condition) do { if (!(condition)) { \
	std::fprintf(stderr, "Failed: %s (line %d)\n", #condition, __LINE__); std::abort(); \
} } while (0)

static void
run_driven_test()
{
	CHECK(comp_multi_macos_displaylink_driven_mode());
	comp_multi_macos_displaylink_set_active(true);
	CHECK(comp_multi_macos_displaylink_active());

	struct comp_multi_macos_displaylink_timing timing = {};
	CHECK(!comp_multi_macos_displaylink_current_timing(&timing));
	int drawable = 0;

	for (bool cancel : {false, true}) {
		std::atomic<bool> returned{false};
		uint64_t now = os_monotonic_get_ns();
		std::thread callback([&] {
			CHECK(comp_multi_macos_displaylink_submit_tick(&drawable, now, now + 1000000000, now + 1008000000));
			returned = true;
		});

		uint64_t cb = 0, target = 0, presentation = 0;
		CHECK(comp_multi_macos_displaylink_wait_tick(&cb, &target, &presentation));
		CHECK(comp_multi_macos_displaylink_current_timing(&timing));
		CHECK(timing.callback_ns == (int64_t)now);
		CHECK(timing.deadline_ns == (int64_t)now + 1000000000);
		CHECK(timing.presentation_ns == (int64_t)now + 1008000000);
		CHECK(cb == now && target == now + 1000000000 && presentation == now + 1008000000);
		CHECK(comp_multi_macos_displaylink_current_drawable() == &drawable);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		CHECK(!returned); // Driven callback retains the drawable until scheduled/cancelled.

		if (cancel) {
			comp_multi_macos_displaylink_cancel_pending_tick();
		} else {
			comp_multi_macos_displaylink_complete_tick();
		}
		CHECK(!comp_multi_macos_displaylink_current_timing(&timing));
		callback.join();
		CHECK(returned && comp_multi_macos_displaylink_current_drawable() == nullptr);
	}

	comp_multi_macos_displaylink_set_active(false);
	CHECK(!comp_multi_macos_displaylink_active());
}

static void
run_hybrid_test()
{
	CHECK(comp_multi_macos_displaylink_hybrid_mode());
	comp_multi_macos_displaylink_set_active(true);
	CHECK(comp_multi_macos_displaylink_active());

	struct comp_multi_macos_displaylink_timing timing = {};
	CHECK(!comp_multi_macos_displaylink_current_timing(&timing));
	CHECK(comp_multi_macos_displaylink_current_drawable() == nullptr);

	uint64_t now = os_monotonic_get_ns();
	CHECK(comp_multi_macos_displaylink_submit_tick(nullptr, now, now + 1000000, now + 9000000));

	uint64_t cb = 0, target = 0, presentation = 0;
	CHECK(comp_multi_macos_displaylink_wait_tick(&cb, &target, &presentation));
	CHECK(cb == now && target == now + 1000000 && presentation == now + 9000000);

	/* Wake-only hybrid deliberately does not expose CAMetal timing or a drawable
	 * to the native compositor/HMD target. Those stay on the legacy path. */
	CHECK(!comp_multi_macos_displaylink_current_timing(&timing));
	CHECK(comp_multi_macos_displaylink_current_drawable() == nullptr);

	comp_multi_macos_displaylink_set_active(false);
	CHECK(!comp_multi_macos_displaylink_active());
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "driven";

	if (!__builtin_available(macOS 14.0, *)) {
		CHECK(!comp_multi_macos_displaylink_enabled());
		return 0;
	}

	if (std::strcmp(mode, "legacy") == 0) {
		CHECK(!comp_multi_macos_displaylink_enabled());
		CHECK(!comp_multi_macos_displaylink_active());
		CHECK(!comp_multi_macos_displaylink_wait_tick(nullptr, nullptr, nullptr));
		return 0;
	}

	CHECK(comp_multi_macos_displaylink_enabled());
	CHECK(!comp_multi_macos_displaylink_active());

	if (std::strcmp(mode, "hybrid") == 0) {
		run_hybrid_test();
		return 0;
	}

	CHECK(std::strcmp(mode, "driven") == 0);
	run_driven_test();
	return 0;
}

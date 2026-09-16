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

int
main(int argc, char **argv)
{
	bool expected_enabled = argc == 1 || std::strcmp(argv[1], "disabled") != 0;
	if (__builtin_available(macOS 14.0, *)) {
		CHECK(comp_multi_macos_displaylink_enabled() == expected_enabled);
	} else {
		CHECK(!comp_multi_macos_displaylink_enabled());
		return 0;
	}
	// Selecting the default must not redirect null/other targets into the bridge.
	CHECK(!comp_multi_macos_displaylink_active());
	if (!expected_enabled) {
		CHECK(!comp_multi_macos_displaylink_wait_tick(nullptr, nullptr, nullptr));
		return 0;
	}

	comp_multi_macos_displaylink_set_active(true);
	CHECK(comp_multi_macos_displaylink_active());
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
		CHECK(cb == now && target == now + 1000000000 && presentation == now + 1008000000);
		CHECK(comp_multi_macos_displaylink_current_drawable() == &drawable);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		CHECK(!returned); // The delegate must retain the drawable until scheduled or cancelled.
		if (cancel) {
			comp_multi_macos_displaylink_cancel_pending_tick();
		} else {
			comp_multi_macos_displaylink_complete_tick();
		}
		callback.join();
		CHECK(returned && comp_multi_macos_displaylink_current_drawable() == nullptr);
	}
	uint64_t now = os_monotonic_get_ns();
	CHECK(!comp_multi_macos_displaylink_submit_tick(&drawable, now, now + 1000000, now + 9000000));
	CHECK(comp_multi_macos_displaylink_current_drawable() == nullptr);
	comp_multi_macos_displaylink_set_active(false);
	CHECK(!comp_multi_macos_displaylink_active());
	return 0;
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "os/os_time.h"
#include "server/ipc_server_swapchain_wait.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <thread>
#include <vector>

namespace {

// Emulates a swapchain image that becomes ready at ready_ns (0: never).
struct FakeSwapchain : xrt_swapchain
{
	int64_t ready_ns = 0;
	std::vector<int64_t> timeouts;

	FakeSwapchain()
	{
		reference.count = 1;
		image_count = 1;
		wait_image = [](xrt_swapchain *base, int64_t timeout_ns, uint32_t) {
			auto *sc = static_cast<FakeSwapchain *>(base);
			sc->timeouts.push_back(timeout_ns);
			int64_t now = os_monotonic_get_ns();
			if (sc->ready_ns != 0 && now >= sc->ready_ns) {
				return XRT_SUCCESS;
			}
			int64_t until = sc->ready_ns != 0 ? std::min(sc->ready_ns, now + timeout_ns) : now + timeout_ns;
			std::this_thread::sleep_for(std::chrono::nanoseconds(until - now));
			return sc->ready_ns != 0 && os_monotonic_get_ns() >= sc->ready_ns ? XRT_SUCCESS : XRT_TIMEOUT;
		};
	}
};

struct Client
{
	int fds[2];
	Client()
	{
		REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	}
	~Client()
	{
		close(fds[0]);
		if (fds[1] >= 0) {
			close(fds[1]);
		}
	}
	void
	hang_up()
	{
		close(fds[1]);
		fds[1] = -1;
	}
};

constexpr int64_t ms = 1000000;

} // namespace

TEST_CASE("infinite wait is honoured across bounded slices")
{
	FakeSwapchain sc;
	Client client;
	volatile bool running = true;
	sc.ready_ns = os_monotonic_get_ns() + 1200 * ms;
	CHECK(ipc_server_swapchain_wait_in_slices(&sc, INT64_MAX, 0, client.fds[0], &running) == XRT_SUCCESS);
	CHECK(sc.timeouts.size() >= 3);
	for (int64_t timeout : sc.timeouts) {
		CHECK(timeout <= IPC_SERVER_SWAPCHAIN_WAIT_SLICE_NS);
	}
}

TEST_CASE("finite wait times out after the application's timeout, not a slice")
{
	FakeSwapchain sc;
	Client client;
	volatile bool running = true;
	int64_t start = os_monotonic_get_ns();
	CHECK(ipc_server_swapchain_wait_in_slices(&sc, 700 * ms, 0, client.fds[0], &running) == XRT_TIMEOUT);
	int64_t elapsed = os_monotonic_get_ns() - start;
	CHECK(elapsed >= 700 * ms);
	CHECK(elapsed < 1000 * ms);
	REQUIRE(sc.timeouts.size() == 2);
	CHECK(sc.timeouts[0] == IPC_SERVER_SWAPCHAIN_WAIT_SLICE_NS);
	CHECK(sc.timeouts[1] <= 200 * ms);
}

TEST_CASE("zero timeout polls once")
{
	FakeSwapchain sc;
	Client client;
	volatile bool running = true;
	CHECK(ipc_server_swapchain_wait_in_slices(&sc, 0, 0, client.fds[0], &running) == XRT_TIMEOUT);
	REQUIRE(sc.timeouts.size() == 1);
	CHECK(sc.timeouts[0] == 0);
}

TEST_CASE("a disconnected client ends an infinite wait after one slice")
{
	FakeSwapchain sc;
	Client client;
	volatile bool running = true;
	client.hang_up();
	CHECK(ipc_server_swapchain_wait_in_slices(&sc, INT64_MAX, 0, client.fds[0], &running) == XRT_TIMEOUT);
	CHECK(sc.timeouts.size() == 1);
}

TEST_CASE("server shutdown ends an infinite wait after one slice")
{
	FakeSwapchain sc;
	Client client;
	volatile bool running = false;
	CHECK(ipc_server_swapchain_wait_in_slices(&sc, INT64_MAX, 0, client.fds[0], &running) == XRT_TIMEOUT);
	CHECK(sc.timeouts.size() == 1);
}

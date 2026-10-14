// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "b_session.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

TEST_CASE("session loss status is sticky and does not consume lifecycle events")
{
	auto *bs = b_session_create(nullptr);
	xrt_session *xs = &bs->base;
	bool lost = true;
	REQUIRE(xrt_session_get_loss_status(xs, &lost) == XRT_SUCCESS);
	CHECK_FALSE(lost);
	union xrt_session_event pending{};
	pending.type = XRT_SESSION_EVENT_LOSS_PENDING;
	b_session_event_push(bs, &pending);
	REQUIRE(xrt_session_get_loss_status(xs, &lost) == XRT_SUCCESS);
	CHECK_FALSE(lost);
	union xrt_session_event event{};
	event.type = XRT_SESSION_EVENT_LOST;
	b_session_event_push(bs, &event);
	REQUIRE(xrt_session_get_loss_status(xs, &lost) == XRT_SUCCESS);
	CHECK(lost);
	REQUIRE(xrt_session_poll_events(xs, &event) == XRT_SUCCESS);
	CHECK(event.type == XRT_SESSION_EVENT_LOSS_PENDING);
	REQUIRE(xrt_session_poll_events(xs, &event) == XRT_SUCCESS);
	CHECK(event.type == XRT_SESSION_EVENT_LOST);
	REQUIRE(xrt_session_poll_events(xs, &event) == XRT_SUCCESS);
	CHECK(event.type == XRT_SESSION_EVENT_NONE);
	REQUIRE(xrt_session_get_loss_status(xs, &lost) == XRT_SUCCESS);
	CHECK(lost);
	xrt_session_destroy(&xs);
}

TEST_CASE("loss observation and event polling coexist during concurrent publication")
{
	auto *bs = b_session_create(nullptr);
	xrt_session *xs = &bs->base;
	std::atomic<bool> start{false}, published{false};
	std::atomic<int> errors{0};

	std::vector<int64_t> received;
	int lost_events = 0;
	std::thread poller([&] {
		while (!start.load())
			std::this_thread::yield();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		for (;;) {
			if (std::chrono::steady_clock::now() > deadline) {
				++errors;
				return;
			}
			union xrt_session_event event{};
			if (xrt_session_poll_events(xs, &event) != XRT_SUCCESS) {
				++errors;
				return;
			}
			if (event.type == XRT_SESSION_EVENT_STATE_CHANGE)
				received.push_back(event.state.timestamp_ns);
			if (event.type == XRT_SESSION_EVENT_LOST)
				++lost_events;
			if (event.type == XRT_SESSION_EVENT_NONE && published.load() && lost_events == 1)
				break;
		}
	});
	std::thread observer([&] {
		while (!start.load())
			std::this_thread::yield();
		do {
			bool lost = false;
			if (xrt_session_get_loss_status(xs, &lost) != XRT_SUCCESS) {
				++errors;
				return;
			}
		} while (!published.load());
		bool lost = false;
		if (xrt_session_get_loss_status(xs, &lost) != XRT_SUCCESS || !lost)
			++errors;
	});
	start.store(true);
	for (int i = 0; i < 1000; ++i) {
		union xrt_session_event event{};
		event.type = XRT_SESSION_EVENT_STATE_CHANGE;
		event.state.timestamp_ns = i;
		b_session_event_push(bs, &event);
	}
	union xrt_session_event event{};
	event.type = XRT_SESSION_EVENT_LOST;
	b_session_event_push(bs, &event);
	published.store(true);
	observer.join();
	poller.join();
	CHECK(errors.load() == 0);
	REQUIRE(received.size() == 1000);
	for (int i = 0; i < 1000; ++i)
		CHECK(received[i] == i);
	CHECK(lost_events == 1);
	bool lost = false;
	REQUIRE(xrt_session_get_loss_status(xs, &lost) == XRT_SUCCESS);
	CHECK(lost);
	xrt_session_destroy(&xs);
}

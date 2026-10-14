// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "oxr/oxr_objects.h"
#include "oxr/oxr_logger.h"
extern "C" {
#include "oxr/oxr_swapchain_common.h"
}
#include "b_session.h"
#include <atomic>
#include <chrono>
#include <thread>

namespace {
oxr_swapchain *created;
std::atomic<int> event_dispatches{0};
struct Fixture
{
	b_session *bs = b_session_create(nullptr);
	oxr_session session{};
	oxr_swapchain sc{};
	xrt_swapchain native{};
	oxr_logger logger{};
	std::atomic<bool> waiting{false}, release{false};
	int barriers = 0;
	bool stall = true;
	Fixture()
	{
		session.xs = &bs->base;
		sc.sess = &session;
		sc.swapchain = &native;
		created = &sc;
		event_dispatches.store(0);
	}
	~Fixture()
	{
		xrt_session *xs = &bs->base;
		xrt_session_destroy(&xs);
	}
};
Fixture *owner;
} // namespace
extern "C" {
XrResult
oxr_swapchain_common_create(oxr_logger *, oxr_session *, const XrSwapchainCreateInfo *, oxr_swapchain **out)
{
	*out = created;
	return XR_SUCCESS;
}
XrResult
oxr_swapchain_common_acquire(oxr_logger *, oxr_swapchain *, uint32_t *out)
{
	*out = 0;
	return XR_SUCCESS;
}
XrResult
oxr_error(oxr_logger *, XrResult result, const char *, ...)
{
	return result;
}
void
oxr_warn(oxr_logger *, const char *, ...)
{}
bool
client_vk_shared_event_enabled()
{
	return true;
}
bool
client_vk_metal_texture_enabled()
{
	return true;
}
XrResult
oxr_session_poll(oxr_logger *, oxr_session *session)
{
	++event_dispatches;
	// Allow the previous implementation to terminate, so the negative control
	// reports an unwanted dispatch rather than hanging the test.
	xrt_atomic_s32_store(&session->has_lost, 1);
	return XR_SUCCESS;
}
}

TEST_CASE("Vulkan acquire cancels after loss even when the event owner already consumed it")
{
	Fixture f;
	owner = &f;
	f.native.wait_image = [](xrt_swapchain *, int64_t timeout, uint32_t) {
		auto *f = owner;
		if (!f->stall)
			return XRT_SUCCESS;
		f->waiting.store(true);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!f->release.load()) {
			if (std::chrono::steady_clock::now() > deadline)
				return XRT_ERROR_VULKAN;
			std::this_thread::yield();
		}
		return timeout == 50000000ll ? XRT_TIMEOUT : XRT_ERROR_VULKAN;
	};
	f.native.barrier_image = [](xrt_swapchain *, xrt_barrier_direction, uint32_t) {
		++owner->barriers;
		return XRT_SUCCESS;
	};
	oxr_swapchain *sc = nullptr;
	XrSwapchainCreateInfo info{};
	REQUIRE(oxr_swapchain_vk_create(&f.logger, &f.session, &info, &sc) == XR_SUCCESS);
	int poll_error = 0;
	std::thread event_owner([&] {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!f.waiting.load()) {
			if (std::chrono::steady_clock::now() > deadline) {
				poll_error = 1;
				f.release.store(true);
				return;
			}
			std::this_thread::yield();
		}
		union xrt_session_event event{};
		event.type = XRT_SESSION_EVENT_LOST;
		b_session_event_push(f.bs, &event);
		if (xrt_session_poll_events(f.session.xs, &event) != XRT_SUCCESS ||
		    event.type != XRT_SESSION_EVENT_LOST)
			poll_error = 2;
		f.release.store(true);
	});
	uint32_t image = UINT32_MAX;
	XrSwapchainImageAcquireInfo acquire{};
	XrResult result = sc->acquire_image(&f.logger, sc, &acquire, &image);
	event_owner.join();
	CHECK(poll_error == 0);
	CHECK(result == XR_ERROR_SESSION_LOST);
	CHECK(event_dispatches.load() == 0);
	CHECK(xrt_atomic_s32_load(&f.session.has_lost) == 1);
	CHECK(f.barriers == 0);
}

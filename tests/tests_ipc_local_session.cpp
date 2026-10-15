// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "client/ipc_client.h"
#include "client/ipc_client_macos_hosted.h"
#include "ipc_client_generated.h"
#include "multi/comp_multi_private.h"
#include "multi/comp_multi_interface.h"
#include "util/u_macos_hosted_client.h"
#include "b_session.h"
#include "xrt/xrt_system.h"
#include "catch_amalgamated.hpp"
#include <cstdlib>
#include <atomic>
#include <cstring>
#include "util/u_logging.h"
#include <vector>

namespace {
int remote_creates;
xrt_session *remote_session;

#ifdef XRT_OS_OSX
// Tests that lose sessions on purpose expect the threshold they were written
// for, not the default. It is read once, so set it before any compositor exists.
const int pin_gpu_hung_threshold = setenv("XRT_MACOS_CLIENT_GPU_HUNG_MS", "500", 0);
#endif

// Use the real multi compositor and its wait thread, without starting a GPU renderer.
struct Fixture
{
	struct multi_system_compositor multi{};
	xrt_compositor_native native{};
	ipc_connection connection{};
	u_macos_hosted_client *client;
	xrt_system *system;
	xrt_session *session = nullptr;
	xrt_compositor_native *compositor = nullptr;
	std::vector<int> calls;
	static xrt_result_t
	attach(void *, uint32_t)
	{
		return XRT_SUCCESS;
	}
	static xrt_result_t
	visible(void *ctx, u_macos_display_host_visibility v)
	{
		static_cast<Fixture *>(ctx)->calls.push_back(v);
		return XRT_SUCCESS;
	}
	static xrt_result_t
	active(void *ctx, bool active)
	{
		static_cast<Fixture *>(ctx)->calls.push_back(active ? 11 : 10);
		return XRT_SUCCESS;
	}
	static void
	detach(void *)
	{}
	const u_macos_hosted_client_ops ops = {attach, visible, active, detach};
	static xrt_result_t
	create(xrt_system_compositor *xsysc,
	       const xrt_session_info *info,
	       xrt_session_event_sink *sink,
	       xrt_compositor_native **out)
	{
		return multi_compositor_create(reinterpret_cast<struct multi_system_compositor *>(xsysc), info, sink,
		                               out);
	}
	Fixture()
	{
		os_mutex_init(&multi.list_and_timing_lock);
		os_thread_helper_init(&multi.oth);
		u_pa_factory_create(&multi.upaf);
		multi.xcn = &native;
		multi.base.create_native_compositor = create;
		multi.last_timings.predicted_display_time_ns = os_monotonic_get_ns();
		multi.last_timings.predicted_display_period_ns = 16000000;
		multi.last_timings.diff_ns = 5000000;
		client = u_macos_hosted_client_create(&ops, this, true);
		system = ipc_client_system_create_with_local_compositor(&connection, &multi.base, client);
		REQUIRE(system != nullptr);
		REQUIRE(u_macos_hosted_client_attach(client, 1) == XRT_SUCCESS);
	}
	void
	create_session()
	{
		xrt_session_info info{};
		REQUIRE(xrt_system_create_session(system, &info, &session, &compositor) == XRT_SUCCESS);
	}
	void
	begin()
	{
		xrt_begin_session_info info{};
		REQUIRE(xrt_comp_begin_session(&compositor->base, &info) == XRT_SUCCESS);
		REQUIRE(u_macos_hosted_client_set_visibility(client, U_MACOS_DISPLAY_HOST_SHOWN) == XRT_SUCCESS);
	}
	~Fixture()
	{
		xrt_comp_native_destroy(&compositor);
		xrt_session_destroy(&session);
		xrt_system_destroy(&system);
		u_macos_hosted_client_close(client);
		u_macos_hosted_client_release(client);
		u_paf_destroy(&multi.upaf);
		os_thread_helper_destroy(&multi.oth);
		os_mutex_destroy(&multi.list_and_timing_lock);
	}
};
} // namespace

// Only remote IPC transport is stubbed. Local session and compositor lifecycle are production code.
extern "C" {
xrt_result_t
ipc_call_session_create(ipc_connection *, const xrt_session_info *, bool)
{
	++remote_creates;
	return XRT_SUCCESS;
}
xrt_result_t
ipc_call_system_get_properties(ipc_connection *, xrt_system_properties *)
{
	return XRT_SUCCESS;
}
xrt_session *
ipc_client_session_create(ipc_connection *)
{
	remote_session = &b_session_create(nullptr)->base;
	return remote_session;
}
xrt_result_t
ipc_client_create_native_compositor(xrt_system_compositor *, const xrt_session_info *, xrt_compositor_native **)
{
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}
bool
ipc_client_macos_hosted_follows_service_focus(u_macos_hosted_client *client)
{
	return u_macos_hosted_client_follows_service_focus(client);
}
void
ipc_client_macos_hosted_session_active(u_macos_hosted_client *client, bool active)
{
	u_macos_hosted_client_set_active(client, active);
}
void
ipc_client_macos_hosted_set_visible(u_macos_hosted_client *client, bool visible)
{
	u_macos_hosted_client_set_visibility(client,
	                                     visible ? U_MACOS_DISPLAY_HOST_SHOWN : U_MACOS_DISPLAY_HOST_HIDDEN);
}
}

TEST_CASE("local end and destroy restore hosting without polling service events")
{
	Fixture f;
	f.create_session();
	f.begin();
	f.calls.clear();
	CHECK(xrt_comp_end_session(&f.compositor->base) == XRT_SUCCESS);
	CHECK(f.calls == std::vector<int>{0, 10});
	f.begin();
	f.calls.clear();
	xrt_comp_native_destroy(&f.compositor);
	CHECK(f.calls == std::vector<int>{0, 10});
	xrt_session_destroy(&f.session);
	f.create_session();
	f.begin();
}

TEST_CASE("separate instances get separate local lifecycle observers")
{
	Fixture first, second;
	first.create_session();
	second.create_session();
	first.begin();
	second.begin();
	first.calls.clear();
	second.calls.clear();
	xrt_comp_native_destroy(&first.compositor);
	CHECK(first.calls == std::vector<int>{0, 10});
	CHECK(second.calls.empty());
	CHECK(xrt_comp_end_session(&second.compositor->base) == XRT_SUCCESS);
	CHECK(second.calls == std::vector<int>{0, 10});
}

TEST_CASE("duplicate local session is rejected before creating a remote session")
{
	Fixture f;
	f.create_session();
	auto creates = remote_creates;
	xrt_session_info info{};
	xrt_session *session = nullptr;
	xrt_compositor_native *compositor = nullptr;
	CHECK(xrt_system_create_session(f.system, &info, &session, &compositor) ==
	      XRT_ERROR_MULTI_SESSION_NOT_IMPLEMENTED);
	CHECK(remote_creates == creates);
	CHECK(session == nullptr);
	CHECK(compositor == nullptr);
}

#ifdef XRT_OS_OSX
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
struct ThreeImageSwapchain : xrt_swapchain
{
	ThreeImageSwapchain(uint32_t count)
	{
		reference.count = 1;
		image_count = count;
		destroy = [](xrt_swapchain *sc) { delete static_cast<ThreeImageSwapchain *>(sc); };
	}
};

void
install_test_allocator(Fixture &f)
{
	f.native.base.get_swapchain_create_properties = [](xrt_compositor *, const xrt_swapchain_create_info *,
	                                                   xrt_swapchain_create_properties *out) {
		out->image_count = 3;
		return XRT_SUCCESS;
	};
	f.native.base.create_swapchain = [](xrt_compositor *, const xrt_swapchain_create_info *, xrt_swapchain **out) {
		*out = new ThreeImageSwapchain(3);
		return XRT_SUCCESS;
	};
	f.native.base.import_swapchain = [](xrt_compositor *, const xrt_swapchain_create_info *, xrt_image_native *,
	                                    uint32_t count, xrt_swapchain **out) {
		*out = new ThreeImageSwapchain(count);
		return XRT_SUCCESS;
	};
}

// A controllable producer semaphore. The production wait thread and slot
// scheduling are real; no GPU or headset is needed for the lifecycle test.
struct ProducerGate : xrt_compositor_semaphore
{
	std::mutex mutex;
	std::condition_variable changed;
	bool entered = false;
	bool ready = false;
	ProducerGate()
	{
		reference.count = 1;
		wait = [](xrt_compositor_semaphore *base, uint64_t, uint64_t timeout) {
			auto *gate = static_cast<ProducerGate *>(base);
			std::unique_lock<std::mutex> lock(gate->mutex);
			gate->entered = true;
			gate->changed.notify_all();
			return gate->changed.wait_for(lock, std::chrono::nanoseconds(timeout), [&] { return gate->ready; })
			           ? XRT_SUCCESS : XRT_TIMEOUT;
		};
		destroy = [](xrt_compositor_semaphore *) {};
	}
	void signal()
	{
		std::lock_guard<std::mutex> lock(mutex);
		ready = true;
		changed.notify_all();
	}
};

int64_t
submit_test_layer_on(xrt_compositor *xc, xrt_swapchain *sc, uint32_t image)
{
	int64_t id, wake, gpu, display, period;
	REQUIRE(xrt_comp_predict_frame(xc, &id, &wake, &gpu, &display, &period) == XRT_SUCCESS);
	REQUIRE(xrt_comp_mark_frame(xc, id, XRT_COMPOSITOR_FRAME_POINT_WOKE, os_monotonic_get_ns()) == XRT_SUCCESS);
	REQUIRE(xrt_comp_begin_frame(xc, id) == XRT_SUCCESS);
	xrt_layer_frame_data frame{};
	frame.frame_id = id;
	// Far in the future, so ordinary scheduling would keep the old scheduled
	// frame and block. Three-image scheduling must promptly supersede it.
	frame.display_time_ns = os_monotonic_get_ns() + 10000000000LL;
	REQUIRE(xrt_comp_layer_begin(xc, &frame) == XRT_SUCCESS);
	xrt_layer_data layer{};
	layer.type = XRT_LAYER_QUAD;
	layer.quad.sub.image_index = image;
	static xrt_device device{};
	REQUIRE(xrt_comp_layer_quad(xc, &device, sc, &layer) == XRT_SUCCESS);
	return id;
}

int64_t
submit_test_layer(Fixture &f, xrt_swapchain *sc, uint32_t image)
{
	return submit_test_layer_on(&f.compositor->base, sc, image);
}

uint32_t
scheduled_image(struct multi_compositor *mc)
{
	os_mutex_lock(&mc->slot_lock);
	uint32_t image = mc->scheduled.layers[0].data.quad.sub.image_index;
	os_mutex_unlock(&mc->slot_lock);
	return image;
}
} // namespace

TEST_CASE("three-image compatibility is explicit and verifies allocator count")
{
	Fixture f;
	f.create_session();
	install_test_allocator(f);
	auto *mc = multi_compositor(&f.compositor->base);
	xrt_swapchain_create_info info{};
	info.width = info.height = 64;
	info.array_size = info.face_count = info.sample_count = info.mip_count = 1;
	xrt_swapchain *sc = nullptr;
	REQUIRE(xrt_comp_create_swapchain(&f.compositor->base, &info, &sc) == XRT_SUCCESS);
	CHECK_FALSE(mc->latest_completed_three_images);
	xrt_swapchain_reference(&sc, nullptr);
	info.create = XRT_SWAPCHAIN_CREATE_MACOS_THREE_IMAGES;
	xrt_image_native images[4]{};
	CHECK(xrt_comp_import_swapchain(&f.compositor->base, &info, images, 4, &sc) ==
	      XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED);
	CHECK(sc == nullptr);
	CHECK_FALSE(mc->latest_completed_three_images);
	REQUIRE(xrt_comp_create_swapchain(&f.compositor->base, &info, &sc) == XRT_SUCCESS);
	CHECK(mc->latest_completed_three_images);
	xrt_swapchain_reference(&sc, nullptr);
}

TEST_CASE("ordinary scheduling keeps the future timestamp gate")
{
	Fixture f;
	f.create_session();
	install_test_allocator(f);
	xrt_swapchain_create_info info{};
	info.width = info.height = 64;
	info.array_size = info.face_count = info.sample_count = info.mip_count = 1;
	xrt_swapchain *sc = nullptr;
	REQUIRE(xrt_comp_create_swapchain(&f.compositor->base, &info, &sc) == XRT_SUCCESS);
	auto *mc = multi_compositor(&f.compositor->base);
	submit_test_layer(f, sc, 0);
	REQUIRE(xrt_comp_layer_commit(&f.compositor->base, XRT_GRAPHICS_SYNC_HANDLE_INVALID) == XRT_SUCCESS);
	os_mutex_lock(&f.multi.list_and_timing_lock);
	multi_compositor_deliver_any_frames(mc, os_monotonic_get_ns() + 8 * U_TIME_1MS_IN_NS);
	os_mutex_unlock(&f.multi.list_and_timing_lock);
	CHECK_FALSE(mc->delivered.active);
	CHECK(mc->scheduled.active);
	xrt_swapchain_reference(&sc, nullptr);
}

TEST_CASE("three-image scheduling retires superseded frames only after producer completion")
{
	ProducerGate gate; // Must outlive the fixture's wait thread even on assertion failure.
	Fixture f;
	f.create_session();
	install_test_allocator(f);
	xrt_swapchain_create_info info{};
	info.width = info.height = 64;
	info.array_size = info.face_count = info.sample_count = info.mip_count = 1;
	info.create = XRT_SWAPCHAIN_CREATE_MACOS_THREE_IMAGES;
	xrt_swapchain *sc = nullptr;
	REQUIRE(xrt_comp_create_swapchain(&f.compositor->base, &info, &sc) == XRT_SUCCESS);
	auto *mc = multi_compositor(&f.compositor->base);
	auto *xc = &f.compositor->base;
	submit_test_layer(f, sc, 0);
	REQUIRE(xrt_comp_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID) == XRT_SUCCESS);
	os_mutex_lock(&f.multi.list_and_timing_lock);
	multi_compositor_deliver_any_frames(mc, os_monotonic_get_ns() + 8 * U_TIME_1MS_IN_NS);
	os_mutex_unlock(&f.multi.list_and_timing_lock);
	REQUIRE(mc->delivered.active); // future timestamp must not starve latest-completed delivery
	submit_test_layer(f, sc, 1);
	REQUIRE(xrt_comp_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID) == XRT_SUCCESS);
	submit_test_layer(f, sc, 2);
	REQUIRE(xrt_comp_layer_commit_with_semaphore(xc, &gate, 1) == XRT_SUCCESS);
	{
		std::unique_lock<std::mutex> lock(gate.mutex);
		CHECK(gate.changed.wait_for(lock, std::chrono::seconds(1), [&] { return gate.entered; }));
	}
	CHECK(scheduled_image(mc) == 1);
	CHECK(mc->delivered.layers[0].data.quad.sub.image_index == 0);
	CHECK(__atomic_load_n(&sc->reference.count, __ATOMIC_RELAXED) == 4); // caller + three slots
	gate.signal();
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	while (scheduled_image(mc) != 2 && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(scheduled_image(mc) == 2);
	CHECK(mc->delivered.layers[0].data.quad.sub.image_index == 0); // preserve reprojection source
	CHECK(__atomic_load_n(&sc->reference.count, __ATOMIC_RELAXED) == 3); // superseded image is released
	xrt_swapchain_reference(&sc, nullptr);
}
#endif

#ifdef XRT_OS_OSX
TEST_CASE("failed producer wait never makes a frame schedulable")
{
	ProducerGate gate;
	gate.wait = [](xrt_compositor_semaphore *, uint64_t, uint64_t) { return XRT_ERROR_VULKAN; };
	Fixture f;
	f.create_session();
	install_test_allocator(f);
	xrt_swapchain_create_info info{};
	info.width = info.height = 64;
	info.array_size = info.face_count = info.sample_count = info.mip_count = 1;
	xrt_swapchain *sc = nullptr;
	REQUIRE(xrt_comp_create_swapchain(&f.compositor->base, &info, &sc) == XRT_SUCCESS);
	auto *mc = multi_compositor(&f.compositor->base);
	submit_test_layer(f, sc, 0);
	REQUIRE(xrt_comp_layer_commit_with_semaphore(&f.compositor->base, &gate, 7) == XRT_SUCCESS);
	bool failed = false;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!failed && std::chrono::steady_clock::now() < deadline) {
		os_thread_helper_lock(&mc->wait_thread.oth);
		failed = mc->wait_thread.failed;
		os_thread_helper_unlock(&mc->wait_thread.oth);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(failed);
	os_mutex_lock(&mc->slot_lock);
	CHECK_FALSE(mc->scheduled.active);
	CHECK_FALSE(mc->delivered.active);
	os_mutex_unlock(&mc->slot_lock);
	CHECK(mc->progress.active); // ownership is retained, not declared complete
	xrt_swapchain_reference(&sc, nullptr);
}

TEST_CASE("missing producer signal isolates a session with finite worker teardown")
{
	ProducerGate gate;
	Fixture f;
	f.create_session();
	install_test_allocator(f);
	xrt_swapchain_create_info info{};
	info.width = info.height = 64;
	info.array_size = info.face_count = info.sample_count = info.mip_count = 1;
	xrt_swapchain *sc = nullptr;
	REQUIRE(xrt_comp_create_swapchain(&f.compositor->base, &info, &sc) == XRT_SUCCESS);
	auto *mc = multi_compositor(&f.compositor->base);
	submit_test_layer(f, sc, 0);
	auto started = std::chrono::steady_clock::now();
	REQUIRE(xrt_comp_layer_commit_with_semaphore(&f.compositor->base, &gate, 19) == XRT_SUCCESS);
	bool failed = false;
	while (!failed && std::chrono::steady_clock::now() - started < std::chrono::seconds(2)) {
		os_thread_helper_lock(&mc->wait_thread.oth);
		failed = mc->wait_thread.failed;
		os_thread_helper_unlock(&mc->wait_thread.oth);
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(failed);
	CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
	os_mutex_lock(&mc->slot_lock);
	CHECK_FALSE(mc->scheduled.active);
	os_mutex_unlock(&mc->slot_lock);
	xrt_layer_frame_data next{};
	next.frame_id = mc->progress.data.frame_id;
	CHECK(xrt_comp_layer_begin(&f.compositor->base, &next) == XRT_ERROR_IPC_FAILURE);
	// Sticky rejection does not mutate pacing for this already-delivered frame.
	os_thread_helper_lock(&mc->wait_thread.oth);
	CHECK(mc->wait_thread.failed);
	os_thread_helper_unlock(&mc->wait_thread.oth);
	xrt_swapchain_reference(&sc, nullptr);
}
#endif

#ifdef XRT_OS_OSX
TEST_CASE("unresolved client does not stop another client on the same multi compositor")
{
	ProducerGate missing;
	Fixture f;
	f.create_session();
	install_test_allocator(f);
	xrt_swapchain_create_info create{};
	create.width = create.height = 64;
	create.array_size = create.face_count = create.sample_count = create.mip_count = 1;
	xrt_swapchain *failed_sc = nullptr;
	REQUIRE(xrt_comp_create_swapchain(&f.compositor->base, &create, &failed_sc) == XRT_SUCCESS);
	submit_test_layer(f, failed_sc, 0);
	REQUIRE(xrt_comp_layer_commit_with_semaphore(&f.compositor->base, &missing, 101) == XRT_SUCCESS);
	const xrt_session_info info{};
	for (unsigned i = 0; i < 2; i++) {
		xrt_compositor_native *healthy = nullptr;
		REQUIRE(multi_compositor_create(&f.multi, &info, nullptr, &healthy) == XRT_SUCCESS);
		xrt_swapchain *sc = nullptr;
		REQUIRE(xrt_comp_create_swapchain(&healthy->base, &create, &sc) == XRT_SUCCESS);
		submit_test_layer_on(&healthy->base, sc, i);
		auto begin = std::chrono::steady_clock::now();
		REQUIRE(xrt_comp_layer_commit(&healthy->base, XRT_GRAPHICS_SYNC_HANDLE_INVALID) == XRT_SUCCESS);
		CHECK(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(100));
		auto *mc = multi_compositor(&healthy->base);
		os_mutex_lock(&f.multi.list_and_timing_lock);
		multi_compositor_deliver_any_frames(mc, os_monotonic_get_ns() + 11000000000LL);
		os_mutex_unlock(&f.multi.list_and_timing_lock);
		CHECK(mc->delivered.active);
		CHECK(mc->delivered.layers[0].data.quad.sub.image_index == i);
		xrt_swapchain_reference(&sc, nullptr);
		xrt_comp_native_destroy(&healthy);
		// Second connection is created after the failed session's hung threshold.
		if (i == 0) std::this_thread::sleep_for(std::chrono::milliseconds(550));
	}
	xrt_swapchain_reference(&failed_sc, nullptr);
}
#endif
#ifdef XRT_OS_OSX
TEST_CASE("completed frame misses readiness cutoff without losing ownership and becomes eligible later")
{
	Fixture f;
	install_test_allocator(f);
	f.create_session();
	xrt_swapchain_create_info info{};
	info.width = info.height = 64;
	info.array_size = info.face_count = info.sample_count = info.mip_count = 1;
	info.create = XRT_SWAPCHAIN_CREATE_MACOS_THREE_IMAGES;
	xrt_swapchain *sc = nullptr;
	REQUIRE(xrt_comp_create_swapchain(&f.compositor->base, &info, &sc) == XRT_SUCCESS);
	auto *mc = multi_compositor(&f.compositor->base);
	submit_test_layer(f, sc, 0);
	REQUIRE(xrt_comp_layer_commit(&f.compositor->base, XRT_GRAPHICS_SYNC_HANDLE_INVALID) == XRT_SUCCESS);
	int64_t now = os_monotonic_get_ns();
	os_mutex_lock(&mc->slot_lock);
	mc->scheduled.gpu_ready_ns = now + 10 * U_TIME_1MS_IN_NS;
	os_mutex_unlock(&mc->slot_lock);
	multi_compositor_deliver_any_frames(mc, now + 12 * U_TIME_1MS_IN_NS);
	CHECK(mc->scheduled.active);
	CHECK_FALSE(mc->delivered.active);
	multi_compositor_deliver_any_frames(mc, now + 20 * U_TIME_1MS_IN_NS);
	CHECK_FALSE(mc->scheduled.active);
	CHECK(mc->delivered.active);
	xrt_swapchain_reference(&sc, nullptr);
}
#endif

TEST_CASE("hosted session loss query preserves queued local loss")
{
	Fixture f;
	f.create_session();
	bool lost = true;
	REQUIRE(xrt_session_get_loss_status(f.session, &lost) == XRT_SUCCESS);
	CHECK_FALSE(lost);
	auto *mc = reinterpret_cast<struct multi_compositor *>(f.compositor);
	union xrt_session_event event{};
	event.type = XRT_SESSION_EVENT_LOST;
	REQUIRE(xrt_session_event_sink_push(mc->xses, &event) == XRT_SUCCESS);
	REQUIRE(xrt_session_get_loss_status(f.session, &lost) == XRT_SUCCESS);
	CHECK(lost);
	REQUIRE(xrt_session_poll_events(f.session, &event) == XRT_SUCCESS);
	CHECK(event.type == XRT_SESSION_EVENT_LOST);
	REQUIRE(xrt_session_get_loss_status(f.session, &lost) == XRT_SUCCESS);
	CHECK(lost);
}

TEST_CASE("hosted loss query ignores unused remote compositor loss but detects transport failure")
{
	Fixture f;
	f.create_session();
	union xrt_session_event event{};
	event.type = XRT_SESSION_EVENT_LOST;
	b_session_event_push(reinterpret_cast<b_session *>(remote_session), &event);
	bool lost = true;
	REQUIRE(xrt_session_get_loss_status(f.session, &lost) == XRT_SUCCESS);
	CHECK_FALSE(lost);
	REQUIRE(xrt_session_poll_events(remote_session, &event) == XRT_SUCCESS);
	CHECK(event.type == XRT_SESSION_EVENT_LOST);
	remote_session->get_loss_status = [](xrt_session *, bool *) { return XRT_ERROR_IPC_FAILURE; };
	CHECK(xrt_session_get_loss_status(f.session, &lost) == XRT_ERROR_IPC_FAILURE);
}

#ifdef XRT_OS_OSX
TEST_CASE("passthrough rejection propagates from the native target before layer submission")
{
	Fixture f;
	f.create_session();
	f.native.base.create_passthrough = [](xrt_compositor *, const xrt_passthrough_create_info *) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	};
	xrt_passthrough_create_info info{};
	CHECK(xrt_comp_create_passthrough(&f.compositor->base, &info) == XRT_ERROR_FEATURE_NOT_SUPPORTED);
}
#endif

#ifdef XRT_OS_OSX
TEST_CASE("stopping an unresolved producer cancels without logging a GPU fault")
{
	struct Capture
	{
		std::atomic<int> faults{0};
		Capture()
		{
			u_log_set_sink(
			    [](const char *, int, const char *, u_logging_level, const char *format, va_list,
			       void *data) {
				    if (strstr(format, "Semaphore waiting failed") ||
				        strstr(format, "Client GPU obligation failed")) {
					    static_cast<Capture *>(data)->faults.fetch_add(1);
				    }
			    },
			    this);
		}
		~Capture()
		{
			u_log_set_sink(nullptr, nullptr);
		}
	} capture;
	ProducerGate gate;
	Fixture f;
	f.create_session();
	install_test_allocator(f);
	xrt_swapchain_create_info info{};
	info.width = info.height = 64;
	info.array_size = info.face_count = info.sample_count = info.mip_count = 1;
	xrt_swapchain *sc = nullptr;
	REQUIRE(xrt_comp_create_swapchain(&f.compositor->base, &info, &sc) == XRT_SUCCESS);
	submit_test_layer(f, sc, 0);
	REQUIRE(xrt_comp_layer_commit_with_semaphore(&f.compositor->base, &gate, 42) == XRT_SUCCESS);
	{
		std::unique_lock<std::mutex> lock(gate.mutex);
		REQUIRE(gate.changed.wait_for(lock, std::chrono::seconds(1), [&] { return gate.entered; }));
	}
	auto started = std::chrono::steady_clock::now();
	xrt_comp_native_destroy(&f.compositor);
	CHECK(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(250));
	CHECK(capture.faults.load() == 0);
	xrt_swapchain_reference(&sc, nullptr);
}
#endif

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
#include <vector>

namespace {
int remote_creates;

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
	return &b_session_create(nullptr)->base;
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

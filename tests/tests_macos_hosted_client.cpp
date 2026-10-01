// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "util/u_macos_hosted_client.h"
#include "catch_amalgamated.hpp"
#include <vector>

namespace {
struct Binding
{
	std::vector<int> calls;
	bool fail_activity = false;
	bool fail_visibility = false;
	static xrt_result_t
	attach(void *ctx, uint32_t id)
	{
		static_cast<Binding *>(ctx)->calls.push_back(100 + id);
		return XRT_SUCCESS;
	}
	static xrt_result_t
	visibility(void *ctx, enum u_macos_display_host_visibility state)
	{
		auto &b = *static_cast<Binding *>(ctx);
		b.calls.push_back(state);
		return b.fail_visibility ? XRT_ERROR_IPC_FAILURE : XRT_SUCCESS;
	}
	static xrt_result_t
	active(void *ctx, bool active)
	{
		auto &b = *static_cast<Binding *>(ctx);
		b.calls.push_back(active ? 11 : 10);
		return b.fail_activity ? XRT_ERROR_IPC_FAILURE : XRT_SUCCESS;
	}
	static void
	detach(void *ctx)
	{
		static_cast<Binding *>(ctx)->calls.push_back(20);
	}
	const u_macos_hosted_client_ops ops = {attach, visibility, active, detach};
	u_macos_hosted_client *client;
	Binding(bool follow_focus = true) : client(u_macos_hosted_client_create(&ops, this, follow_focus)) {}
	~Binding()
	{
		u_macos_hosted_client_close(client);
		u_macos_hosted_client_release(client);
	}
	void
	start()
	{
		REQUIRE(client != nullptr);
		REQUIRE(u_macos_hosted_client_attach(client, 1) == XRT_SUCCESS);
		REQUIRE(u_macos_hosted_client_set_active(client, true) == XRT_SUCCESS);
		REQUIRE(u_macos_hosted_client_set_visibility(client, U_MACOS_DISPLAY_HOST_SHOWN) == XRT_SUCCESS);
	}
};
} // namespace

TEST_CASE("ending a hosted session hides before deactivation without event polling")
{
	Binding b;
	b.start();
	auto ticket = u_macos_hosted_client_present_ticket(b.client);
	u_macos_hosted_client_note_presented(b.client, ticket);
	b.calls.clear();
	CHECK(u_macos_hosted_client_set_active(b.client, false) == XRT_SUCCESS);
	CHECK(b.calls == std::vector<int>{0, 10});
	u_macos_hosted_client_note_presented(b.client, ticket);
	CHECK(b.calls == std::vector<int>{0, 10});
	CHECK(u_macos_hosted_client_present_ticket(b.client) == 0);
}

TEST_CASE("stale presentation cannot promote a new handoff")
{
	Binding b;
	b.start();
	auto stale = u_macos_hosted_client_present_ticket(b.client);
	REQUIRE(u_macos_hosted_client_set_visibility(b.client, U_MACOS_DISPLAY_HOST_HIDDEN) == XRT_SUCCESS);
	REQUIRE(u_macos_hosted_client_set_visibility(b.client, U_MACOS_DISPLAY_HOST_SHOWN) == XRT_SUCCESS);
	auto current = u_macos_hosted_client_present_ticket(b.client);
	REQUIRE(current != stale);
	b.calls.clear();
	u_macos_hosted_client_note_presented(b.client, stale);
	CHECK(b.calls.empty());
	u_macos_hosted_client_note_presented(b.client, current);
	CHECK(b.calls == std::vector<int>{2});
	u_macos_hosted_client_note_presented(b.client, current);
	CHECK(b.calls == std::vector<int>{2});
	CHECK(u_macos_hosted_client_set_visibility(b.client, U_MACOS_DISPLAY_HOST_SHOWN) == XRT_SUCCESS);
	CHECK(b.calls == std::vector<int>{2});
}

TEST_CASE("hosted instances own separate connections and callbacks survive close")
{
	Binding first, second;
	first.start();
	second.start();
	auto ticket = u_macos_hosted_client_present_ticket(first.client);
	u_macos_hosted_client_reference(first.client);
	u_macos_hosted_client_close(first.client);
	auto count = first.calls.size();
	u_macos_hosted_client_note_presented(first.client, ticket);
	CHECK(first.calls.size() == count);
	CHECK(u_macos_hosted_client_set_active(first.client, true) != XRT_SUCCESS);
	CHECK(u_macos_hosted_client_present_ticket(second.client) != 0);
	u_macos_hosted_client_release(first.client);
}

TEST_CASE("failed activity reports can be retried and debug visibility waits for begin")
{
	Binding b(false);
	REQUIRE(u_macos_hosted_client_attach(b.client, 1) == XRT_SUCCESS);
	CHECK(u_macos_hosted_client_set_visibility(b.client, U_MACOS_DISPLAY_HOST_SHOWN) != XRT_SUCCESS);
	CHECK(u_macos_hosted_client_present_ticket(b.client) == 0);
	b.fail_activity = true;
	CHECK(u_macos_hosted_client_set_active(b.client, true) == XRT_ERROR_IPC_FAILURE);
	b.fail_activity = false;
	CHECK(u_macos_hosted_client_set_active(b.client, true) == XRT_SUCCESS);
	CHECK(u_macos_hosted_client_present_ticket(b.client) != 0);
	b.fail_activity = true;
	CHECK(u_macos_hosted_client_set_active(b.client, false) == XRT_ERROR_IPC_FAILURE);
	CHECK(u_macos_hosted_client_present_ticket(b.client) == 0);
	CHECK(u_macos_hosted_client_set_visibility(b.client, U_MACOS_DISPLAY_HOST_SHOWN) != XRT_SUCCESS);
	b.fail_activity = false;
	CHECK(u_macos_hosted_client_set_active(b.client, false) == XRT_SUCCESS);
}

TEST_CASE("a failed hide still invalidates queued promotion")
{
	Binding b;
	b.start();
	auto ticket = u_macos_hosted_client_present_ticket(b.client);
	b.fail_visibility = true;
	CHECK(u_macos_hosted_client_set_visibility(b.client, U_MACOS_DISPLAY_HOST_HIDDEN) == XRT_ERROR_IPC_FAILURE);
	b.calls.clear();
	u_macos_hosted_client_note_presented(b.client, ticket);
	CHECK(b.calls.empty());
}

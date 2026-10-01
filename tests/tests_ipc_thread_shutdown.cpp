// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "server/ipc_server.h"
#include "server/ipc_server_thread_shutdown.h"
#include "catch_amalgamated.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <cwchar>
#include <mutex>

namespace {
struct Fixture
{
	std::unique_ptr<ipc_server> server{new ipc_server{}};
	std::mutex mutex;
	std::condition_variable condition;
	bool start_allowed = true;
	bool cancelled = false;
	unsigned started = 0;
	unsigned exited = 0;
	unsigned cancel_calls = 0;
	unsigned client_count = 0;
#ifdef XRT_OS_WINDOWS
	HANDLE pipe = INVALID_HANDLE_VALUE;
#endif
	struct Client
	{
		Fixture *fixture;
		unsigned slot;
	} clients[2]{};

	Fixture()
	{
		os_mutex_init(&server->global_state.lock);
		server->callback_data = this;
	}
	~Fixture()
	{
		ipc_server_stop_and_join_client_threads(server.get(), cancel);
		os_mutex_destroy(&server->global_state.lock);
	}
	static void *
	run(void *ptr)
	{
		auto &client = *static_cast<Client *>(ptr);
		auto &f = *client.fixture;
		auto &thread = f.server->threads[client.slot];
		std::unique_lock<std::mutex> lock(f.mutex);
		f.condition.wait(lock, [&] { return f.start_allowed; });
		ipc_server_client_thread_set_running(f.server.get(), &thread);
		++f.started;
		f.condition.notify_all();
		// Model a client stuck in a blocking read until its transport is cancelled.
		if (thread.state == IPC_THREAD_RUNNING) {
#ifdef XRT_OS_WINDOWS
			if (f.pipe != INVALID_HANDLE_VALUE) {
				lock.unlock();
				char byte;
				DWORD bytes;
				ReadFile(f.pipe, &byte, 1, &bytes, nullptr);
				lock.lock();
			} else
#endif
			{
				f.condition.wait(lock, [&] { return f.cancelled; });
			}
		}
		++f.exited;
		return nullptr;
	}
	void
	start(unsigned count, bool defer_start = false)
	{
		client_count = count;
		start_allowed = !defer_start;
		for (unsigned i = 0; i < count; ++i) {
			clients[i] = {this, i};
			server->threads[i].state = IPC_THREAD_STARTING;
			REQUIRE(os_thread_start(&server->threads[i].thread, run, &clients[i]) == 0);
		}
		if (!defer_start) {
			std::unique_lock<std::mutex> lock(mutex);
			REQUIRE(condition.wait_for(lock, std::chrono::seconds(2), [&] { return started == count; }));
		}
	}
	static void
	cancel(ipc_server *server)
	{
		auto &f = *static_cast<Fixture *>(server->callback_data);
		// This callback must be called after every loop is stopped and before any join.
		for (unsigned i = 0; i < f.client_count; ++i) {
			CHECK(server->threads[i].state == IPC_THREAD_STOPPING);
		}
#ifdef XRT_OS_WINDOWS
		if (f.pipe != INVALID_HANDLE_VALUE) {
			ipc_server_cancel_all_client_io(server);
		}
#endif
		std::lock_guard<std::mutex> lock(f.mutex);
		++f.cancel_calls;
		f.cancelled = true;
		f.start_allowed = true;
		f.condition.notify_all();
	}
};
} // namespace

TEST_CASE("server shutdown cancels all blocked clients before joining and is repeatable")
{
	Fixture f;
	f.start(2);
	ipc_server_stop_and_join_client_threads(f.server.get(), Fixture::cancel);
	CHECK(f.exited == 2);
	CHECK(f.cancel_calls == 1);
	CHECK(f.server->threads[0].state == IPC_THREAD_READY);
	CHECK(f.server->threads[1].state == IPC_THREAD_READY);
	ipc_server_stop_and_join_client_threads(f.server.get(), Fixture::cancel);
	CHECK(f.cancel_calls == 1);
	CHECK(f.exited == 2);
}

TEST_CASE("shutdown prevents a late client startup from reviving its stopped loop")
{
	Fixture f;
	f.start(1, true);
	ipc_server_stop_and_join_client_threads(f.server.get(), Fixture::cancel);
	CHECK(f.started == 1);
	CHECK(f.exited == 1);
	CHECK(f.server->threads[0].state == IPC_THREAD_READY);
}

TEST_CASE("shutdown joins an already stopping thread once")
{
	Fixture f;
	f.start(1);
	{
		std::lock_guard<std::mutex> lock(f.mutex);
		f.server->threads[0].state = IPC_THREAD_STOPPING;
		f.cancelled = true;
		f.condition.notify_all();
	}
	ipc_server_stop_and_join_client_threads(f.server.get(), Fixture::cancel);
	CHECK(f.exited == 1);
	CHECK(f.server->threads[0].state == IPC_THREAD_READY);
	ipc_server_stop_and_join_client_threads(f.server.get(), Fixture::cancel);
	CHECK(f.cancel_calls == 1);
}

TEST_CASE("shutdown with no client threads needs no cancellation")
{
	Fixture f;
	ipc_server_stop_and_join_client_threads(f.server.get(), Fixture::cancel);
	CHECK(f.cancel_calls == 0);
}

#ifdef XRT_OS_WINDOWS
TEST_CASE("Windows server shutdown releases a synchronous named-pipe reader")
{
	struct Pipe
	{
		HANDLE server = INVALID_HANDLE_VALUE;
		HANDLE client = INVALID_HANDLE_VALUE;
		~Pipe()
		{
			if (client != INVALID_HANDLE_VALUE)
				CloseHandle(client);
			if (server != INVALID_HANDLE_VALUE)
				CloseHandle(server);
		}
	} pipe;
	Fixture f;
	wchar_t name[128];
	swprintf(name, sizeof(name) / sizeof(name[0]), L"\\\\.\\pipe\\monado_shutdown_test_%lu", GetCurrentProcessId());
	pipe.server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
	                               1, 4096, 4096, 0, nullptr);
	REQUIRE(pipe.server != INVALID_HANDLE_VALUE);
	pipe.client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
	REQUIRE(pipe.client != INVALID_HANDLE_VALUE);
	BOOL connected = ConnectNamedPipe(pipe.server, nullptr);
	REQUIRE((connected || GetLastError() == ERROR_PIPE_CONNECTED));
	f.pipe = pipe.server;
	f.server->threads[0].ics.imc.ipc_handle = pipe.server;
	f.start(1);
	ipc_server_stop_and_join_client_threads(f.server.get(), ipc_server_cancel_all_client_io);
	CHECK(f.exited == 1);
	CHECK(f.server->threads[0].state == IPC_THREAD_READY);
	ipc_server_stop_and_join_client_threads(f.server.get(), ipc_server_cancel_all_client_io);
}
#endif

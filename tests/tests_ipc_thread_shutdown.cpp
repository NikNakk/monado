// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Regression tests for IPC client thread shutdown.
 * @author Nick Kennedy <code@nick-kennedy.com>
 */

#include "server/ipc_server.h"
#include "server/ipc_server_thread_shutdown.h"
#include "catch_amalgamated.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <cwchar>
#include <mutex>
#include <thread>

namespace {
struct Fixture
{
	std::unique_ptr<ipc_server> server{new ipc_server{}};
	std::mutex mutex;
	std::condition_variable condition;
	bool start_allowed = true;
	bool revived = false;
	unsigned started = 0;
	unsigned exited = 0;
#ifdef XRT_OS_WINDOWS
	HANDLE pipe = INVALID_HANDLE_VALUE;
	HANDLE pipe_thread = nullptr;
	bool read_allowed = true;
	bool read_attempted = false;
	BOOL read_succeeded = TRUE;
	DWORD read_error = ERROR_SUCCESS;
#endif
	struct Client
	{
		Fixture *fixture;
		unsigned slot;
	} clients[2]{};

	Fixture()
	{
		os_mutex_init(&server->global_state.lock);
#ifdef XRT_OS_WINDOWS
		for (auto &thread : server->threads) {
			thread.ics.imc.ipc_handle = XRT_IPC_HANDLE_INVALID;
		}
#endif
	}
	~Fixture()
	{
		ipc_server_stop_and_join_client_threads(server.get());
		os_mutex_destroy(&server->global_state.lock);
#ifdef XRT_OS_WINDOWS
		if (pipe_thread != nullptr) {
			CloseHandle(pipe_thread);
		}
#endif
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
		bool running = ipc_server_client_thread_is_running(f.server.get(), &thread);
		f.revived |= running;
#ifdef XRT_OS_WINDOWS
		if (f.pipe != INVALID_HANDLE_VALUE) {
			f.pipe_thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
		}
#endif
		++f.started;
		f.condition.notify_all();
		// Model the platform client loop: Windows blocks in a pipe read, other platforms poll.
		if (running) {
#ifdef XRT_OS_WINDOWS
			if (f.pipe != INVALID_HANDLE_VALUE) {
				// Capture RUNNING before waiting: model a read starting after cancellation.
				f.condition.wait(lock, [&] { return f.read_allowed; });
				f.read_attempted = true;
				lock.unlock();
				char byte;
				DWORD bytes;
				f.read_succeeded = ReadFile(f.pipe, &byte, 1, &bytes, nullptr);
				f.read_error = f.read_succeeded ? ERROR_SUCCESS : GetLastError();
				lock.lock();
			} else
#endif
			{
				lock.unlock();
				while (ipc_server_client_thread_is_running(f.server.get(), &thread)) {
					std::this_thread::yield();
				}
				lock.lock();
			}
		}
		++f.exited;
		return nullptr;
	}
	void
	release_client_gates()
	{
		std::lock_guard<std::mutex> lock(mutex);
		start_allowed = true;
#ifdef XRT_OS_WINDOWS
		read_allowed = true;
#endif
		condition.notify_all();
	}
	void
	start(unsigned count, bool defer_start = false)
	{
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
};
} // namespace

TEST_CASE("server shutdown stops all client loops before joining and is repeatable")
{
	Fixture f;
	f.start(2);
	ipc_server_stop_and_join_client_threads(f.server.get());
	CHECK(f.exited == 2);
	CHECK(f.server->threads[0].state == IPC_THREAD_READY);
	CHECK(f.server->threads[1].state == IPC_THREAD_READY);
	ipc_server_stop_and_join_client_threads(f.server.get());
	CHECK(f.exited == 2);
}

TEST_CASE("shutdown prevents a late client startup from reviving its stopped loop")
{
	Fixture f;
	f.start(1, true);
	std::thread release_start([&] {
		while (true) {
			os_mutex_lock(&f.server->global_state.lock);
			bool stopping = f.server->threads[0].state == IPC_THREAD_STOPPING;
			os_mutex_unlock(&f.server->global_state.lock);
			if (stopping) {
				break;
			}
			std::this_thread::yield();
		}
		f.release_client_gates();
	});
	ipc_server_stop_and_join_client_threads(f.server.get());
	release_start.join();
	CHECK(f.started == 1);
	CHECK(f.exited == 1);
	CHECK_FALSE(f.revived);
	CHECK(f.server->threads[0].state == IPC_THREAD_READY);
}

TEST_CASE("shutdown joins an already stopping thread once")
{
	Fixture f;
	f.start(1);
	{
		std::lock_guard<std::mutex> lock(f.mutex);
		os_mutex_lock(&f.server->global_state.lock);
		f.server->threads[0].state = IPC_THREAD_STOPPING;
		os_mutex_unlock(&f.server->global_state.lock);
	}
	ipc_server_stop_and_join_client_threads(f.server.get());
	CHECK(f.exited == 1);
	CHECK(f.server->threads[0].state == IPC_THREAD_READY);
	ipc_server_stop_and_join_client_threads(f.server.get());
}

TEST_CASE("shutdown with no client threads is harmless")
{
	Fixture f;
	ipc_server_stop_and_join_client_threads(f.server.get());
}

#ifdef XRT_OS_WINDOWS
TEST_CASE("Windows server shutdown releases synchronous named-pipe reads")
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
	SECTION("a read already blocked in the kernel")
	{
		f.start(1);
		REQUIRE(f.pipe_thread != nullptr);
		BOOL pending = FALSE;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		do {
			REQUIRE(GetThreadIOPendingFlag(f.pipe_thread, &pending));
			if (pending) {
				break;
			}
			Sleep(1);
		} while (std::chrono::steady_clock::now() < deadline);
		REQUIRE(pending);
		ipc_server_stop_and_join_client_threads(f.server.get());
	}
	SECTION("a read starting after cancellation and disconnection")
	{
		f.read_allowed = false;
		f.start(1);
		std::thread release_read([&] {
			// The client end observes disconnection only after shutdown cancels I/O.
			while (PeekNamedPipe(pipe.client, nullptr, 0, nullptr, nullptr, nullptr)) {
				std::this_thread::yield();
			}
			f.release_client_gates();
		});
		ipc_server_stop_and_join_client_threads(f.server.get());
		release_read.join();
	}
	CHECK(f.read_attempted);
	CHECK_FALSE(f.read_succeeded);
	CHECK((f.read_error == ERROR_OPERATION_ABORTED || f.read_error == ERROR_PIPE_NOT_CONNECTED ||
	       f.read_error == ERROR_BROKEN_PIPE));
	CHECK(f.exited == 1);
	CHECK(f.server->threads[0].state == IPC_THREAD_READY);
	ipc_server_stop_and_join_client_threads(f.server.get());
}
#endif

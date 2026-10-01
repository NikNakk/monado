// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif
#include "shared/ipc_tcp_auth.h"
#include "catch_amalgamated.hpp"
#include <thread>
#include <cstring>
#include <chrono>

namespace {
struct Pair
{
#ifdef _WIN32
	WSADATA wsa{};
	SOCKET fd[2]{INVALID_SOCKET, INVALID_SOCKET};
	Pair()
	{
		REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
		SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
		REQUIRE(listener != INVALID_SOCKET);
		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		REQUIRE(bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
		REQUIRE(listen(listener, 1) == 0);
		int size = sizeof(addr);
		REQUIRE(getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &size) == 0);
		fd[1] = socket(AF_INET, SOCK_STREAM, 0);
		REQUIRE(fd[1] != INVALID_SOCKET);
		REQUIRE(connect(fd[1], reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
		fd[0] = accept(listener, nullptr, nullptr);
		closesocket(listener);
		REQUIRE(fd[0] != INVALID_SOCKET);
	}
	~Pair()
	{
		for (auto socket : fd)
			closesocket(socket);
		WSACleanup();
	}
	xrt_ipc_handle_t
	handle(unsigned i)
	{
		return reinterpret_cast<xrt_ipc_handle_t>(fd[i]);
	}
	void
	finish()
	{
		shutdown(fd[0], SD_BOTH);
	}
#else
	int fd[2]{-1, -1};
	Pair()
	{
		REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
	}
	~Pair()
	{
		for (int socket : fd)
			if (socket >= 0)
				close(socket);
	}
	xrt_ipc_handle_t
	handle(unsigned i)
	{
		return fd[i];
	}
	void
	finish()
	{
		shutdown(fd[0], SHUT_RDWR);
	}
#endif
};
const char *secret = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
} // namespace
TEST_CASE("Wine challenge authentication accepts matching keys and rejects mismatches")
{
	for (bool match : {true, false}) {
		Pair pair;
		bool accepted = false;
		std::thread server([&] {
			accepted = ipc_tcp_authenticate_server(pair.handle(0), secret, 1000);
			pair.finish();
		});
		bool result = ipc_tcp_authenticate_client(
		    pair.handle(1), match ? secret : "1123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
		    1000);
		server.join();
		CHECK(accepted == match);
		CHECK(result == match);
	}
}
TEST_CASE("Wine authentication rejects missing keys and times out incomplete clients")
{
	CHECK_FALSE(ipc_tcp_auth_token_valid(nullptr));
	CHECK_FALSE(ipc_tcp_auth_token_valid("short"));
	Pair pair;
	CHECK_FALSE(ipc_tcp_authenticate_server(pair.handle(0), secret, 20));
	REQUIRE(send(pair.fd[1], secret, 7, 0) == 7);
	CHECK_FALSE(ipc_tcp_authenticate_server(pair.handle(0), secret, 20));
}
TEST_CASE("an impostor server cannot elicit a client proof or learn its key")
{
	Pair pair;
	bool received_nonce = false;
	bool received_proof = false;
	std::thread impostor([&] {
		char nonce[32];
		unsigned received = 0;
		while (received < sizeof(nonce)) {
			int count = (int)recv(pair.fd[0], nonce + received, (int)(sizeof(nonce) - received), 0);
			if (count <= 0)
				return;
			received += (unsigned)count;
		}
		received_nonce = true;
		char invalid_response[64]{};
		send(pair.fd[0], invalid_response, sizeof(invalid_response), 0);
		char proof[32];
		received_proof = recv(pair.fd[0], proof, sizeof(proof), 0) > 0;
	});
	bool accepted = ipc_tcp_authenticate_client(pair.handle(1), secret, 1000);
#ifdef _WIN32
	shutdown(pair.fd[1], SD_BOTH);
#else
	shutdown(pair.fd[1], SHUT_RDWR);
#endif
	impostor.join();
	CHECK_FALSE(accepted);
	CHECK(received_nonce);
	CHECK_FALSE(received_proof);
}

TEST_CASE("server authentication rejects reflecting its own proof as a client proof")
{
	Pair pair;
	bool accepted = true;
	std::thread server([&] {
		accepted = ipc_tcp_authenticate_server(pair.handle(0), secret, 1000);
		pair.finish();
	});
	char nonce[32]{};
	send(pair.fd[1], nonce, sizeof(nonce), 0);
	char response[64]{};
	unsigned received = 0;
	while (received < sizeof(response)) {
		int count = (int)recv(pair.fd[1], response + received, (int)(sizeof(response) - received), 0);
		if (count <= 0)
			break;
		received += (unsigned)count;
	}
	if (received == sizeof(response))
		send(pair.fd[1], response + 32, 32, 0);
	server.join();
	CHECK(received == sizeof(response));
	CHECK_FALSE(accepted);
}

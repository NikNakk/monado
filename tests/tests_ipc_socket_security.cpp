// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "shared/ipc_socket_security.h"
#include "shared/ipc_tcp_auth.h"
#include "util/u_file.h"
#include <sys/stat.h>
#include <fstream>
#include "catch_amalgamated.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <thread>
#include <cstring>
#include <cstdio>
#include <string>

namespace {
struct Pair
{
	int fd[2]{-1, -1};
	Pair()
	{
		REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
	}
	~Pair()
	{
		for (int i : fd)
			if (i >= 0)
				close(i);
	}
};
struct Endpoint
{
	char dir[64] = "/tmp/monado-socket-test-XXXXXX";
	std::string path;
	int fd = -1, second = -1, lock = -1, other_lock = -1;
	Endpoint()
	{
		REQUIRE(mkdtemp(dir) != nullptr);
		path = std::string(dir) + "/socket";
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		second = socket(AF_UNIX, SOCK_STREAM, 0);
		REQUIRE(fd >= 0);
		REQUIRE(second >= 0);
	}
	~Endpoint()
	{
		close(fd);
		close(second);
		unlink(path.c_str());
		if (lock >= 0)
			close(lock);
		if (other_lock >= 0)
			close(other_lock);
		unlink((path + ".lock").c_str());
		rmdir(dir);
	}
};
} // namespace

TEST_CASE("IPC peer identity comes from the connected socket")
{
	Pair pair;
	uid_t uid = 0;
	pid_t pid = 0;
	REQUIRE(ipc_socket_get_peer_identity(pair.fd[0], &uid, &pid));
	CHECK(uid == getuid());
	CHECK(pid == getpid());
	CHECK_FALSE(ipc_socket_get_peer_identity(-1, &uid, &pid));
}
TEST_CASE("second server cannot steal a live socket endpoint")
{
	Endpoint e;
	REQUIRE(ipc_socket_bind_exclusive(e.fd, e.path.c_str(), &e.lock) == 0);
	REQUIRE(listen(e.fd, 1) == 0);
	CHECK(ipc_socket_bind_exclusive(e.second, e.path.c_str(), &e.other_lock) == -1);
	CHECK(e.other_lock == -1);
	int client = socket(AF_UNIX, SOCK_STREAM, 0);
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::strcpy(addr.sun_path, e.path.c_str());
	CHECK(connect(client, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	close(client);
}
TEST_CASE("a live legacy endpoint is preserved and a stale one is recovered")
{
	Endpoint e;
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::strcpy(addr.sun_path, e.path.c_str());
	REQUIRE(bind(e.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
	REQUIRE(listen(e.fd, 1) == 0);
	CHECK(ipc_socket_bind_exclusive(e.second, e.path.c_str(), &e.other_lock) == -1);
	close(e.fd);
	e.fd = -1;
	CHECK(ipc_socket_bind_exclusive(e.second, e.path.c_str(), &e.other_lock) == 0);
}
TEST_CASE("private configuration replacement does not follow a destination symlink")
{
	Endpoint e;
	std::string victim = std::string(e.dir) + "/other";
	{
		std::ofstream file(victim);
		file << "unchanged";
	}
	REQUIRE(symlink(victim.c_str(), e.path.c_str()) == 0);
	const char data[] = "private configuration";
	REQUIRE(u_file_write_private_atomic(e.path.c_str(), data, sizeof(data)) == 0);
	struct stat st{};
	REQUIRE(lstat(e.path.c_str(), &st) == 0);
	CHECK(S_ISREG(st.st_mode));
	CHECK((st.st_mode & 0777) == 0600);
	std::ifstream file(victim);
	std::string value;
	file >> value;
	CHECK(value == "unchanged");
	unlink(victim.c_str());
}

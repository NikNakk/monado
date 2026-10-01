// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#if defined(_WIN32)
#include <winsock2.h>
#endif
#include "shared/ipc_tcp_auth.h"
#include "os/os_time.h"
#include <stdint.h>
#include <string.h>
#include <errno.h>
#ifndef XRT_OS_WINDOWS
#include <poll.h>
#include <sys/socket.h>
#endif

#ifdef XRT_OS_WINDOWS
#include <bcrypt.h>
#elif defined(XRT_OS_OSX)
#include <CommonCrypto/CommonHMAC.h>
#include <stdlib.h>
#elif defined(IPC_TCP_AUTH_OPENSSL)
#include <openssl/hmac.h>
#include <openssl/rand.h>
#endif

static bool
random_nonce(unsigned char nonce[32])
{
#ifdef XRT_OS_WINDOWS
	return BCryptGenRandom(NULL, nonce, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(XRT_OS_OSX)
	arc4random_buf(nonce, 32);
	return true;
#elif defined(IPC_TCP_AUTH_OPENSSL)
	return RAND_bytes(nonce, 32) == 1;
#else
	(void)nonce;
	return false;
#endif
}

static bool
make_proof(const char *token,
           const char role[6],
           const unsigned char client[32],
           const unsigned char server[32],
           unsigned char proof[32])
{
	unsigned char input[70];
	memcpy(input, role, 6);
	memcpy(input + 6, client, 32);
	memcpy(input + 38, server, 32);
#ifdef XRT_OS_WINDOWS
	BCRYPT_ALG_HANDLE algorithm = NULL;
	BCRYPT_HASH_HANDLE hash = NULL;
	bool success =
	    BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0 &&
	    BCryptCreateHash(algorithm, &hash, NULL, 0, (PUCHAR)token, IPC_TCP_AUTH_TOKEN_SIZE, 0) == 0 &&
	    BCryptHashData(hash, input, sizeof(input), 0) == 0 && BCryptFinishHash(hash, proof, 32, 0) == 0;
	if (hash != NULL)
		BCryptDestroyHash(hash);
	if (algorithm != NULL)
		BCryptCloseAlgorithmProvider(algorithm, 0);
	return success;
#elif defined(XRT_OS_OSX)
	CCHmac(kCCHmacAlgSHA256, token, IPC_TCP_AUTH_TOKEN_SIZE, input, sizeof(input), proof);
	return true;
#elif defined(IPC_TCP_AUTH_OPENSSL)
	unsigned int size = 0;
	return HMAC(EVP_sha256(), token, IPC_TCP_AUTH_TOKEN_SIZE, input, sizeof(input), proof, &size) != NULL &&
	       size == 32;
#else
	(void)token;
	(void)proof;
	return false;
#endif
}

static bool
proof_matches(const unsigned char a[32], const unsigned char b[32])
{
	unsigned difference = 0;
	for (size_t i = 0; i < 32; i++)
		difference |= a[i] ^ b[i];
	return difference == 0;
}

bool
ipc_tcp_auth_token_valid(const char *token)
{
	if (token == NULL || strlen(token) != IPC_TCP_AUTH_TOKEN_SIZE)
		return false;
	for (size_t i = 0; i < IPC_TCP_AUTH_TOKEN_SIZE; i++) {
		if (!((token[i] >= '0' && token[i] <= '9') || (token[i] >= 'a' && token[i] <= 'f')))
			return false;
	}
	return true;
}

static bool
transfer(xrt_ipc_handle_t socket, char *bytes, size_t size, bool sending, uint64_t deadline)
{
	size_t offset = 0;
	while (offset < size) {
		uint64_t now = os_monotonic_get_ns();
		if (now >= deadline)
			return false;
		int ms = (int)((deadline - now + 999999) / 1000000);
#ifdef XRT_OS_WINDOWS
		SOCKET fd = (SOCKET)(uintptr_t)socket;
		fd_set set;
		FD_ZERO(&set);
		FD_SET(fd, &set);
		struct timeval timeout = {ms / 1000, (ms % 1000) * 1000};
		int ready = select(0, sending ? NULL : &set, sending ? &set : NULL, NULL, &timeout);
		if (ready <= 0)
			return false;
		int count = sending ? send(fd, bytes + offset, (int)(size - offset), 0)
		                    : recv(fd, bytes + offset, (int)(size - offset), 0);
		if (count <= 0)
			return false;
#else
		struct pollfd event = {.fd = socket, .events = sending ? POLLOUT : POLLIN};
		int ready = poll(&event, 1, ms);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0)
			return false;
		int flags = MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
		if (sending)
			flags |= MSG_NOSIGNAL;
#endif
#ifdef SO_NOSIGPIPE
		int one = 1;
		if (sending && setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0)
			return false;
#endif
		ssize_t count = sending ? send(socket, bytes + offset, size - offset, flags)
		                        : recv(socket, bytes + offset, size - offset, flags);
		if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (count <= 0)
			return false;
#endif
		offset += (size_t)count;
	}
	return true;
}


bool
ipc_tcp_authenticate_server(xrt_ipc_handle_t socket, const char *token, int timeout_ms)
{
	if (!ipc_tcp_auth_token_valid(token) || timeout_ms <= 0)
		return false;
	uint64_t deadline = os_monotonic_get_ns() + (uint64_t)timeout_ms * 1000000;
	unsigned char client[32], response[64], proof[32], expected[32];
	if (!transfer(socket, (char *)client, sizeof(client), false, deadline) || !random_nonce(response) ||
	    !make_proof(token, "server", client, response, response + 32) ||
	    !transfer(socket, (char *)response, sizeof(response), true, deadline) ||
	    !transfer(socket, (char *)proof, sizeof(proof), false, deadline) ||
	    !make_proof(token, "client", client, response, expected) || !proof_matches(proof, expected))
		return false;
	char accepted = 1;
	return transfer(socket, &accepted, 1, true, deadline);
}

bool
ipc_tcp_authenticate_client(xrt_ipc_handle_t socket, const char *token, int timeout_ms)
{
	if (!ipc_tcp_auth_token_valid(token) || timeout_ms <= 0)
		return false;
	uint64_t deadline = os_monotonic_get_ns() + (uint64_t)timeout_ms * 1000000;
	unsigned char client[32], response[64], expected[32], proof[32];
	if (!random_nonce(client) || !transfer(socket, (char *)client, sizeof(client), true, deadline) ||
	    !transfer(socket, (char *)response, sizeof(response), false, deadline) ||
	    !make_proof(token, "server", client, response, expected) || !proof_matches(response + 32, expected) ||
	    !make_proof(token, "client", client, response, proof) ||
	    !transfer(socket, (char *)proof, sizeof(proof), true, deadline))
		return false;
	char accepted = 0;
	return transfer(socket, &accepted, 1, false, deadline) && accepted == 1;
}

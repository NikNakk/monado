// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "xrt/xrt_handles.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_TCP_AUTH_TOKEN_SIZE 64

//! Secrets must contain 256 bits encoded as 64 lowercase hexadecimal characters.
bool
ipc_tcp_auth_token_valid(const char *token);

//! Authenticate before any framed IPC traffic, with a deadline for the whole handshake.
bool
ipc_tcp_authenticate_server(xrt_ipc_handle_t socket, const char *token, int timeout_ms);
bool
ipc_tcp_authenticate_client(xrt_ipc_handle_t socket, const char *token, int timeout_ms);

#ifdef __cplusplus
}
#endif

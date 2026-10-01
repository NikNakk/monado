// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdbool.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Read kernel-supplied credentials, never application metadata.
bool
ipc_socket_get_peer_identity(int fd, uid_t *uid, pid_t *pid);

/*!
 * Bind a Unix socket while holding an exclusive lock for its lifetime.
 * A listening endpoint is never removed. Keep the returned lock fd open until
 * after closing and unlinking the socket; the lock file itself stays in place.
 */
int
ipc_socket_bind_exclusive(int fd, const char *path, int *out_lock_fd);

#ifdef __cplusplus
}
#endif

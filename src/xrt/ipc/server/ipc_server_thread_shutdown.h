// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "xrt/xrt_config_os.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ipc_server;
struct ipc_thread;

#ifdef XRT_OS_WINDOWS
//! Cancel pending reads and disconnect the server pipes before joining clients.
void
ipc_server_cancel_all_client_io(struct ipc_server *s);
#endif

//! Do not revive a starting thread that shutdown has already stopped.
void
ipc_server_client_thread_set_running(struct ipc_server *s, struct ipc_thread *it);

/*!
 * Stop all client loops, wake blocking I/O, then join each thread once.
 * Called by the server main thread after it stops accepting connections.
 * The optional cancellation callback runs without the global state lock held.
 * Safe to repeat: joined slots are READY and no longer own a thread.
 */
void
ipc_server_stop_and_join_client_threads(struct ipc_server *s, void (*cancel_io)(struct ipc_server *s));

#ifdef __cplusplus
}
#endif

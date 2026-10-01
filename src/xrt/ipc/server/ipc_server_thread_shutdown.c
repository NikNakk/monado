// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "server/ipc_server.h"
#include "server/ipc_server_thread_shutdown.h"

#ifdef XRT_OS_WINDOWS
/*!
 * Cancel the blocking read each client thread is sitting in.
 *
 * The epoll based client loop wakes up on its own timeout and so notices
 * their thread entering STOPPING, but the Windows one blocks in ReadFile() with no
 * timeout and would only notice once the client happens to send something.
 * Cancelling the pending I/O makes that read fail so the thread can run its
 * shutdown and be joined.
 *
 * The client thread closes this handle in common_shutdown() while holding the
 * global state lock, so take it here as well, otherwise we could cancel I/O on
 * a handle that has just been closed and possibly reused.
 */
void
ipc_server_cancel_all_client_io(struct ipc_server *s)
{
	os_mutex_lock(&s->global_state.lock);

	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		struct ipc_thread *it = &s->threads[i];
		if (it->state == IPC_THREAD_READY) {
			continue;
		}

		xrt_ipc_handle_t ipc_handle = it->ics.imc.ipc_handle;
		if (!xrt_ipc_handle_is_valid(ipc_handle)) {
			continue;
		}

		CancelIoEx(ipc_handle, NULL);
		// Also prevent a read starting just after cancellation from blocking.
		// This is the server end of a named pipe, owned by the client thread.
		DisconnectNamedPipe(ipc_handle);
	}

	os_mutex_unlock(&s->global_state.lock);
}
#endif // XRT_OS_WINDOWS

void
ipc_server_client_thread_set_running(struct ipc_server *s, struct ipc_thread *it)
{
	os_mutex_lock(&s->global_state.lock);
	if (it->state == IPC_THREAD_STARTING) {
		it->state = IPC_THREAD_RUNNING;
	}
	os_mutex_unlock(&s->global_state.lock);
}

void
ipc_server_stop_and_join_client_threads(struct ipc_server *s, void (*cancel_io)(struct ipc_server *s))
{
	bool have_threads = false;
	os_mutex_lock(&s->global_state.lock);
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		struct ipc_thread *it = &s->threads[i];
		if (it->state != IPC_THREAD_READY) {
			it->state = IPC_THREAD_STOPPING;
			have_threads = true;
		}
	}
	os_mutex_unlock(&s->global_state.lock);

	if (!have_threads) {
		return;
	}
	// Wake every blocked read before waiting for any one client to finish.
	if (cancel_io != NULL) {
		cancel_io(s);
	}
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		struct ipc_thread *it = &s->threads[i];
		if (it->state == IPC_THREAD_READY) {
			continue;
		}
		os_thread_join(&it->thread);
		os_thread_destroy(&it->thread);
		os_mutex_lock(&s->global_state.lock);
		it->state = IPC_THREAD_READY;
		os_mutex_unlock(&s->global_state.lock);
	}
}

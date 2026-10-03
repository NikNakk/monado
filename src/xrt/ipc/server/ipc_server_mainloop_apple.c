// Copyright 2025, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Server mainloop details on macOS.
 * @author OpenAI Codex
 * @ingroup ipc_server
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_config_have.h"
#include "xrt/xrt_config_os.h"

#include "os/os_time.h"
#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_trace_marker.h"
#include "util/u_timing_trace.h"
#include "util/u_file.h"
#include "util/u_truncate_printf.h"

#include "shared/ipc_shmem.h"
#include "shared/ipc_socket_security.h"
#include "server/ipc_server.h"

#import <AppKit/AppKit.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>


/*
 *
 * Static functions.
 *
 */

static int
create_listen_socket(struct ipc_server_mainloop *ml, int *out_fd)
{
	struct sockaddr_un addr = XRT_STRUCT_INIT;
	int fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		U_LOG_E("Message Socket Create Error!");
		return fd;
	}

	char sock_file[PATH_MAX];
	int size = u_file_get_path_in_runtime_dir(XRT_IPC_MSG_SOCK_FILENAME, sock_file, PATH_MAX);
	if (size == -1) {
		U_LOG_E("Could not get socket file name");
		close(fd);
		return -1;
	}

	const int dst_size = (int)ARRAY_SIZE(addr.sun_path);
	if (size >= dst_size) {
		U_LOG_E("Total IPC path too long (%i > %i)", size, dst_size);
		close(fd);
		return -1;
	}

	int ret = ipc_socket_bind_exclusive(fd, sock_file, &ml->socket_lock_fd);

	if (ret < 0) {
		U_LOG_E("Could not bind socket to path %s: %s. Is the service running already?", sock_file,
		        strerror(errno));
		close(fd);
		return ret;
	}

	ml->socket_filename = strdup(sock_file);
	if (ml->socket_filename == NULL) {
		unlink(sock_file);
		close(fd);
		return -1;
	}

	ret = listen(fd, IPC_MAX_CLIENTS);
	if (ret < 0) {
		close(fd);
		return ret;
	}

	U_LOG_D("Created listening socket %s.", sock_file);
	*out_fd = fd;
	return 0;
}

static int
init_listen_socket(struct ipc_server_mainloop *ml)
{
	int fd = -1;
	int ret = create_listen_socket(ml, &fd);
	if (ret < 0) {
		return ret;
	}

	ml->listen_socket = fd;
	U_LOG_D("Listening socket is fd %d", ml->listen_socket);
	return fd;
}

static volatile sig_atomic_t got_shutdown_signal = 0;

static void
shutdown_signal_handler(int sig)
{
	(void)sig;
	got_shutdown_signal = 1;
}

static void
install_signal_handlers(void)
{
	struct sigaction sa = {0};
	sa.sa_handler = shutdown_signal_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESETHAND;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
}

static void
handle_listen(struct ipc_server *vs, struct ipc_server_mainloop *ml)
{
	int ret = accept(ml->listen_socket, NULL, NULL);
	if (ret < 0) {
		U_LOG_E("accept '%i'", ret);
		ipc_server_handle_failure(vs);
		return;
	}

	ipc_server_handle_client_connected(vs, ret);
}

#define NO_SLEEP 0

/* Passive, decimated observation: no timer or transaction is introduced. */
static FILE *g_appkit_pump_trace;
static bool g_appkit_pump_trace_attempted;
static uint64_t g_appkit_pump_previous_begin_ns, g_appkit_pump_previous_end_ns, g_appkit_pump_last_row_ns;

static void
trace_appkit_pump(uint64_t begin_ns, uint64_t end_ns)
{
	if (!u_timing_trace_enabled()) {
		return;
	}
	if (!g_appkit_pump_trace_attempted) {
		g_appkit_pump_trace_attempted = true;
		g_appkit_pump_trace = u_timing_trace_open("appkit_pump", 64 * 1024);
		if (g_appkit_pump_trace != NULL) {
			fputs("begin_ns,end_ns,previous_begin_ns,previous_end_ns,nsapp_present,main_thread\n",
			      g_appkit_pump_trace);
		}
	}
	if (g_appkit_pump_trace != NULL &&
	    (end_ns - g_appkit_pump_last_row_ns >= 10000000 || end_ns - begin_ns >= 10000000)) {
		fprintf(g_appkit_pump_trace, "%llu,%llu,%llu,%llu,%u,%u\n", (unsigned long long)begin_ns,
		        (unsigned long long)end_ns, (unsigned long long)g_appkit_pump_previous_begin_ns,
		        (unsigned long long)g_appkit_pump_previous_end_ns, NSApp != nil ? 1u : 0u,
		        [NSThread isMainThread] ? 1u : 0u);
		if (!u_timing_trace_fully_buffered()) {
			fflush(g_appkit_pump_trace);
		}
		g_appkit_pump_last_row_ns = end_ns;
	}
	g_appkit_pump_previous_begin_ns = begin_ns;
	g_appkit_pump_previous_end_ns = end_ns;
}

static void
pump_appkit_events(void)
{
	if (NSApp == nil) {
		return;
	}

	@autoreleasepool {
		NSEvent *event = nil;
		while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
		                                   untilDate:[NSDate distantPast]
		                                      inMode:NSDefaultRunLoopMode
		                                     dequeue:YES]) != nil) {
			[NSApp sendEvent:event];
		}
		[NSApp updateWindows];
	}
}


/*
 *
 * Exported functions.
 *
 */

void
ipc_server_mainloop_apple_poll(struct ipc_server *vs, struct ipc_server_mainloop *ml)
{
	IPC_TRACE_MARKER();
	u_timing_trace_poll_flush_request();
	uint64_t pump_begin_ns = u_timing_trace_enabled() ? os_monotonic_get_ns() : 0;
	pump_appkit_events();
	if (pump_begin_ns != 0) {
		trace_appkit_pump(pump_begin_ns, os_monotonic_get_ns());
	}

	struct pollfd pollfds[2] = {0};
	nfds_t nfds = 0;

	if (!ml->no_stdin) {
		pollfds[nfds].fd = 0;
		pollfds[nfds].events = POLLIN;
		nfds++;
	}

	pollfds[nfds].fd = ml->listen_socket;
	pollfds[nfds].events = POLLIN;
	nfds++;

	int ret = poll(pollfds, nfds, NO_SLEEP);
	if (ret < 0) {
		if (errno == EINTR) {
			return;
		}
		U_LOG_E("poll failed with '%i'.", ret);
		ipc_server_handle_failure(vs);
		return;
	}

	if (got_shutdown_signal) {
		U_LOG_I("Got shutdown signal, shutting down.");
		ipc_server_handle_shutdown_signal(vs);
		return;
	}

	for (nfds_t i = 0; i < nfds; i++) {
		if ((pollfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
			if (pollfds[i].fd == ml->listen_socket) {
				U_LOG_E("Listen socket poll error, shutting down.");
				ipc_server_handle_failure(vs);
				return;
			}
			ipc_server_handle_shutdown_signal(vs);
			return;
		}

		if ((pollfds[i].revents & POLLIN) == 0) {
			continue;
		}

		if (pollfds[i].fd == 0) {
			ipc_server_handle_shutdown_signal(vs);
			return;
		}

		if (pollfds[i].fd == ml->listen_socket) {
			handle_listen(vs, ml);
		}
	}
}

int
ipc_server_mainloop_apple_init(struct ipc_server_mainloop *ml, bool no_stdin)
{
	IPC_TRACE_MARKER();

	ml->listen_socket = -1;
	ml->socket_lock_fd = -1;
	ml->socket_filename = NULL;
	ml->no_stdin = no_stdin;

	int ret = init_listen_socket(ml);
	if (ret < 0) {
		ipc_server_mainloop_apple_deinit(ml);
		return ret;
	}

	install_signal_handlers();
	return 0;
}

void
ipc_server_mainloop_apple_deinit(struct ipc_server_mainloop *ml)
{
	if (g_appkit_pump_trace != NULL) {
		fclose(g_appkit_pump_trace);
		g_appkit_pump_trace = NULL;
	}
	g_appkit_pump_trace_attempted = false;
	g_appkit_pump_previous_begin_ns = g_appkit_pump_previous_end_ns = g_appkit_pump_last_row_ns = 0;
	IPC_TRACE_MARKER();

	if (ml == NULL) {
		return;
	}
	if (ml->listen_socket >= 0) {
		close(ml->listen_socket);
		ml->listen_socket = -1;
	}
	if (ml->socket_filename != NULL) {
		unlink(ml->socket_filename);
		free(ml->socket_filename);
		ml->socket_filename = NULL;
	}
	if (ml->socket_lock_fd >= 0) {
		close(ml->socket_lock_fd);
		ml->socket_lock_fd = -1;
	}
}

#ifndef XRT_FEATURE_SERVICE
/*
 * Service builds wrap these with launchd/XPC lifecycle handling in
 * ipc_server_mainloop_apple_xpc.m; otherwise they are the main loop.
 */
int
ipc_server_mainloop_init(struct ipc_server_mainloop *ml, bool no_stdin)
{
	return ipc_server_mainloop_apple_init(ml, no_stdin);
}

void
ipc_server_mainloop_poll(struct ipc_server *vs, struct ipc_server_mainloop *ml)
{
	ipc_server_mainloop_apple_poll(vs, ml);
}

void
ipc_server_mainloop_deinit(struct ipc_server_mainloop *ml)
{
	ipc_server_mainloop_apple_deinit(ml);
}
#endif

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
#include "util/u_file.h"
#include "util/u_truncate_printf.h"

#include "shared/ipc_shmem.h"
#include "server/ipc_server.h"

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

	addr.sun_family = AF_UNIX;
	u_truncate_snprintf(addr.sun_path, dst_size, "%s", sock_file);

	int ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0 && errno == EADDRINUSE) {
		U_LOG_W("Removing stale socket file %s", sock_file);
		ret = unlink(sock_file);
		if (ret < 0) {
			U_LOG_E("Failed to remove stale socket file %s: %s", sock_file, strerror(errno));
			close(fd);
			return ret;
		}
		ret = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
	}

	if (ret < 0) {
		U_LOG_E("Could not bind socket to path %s: %s. Is the service running already?", sock_file,
		        strerror(errno));
		if (errno == EADDRINUSE) {
			U_LOG_E("If monado-service is not running, delete %s before starting a new instance", sock_file);
		}
		close(fd);
		return ret;
	}

	ml->socket_filename = strdup(sock_file);

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


/*
 *
 * Exported functions.
 *
 */

void
ipc_server_mainloop_poll(struct ipc_server *vs, struct ipc_server_mainloop *ml)
{
	IPC_TRACE_MARKER();

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
ipc_server_mainloop_init(struct ipc_server_mainloop *ml, bool no_stdin)
{
	IPC_TRACE_MARKER();

	ml->listen_socket = -1;
	ml->socket_filename = NULL;
	ml->no_stdin = no_stdin;

	int ret = init_listen_socket(ml);
	if (ret < 0) {
		ipc_server_mainloop_deinit(ml);
		return ret;
	}

	install_signal_handlers();
	return 0;
}

void
ipc_server_mainloop_deinit(struct ipc_server_mainloop *ml)
{
	IPC_TRACE_MARKER();

	if (ml == NULL) {
		return;
	}
	if (ml->listen_socket > 0) {
		close(ml->listen_socket);
		ml->listen_socket = -1;
	}
	if (ml->socket_filename != NULL) {
		U_LOG_W("Preserving Apple IPC socket path %s for WiVRn/macOS port bring-up", ml->socket_filename);
		free(ml->socket_filename);
		ml->socket_filename = NULL;
	}
}

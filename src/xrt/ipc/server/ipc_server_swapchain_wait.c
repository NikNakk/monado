// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Bounded swapchain image waits for IPC worker threads on macOS.
 * @ingroup ipc_server
 */

#include "os/os_time.h"

#include "server/ipc_server_swapchain_wait.h"

#include <poll.h>
#include <sys/socket.h>

/*!
 * The client sends nothing while it waits for a reply, so a readable or
 * hung-up socket during a wait means it has disconnected or died.
 */
static bool
client_hung_up(int fd)
{
	struct pollfd pfd = {.fd = fd, .events = POLLIN};
	if (poll(&pfd, 1, 0) <= 0) {
		return false;
	}
	if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
		return true;
	}
	char byte;
	return recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

xrt_result_t
ipc_server_swapchain_wait_in_slices(struct xrt_swapchain *xsc,
                                    int64_t timeout_ns,
                                    uint32_t index,
                                    int client_fd,
                                    const volatile bool *running)
{
	int64_t start_ns = os_monotonic_get_ns();

	for (;;) {
		int64_t elapsed_ns = os_monotonic_get_ns() - start_ns;
		int64_t remaining_ns = timeout_ns == INT64_MAX ? INT64_MAX
		                       : timeout_ns > elapsed_ns ? timeout_ns - elapsed_ns
		                                                 : 0;
		int64_t slice_ns =
		    remaining_ns < IPC_SERVER_SWAPCHAIN_WAIT_SLICE_NS ? remaining_ns : IPC_SERVER_SWAPCHAIN_WAIT_SLICE_NS;

		xrt_result_t xret = xrt_swapchain_wait_image(xsc, slice_ns, index);
		if (xret != XRT_TIMEOUT || remaining_ns <= slice_ns || !*running || client_hung_up(client_fd)) {
			return xret;
		}
	}
}

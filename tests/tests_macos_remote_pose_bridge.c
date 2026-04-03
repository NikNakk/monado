// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Bridge a tiny MVP head-pose packet into Monado's remote driver.
 */

#include "remote/r_interface.h"

#include "tests_macos_remote_pose_protocol.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static struct xrt_quat
rotate_y_180_quat(struct xrt_quat quat)
{
	quat.x = -quat.x;
	quat.z = -quat.z;
	return quat;
}

static struct xrt_vec3
rotate_y_180_vec3(struct xrt_vec3 vec)
{
	vec.x = -vec.x;
	vec.z = -vec.z;
	return vec;
}

static struct xrt_pose
rotate_y_180_pose(struct xrt_pose pose)
{
	pose.orientation = rotate_y_180_quat(pose.orientation);
	pose.position = rotate_y_180_vec3(pose.position);
	return pose;
}

static bool g_logged_v0_pose = false;
static bool g_logged_v1_pose = false;
static bool g_has_center_baseline = false;
static struct xrt_vec3 g_center_baseline = {0};
static bool g_logged_translation = false;

static void
maybe_log_translation(const struct xrt_vec3 *position)
{
	if (!g_has_center_baseline) {
		g_center_baseline = *position;
		g_has_center_baseline = true;
		return;
	}
	if (g_logged_translation) {
		return;
	}

	const float dx = position->x - g_center_baseline.x;
	const float dy = position->y - g_center_baseline.y;
	const float dz = position->z - g_center_baseline.z;
	const float distance_squared = dx * dx + dy * dy + dz * dz;
	if (distance_squared > (0.05f * 0.05f)) {
		fprintf(stdout, "Bridge translation detected: dx=%0.4f dy=%0.4f dz=%0.4f\n", dx, dy, dz);
		fflush(stdout);
		g_logged_translation = true;
	}
}

static int
fail_msg(const char *what)
{
	fprintf(stderr, "%s\n", what);
	return 1;
}

static uint16_t
get_udp_port(void)
{
	const char *value = getenv("MACOS_REMOTE_POSE_BRIDGE_UDP_PORT");
	if (value == NULL || value[0] == '\0') {
		return 4243;
	}

	errno = 0;
	char *end = NULL;
	long parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
		fprintf(stderr,
		        "Invalid MACOS_REMOTE_POSE_BRIDGE_UDP_PORT=%s, expected integer in [1,65535]\n",
		        value);
		return 0;
	}

	return (uint16_t)parsed;
}

static uint16_t
get_remote_driver_port(void)
{
	const char *value = getenv("MACOS_REMOTE_POSE_BRIDGE_REMOTE_PORT");
	if (value == NULL || value[0] == '\0') {
		return 4242;
	}

	errno = 0;
	char *end = NULL;
	long parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
		fprintf(stderr,
		        "Invalid MACOS_REMOTE_POSE_BRIDGE_REMOTE_PORT=%s, expected integer in [1,65535]\n",
		        value);
		return 0;
	}

	return (uint16_t)parsed;
}

static uint32_t
get_idle_timeout_ms(void)
{
	const char *value = getenv("MACOS_REMOTE_POSE_BRIDGE_IDLE_TIMEOUT_MS");
	if (value == NULL || value[0] == '\0') {
		return 3000;
	}

	errno = 0;
	char *end = NULL;
	long parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 60000) {
		fprintf(stderr,
		        "Invalid MACOS_REMOTE_POSE_BRIDGE_IDLE_TIMEOUT_MS=%s, expected integer in [1,60000]\n",
		        value);
		return 0;
	}

	return (uint32_t)parsed;
}

static void
apply_packet_to_remote_data(const struct macos_remote_pose_packet_v0 *packet, struct r_remote_data *data)
{
	data->header = R_HEADER_VALUE;
	data->head.per_view_data_valid = false;

	if ((packet->flags & MACOS_REMOTE_POSE_PACKET_ORIENTATION_VALID) != 0) {
		data->head.center.orientation = packet->orientation;
	}
	if ((packet->flags & MACOS_REMOTE_POSE_PACKET_POSITION_VALID) != 0) {
		data->head.center.position = packet->position;
	}
	if (!g_logged_v0_pose) {
		fprintf(stdout,
		        "Bridge v0 center position: x=%0.4f y=%0.4f z=%0.4f\n",
		        data->head.center.position.x,
		        data->head.center.position.y,
		        data->head.center.position.z);
		g_logged_v0_pose = true;
	}
	maybe_log_translation(&data->head.center.position);
}

static void
apply_packet_v1_to_remote_data(const struct macos_remote_pose_packet_v1 *packet, struct r_remote_data *data)
{
	data->header = R_HEADER_VALUE;
	data->head.per_view_data_valid = false;

	if ((packet->flags & MACOS_REMOTE_POSE_PACKET_ORIENTATION_VALID) != 0) {
		data->head.center.orientation = packet->orientation;
	}
	if ((packet->flags & MACOS_REMOTE_POSE_PACKET_POSITION_VALID) != 0) {
		data->head.center.position = packet->position;
	}
	if ((packet->flags & MACOS_REMOTE_POSE_PACKET_PER_VIEW_VALID) != 0 && packet->view_count >= 2) {
		for (uint32_t i = 0; i < 2; ++i) {
			data->head.views[i].pose = packet->views[i].pose;
			data->head.views[i].fov = packet->views[i].fov;
		}
		data->head.per_view_data_valid = true;
	}
	if (!g_logged_v1_pose) {
		fprintf(stdout,
		        "Bridge v1 center position: x=%0.4f y=%0.4f z=%0.4f views: left_x=%0.4f right_x=%0.4f\n",
		        data->head.center.position.x,
		        data->head.center.position.y,
		        data->head.center.position.z,
		        data->head.views[0].pose.position.x,
		        data->head.views[1].pose.position.x);
		g_logged_v1_pose = true;
	}
	maybe_log_translation(&data->head.center.position);
}

int
main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	uint16_t udp_port = get_udp_port();
	uint16_t remote_driver_port = get_remote_driver_port();
	uint32_t idle_timeout_ms = get_idle_timeout_ms();
	struct r_remote_connection rc = {0};
	struct r_remote_data reset = {0};
	struct r_remote_data data = {0};
	int udp_fd = -1;
	uint32_t packets_forwarded = 0;
	int ret = 1;

	if (udp_port == 0 || remote_driver_port == 0 || idle_timeout_ms == 0) {
		return 1;
	}

	r_socket_t driver_fd = r_remote_connection_init(&rc, "127.0.0.1", remote_driver_port);
#ifdef XRT_OS_WINDOWS
	if (driver_fd == INVALID_SOCKET) {
#else
	if (driver_fd < 0) {
#endif
		return fail_msg("Failed to connect to remote driver socket");
	}

	if (r_remote_connection_read_one(&rc, &reset) < 0) {
		return fail_msg("Failed to read remote reset packet");
	}
	if (r_remote_connection_read_one(&rc, &data) < 0) {
		return fail_msg("Failed to read remote current packet");
	}

	udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (udp_fd < 0) {
		return fail_msg("Failed to create UDP socket");
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(udp_port);
	if (bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		return fail_msg("Failed to bind UDP pose socket");
	}

	data.left.active = false;
	data.left.hand_tracking_active = false;
	data.right.active = false;
	data.right.hand_tracking_active = false;

	for (;;) {
		fd_set set;
		FD_ZERO(&set);
		FD_SET(udp_fd, &set);

		struct timeval timeout = {
		    .tv_sec = (int)(idle_timeout_ms / 1000),
		    .tv_usec = (int)((idle_timeout_ms % 1000) * 1000),
		};

		int ready = select(udp_fd + 1, &set, NULL, NULL, &timeout);
		if (ready < 0) {
			ret = fail_msg("select() failed on UDP pose socket");
			goto out;
		}
		if (ready == 0) {
			break;
		}

		union {
			struct macos_remote_pose_packet_v0 v0;
			struct macos_remote_pose_packet_v1 v1;
		} packet = {0};
		ssize_t received = recvfrom(udp_fd, &packet, sizeof(packet), 0, NULL, NULL);
		if (received < 0) {
			ret = fail_msg("recvfrom() failed on UDP pose socket");
			goto out;
		}
		if (packet.v0.magic != MACOS_REMOTE_POSE_PACKET_MAGIC) {
			ret = fail_msg("Received invalid UDP pose packet header");
			goto out;
		}
		if (packet.v0.version == 0) {
			if ((size_t)received != sizeof(packet.v0)) {
				ret = fail_msg("Received wrong-sized UDP pose packet");
				goto out;
			}
			apply_packet_to_remote_data(&packet.v0, &data);
		} else if (packet.v0.version == 1) {
			if ((size_t)received != sizeof(packet.v1)) {
				ret = fail_msg("Received wrong-sized UDP pose packet");
				goto out;
			}
			apply_packet_v1_to_remote_data(&packet.v1, &data);
		} else {
			ret = fail_msg("Received unknown UDP pose packet version");
			goto out;
		}

		if (r_remote_connection_write_one(&rc, &data) < 0) {
			ret = fail_msg("Failed to forward packet to remote driver");
			goto out;
		}

		packets_forwarded++;
	}

	fprintf(stdout,
	        "Remote pose bridge connected to port %u, listened on UDP %u, and forwarded %u packets.\n",
	        (unsigned)remote_driver_port, (unsigned)udp_port, packets_forwarded);
	ret = 0;

out:
	if (udp_fd >= 0) {
		close(udp_fd);
	}
	return ret;
}

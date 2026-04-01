// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Synthetic sender for the tiny MVP remote pose packet.
 */

#include "tests_macos_remote_pose_protocol.h"

#include "math/m_api.h"
#include "os/os_time.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static int
fail_msg(const char *what)
{
	fprintf(stderr, "%s\n", what);
	return 1;
}

static uint32_t
get_frame_count(void)
{
	const char *value = getenv("MACOS_REMOTE_POSE_PACKET_SENDER_FRAMES");
	if (value == NULL || value[0] == '\0') {
		return 600;
	}

	errno = 0;
	char *end = NULL;
	long parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 3600) {
		fprintf(stderr,
		        "Invalid MACOS_REMOTE_POSE_PACKET_SENDER_FRAMES=%s, expected integer in [1,3600]\n",
		        value);
		return 0;
	}

	return (uint32_t)parsed;
}

static uint16_t
get_udp_port(void)
{
	const char *value = getenv("MACOS_REMOTE_POSE_PACKET_SENDER_UDP_PORT");
	if (value == NULL || value[0] == '\0') {
		return 4243;
	}

	errno = 0;
	char *end = NULL;
	long parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
		fprintf(stderr,
		        "Invalid MACOS_REMOTE_POSE_PACKET_SENDER_UDP_PORT=%s, expected integer in [1,65535]\n",
		        value);
		return 0;
	}

	return (uint16_t)parsed;
}

static void
fill_packet(struct macos_remote_pose_packet_v0 *packet, uint32_t frame_index)
{
	const float t = (float)frame_index / 60.0f;
	const float yaw = sinf(t * 1.3f) * 0.20f;
	const float x = sinf(t * 0.7f) * 0.05f;
	const float y = 1.60f + cosf(t * 0.5f) * 0.02f;
	const float z = -0.10f + sinf(t * 0.9f) * 0.03f;

	packet->magic = MACOS_REMOTE_POSE_PACKET_MAGIC;
	packet->version = MACOS_REMOTE_POSE_PACKET_VERSION;
	packet->flags = MACOS_REMOTE_POSE_PACKET_ORIENTATION_VALID | MACOS_REMOTE_POSE_PACKET_POSITION_VALID;
	packet->client_time_ns = os_monotonic_get_ns();
	packet->predicted_display_time_ns = 0;
	packet->position.x = x;
	packet->position.y = y;
	packet->position.z = z;
	math_quat_from_angle_vector(yaw, &(struct xrt_vec3){0.0f, 1.0f, 0.0f}, &packet->orientation);
}

int
main(void)
{
	uint32_t frame_count = get_frame_count();
	uint16_t udp_port = get_udp_port();
	int sock_fd = -1;
	int ret = 1;

	if (frame_count == 0 || udp_port == 0) {
		return 1;
	}

	sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock_fd < 0) {
		return fail_msg("Failed to create UDP sender socket");
	}

	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(udp_port);

	for (uint32_t i = 0; i < frame_count; ++i) {
		struct macos_remote_pose_packet_v0 packet = {0};
		fill_packet(&packet, i);

		ssize_t sent = sendto(sock_fd, &packet, sizeof(packet), 0, (struct sockaddr *)&addr, sizeof(addr));
		if ((size_t)sent != sizeof(packet)) {
			ret = fail_msg("Failed to send UDP pose packet");
			goto out;
		}

		os_nanosleep(U_TIME_1S_IN_NS / 60);
	}

	fprintf(stdout,
	        "Remote pose packet sender sent %u packets to UDP port %u.\n",
	        frame_count, (unsigned)udp_port);
	ret = 0;

out:
	if (sock_fd >= 0) {
		close(sock_fd);
	}
	return ret;
}

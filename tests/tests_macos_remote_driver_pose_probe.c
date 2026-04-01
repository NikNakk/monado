// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Synthetic pose sender for Monado's remote driver on macOS.
 */

#include "remote/r_interface.h"

#include "math/m_api.h"
#include "os/os_time.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
fail_msg(const char *what)
{
	fprintf(stderr, "%s\n", what);
	return 1;
}

static uint32_t
get_frame_count(void)
{
	const char *value = getenv("MACOS_REMOTE_DRIVER_POSE_PROBE_FRAMES");
	if (value == NULL || value[0] == '\0') {
		return 180;
	}

	errno = 0;
	char *end = NULL;
	long parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 3600) {
		fprintf(stderr,
		        "Invalid MACOS_REMOTE_DRIVER_POSE_PROBE_FRAMES=%s, expected integer in [1,3600]\n",
		        value);
		return 0;
	}

	return (uint32_t)parsed;
}

static uint16_t
get_port(void)
{
	const char *value = getenv("MACOS_REMOTE_DRIVER_POSE_PROBE_PORT");
	if (value == NULL || value[0] == '\0') {
		return 4242;
	}

	errno = 0;
	char *end = NULL;
	long parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 65535) {
		fprintf(stderr,
		        "Invalid MACOS_REMOTE_DRIVER_POSE_PROBE_PORT=%s, expected integer in [1,65535]\n",
		        value);
		return 0;
	}

	return (uint16_t)parsed;
}

static void
animate_head_pose(struct r_remote_data *data, uint32_t frame_index)
{
	const float t = (float)frame_index / 60.0f;
	const float yaw = sinf(t * 1.3f) * 0.20f;
	const float x = sinf(t * 0.7f) * 0.05f;
	const float y = 1.60f + cosf(t * 0.5f) * 0.02f;
	const float z = -0.10f + sinf(t * 0.9f) * 0.03f;

	struct xrt_quat rot = XRT_QUAT_IDENTITY;
	math_quat_from_angle_vector(yaw, &(struct xrt_vec3){0.0f, 1.0f, 0.0f}, &rot);

	data->head.center.orientation = rot;
	data->head.center.position.x = x;
	data->head.center.position.y = y;
	data->head.center.position.z = z;
	data->head.per_view_data_valid = false;
}

int
main(void)
{
	uint32_t frame_count = get_frame_count();
	uint16_t port = get_port();
	struct r_remote_connection rc = {0};
	struct r_remote_data reset = {0};
	struct r_remote_data data = {0};

	if (frame_count == 0 || port == 0) {
		return 1;
	}

	r_socket_t fd = r_remote_connection_init(&rc, "127.0.0.1", port);
#ifdef XRT_OS_WINDOWS
	if (fd == INVALID_SOCKET) {
#else
	if (fd < 0) {
#endif
		return fail_msg("Failed to connect to remote driver socket");
	}

	if (r_remote_connection_read_one(&rc, &reset) < 0) {
		return fail_msg("Failed to read remote reset packet");
	}
	if (r_remote_connection_read_one(&rc, &data) < 0) {
		return fail_msg("Failed to read remote current packet");
	}

	if (reset.header != R_HEADER_VALUE || data.header != R_HEADER_VALUE) {
		return fail_msg("Remote driver handshake did not return valid packets");
	}

	for (uint32_t i = 0; i < frame_count; ++i) {
		animate_head_pose(&data, i);
		if (r_remote_connection_write_one(&rc, &data) < 0) {
			return fail_msg("Failed to send remote pose packet");
		}

		os_nanosleep(U_TIME_1S_IN_NS / 60);
	}

	fprintf(stdout,
	        "Remote pose probe connected on port %u and sent %u head pose packets.\n",
	        (unsigned)port, frame_count);
	return 0;
}

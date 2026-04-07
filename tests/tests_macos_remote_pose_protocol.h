// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "xrt/xrt_defines.h"

#include <stdint.h>

#define MACOS_REMOTE_POSE_PACKET_MAGIC 0x3156504dU
#define MACOS_REMOTE_POSE_PACKET_VERSION 1U

enum macos_remote_pose_packet_flags
{
	MACOS_REMOTE_POSE_PACKET_ORIENTATION_VALID = 1 << 0,
	MACOS_REMOTE_POSE_PACKET_POSITION_VALID = 1 << 1,
	MACOS_REMOTE_POSE_PACKET_PER_VIEW_VALID = 1 << 2,
};

struct macos_remote_pose_packet_v0
{
	uint32_t magic;
	uint16_t version;
	uint16_t flags;
	uint64_t client_time_ns;
	uint64_t predicted_display_time_ns;
	struct xrt_quat orientation;
	struct xrt_vec3 position;
	uint32_t _pad;
};

struct macos_remote_pose_packet_v1_view
{
	struct xrt_pose pose;
	struct xrt_fov fov;
};

struct macos_remote_pose_packet_v1
{
	uint32_t magic;
	uint16_t version;
	uint16_t flags;
	uint64_t client_time_ns;
	uint64_t predicted_display_time_ns;
	struct xrt_quat orientation;
	struct xrt_vec3 position;
	uint16_t view_count;
	uint16_t _pad0;
	struct macos_remote_pose_packet_v1_view views[2];
};

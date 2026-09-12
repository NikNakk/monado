// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Source-local renderer submit overrides for swapchain GPU reuse tracking.
 * @ingroup comp_util
 */

#pragma once

/* Pull in the real declarations before replacing calls in comp_renderer.c. */
#include "vk/vk_cmd.h"
#include "vk/vk_submit_helpers.h"
#include "util/comp_swapchain_gpu_reuse_internal.h"

#define vk_submit_info_builder_prepare(BUILDER, WAITS, CMDS, CMD_COUNT, SIGNALS, NEXT)                                  \
	comp_swapchain_gpu_reuse_submit_info_builder_prepare((BUILDER), (WAITS), (CMDS), (CMD_COUNT), (SIGNALS), (NEXT))

#define vk_cmd_submit_locked(VK, QUEUE, COUNT, INFOS, FENCE)                                                           \
	comp_swapchain_gpu_reuse_vk_cmd_submit_locked((VK), (QUEUE), (COUNT), (INFOS), (FENCE))

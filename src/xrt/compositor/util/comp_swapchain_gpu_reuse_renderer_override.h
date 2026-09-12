// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Source-local renderer submit override for swapchain GPU reuse tracking.
 * @ingroup comp_util
 */

#pragma once

/* Pull in the real declaration before replacing calls in comp_renderer.c. */
#include "vk/vk_cmd.h"
#include "util/comp_swapchain_gpu_reuse_internal.h"

/*
 * comp_renderer.c has a local `r` at its only vk_cmd_submit_locked() call.
 * Pass the exact native layer accumulator that produced the sampled image views.
 */
#define vk_cmd_submit_locked(VK, QUEUE, COUNT, INFOS, FENCE)                                                           \
	comp_swapchain_gpu_reuse_vk_cmd_submit_locked(&(r->c->base.layer_accum), (VK), (QUEUE), (COUNT), (INFOS),      \
	                                              (FENCE))

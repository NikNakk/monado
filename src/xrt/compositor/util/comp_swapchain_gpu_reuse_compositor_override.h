// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Source-local compositor draw cleanup for swapchain GPU reuse tracking.
 * @ingroup comp_util
 */

#pragma once

/* Pull in the real declaration before replacing calls in comp_compositor.c. */
#include "main/comp_renderer.h"
#include "util/comp_swapchain_gpu_reuse_internal.h"

static inline xrt_result_t
comp_swapchain_gpu_reuse_renderer_draw(struct comp_renderer *r, struct comp_layer_accum *cla)
{
	xrt_result_t xret = comp_renderer_draw(r);

	/*
	 * The normal renderer submit hook releases these claims immediately after
	 * vkQueueSubmit. This is a no-op then, but also covers renderer error/early
	 * return paths that never reached a queue submit.
	 */
	comp_swapchain_gpu_reuse_native_accum_release(cla);
	return xret;
}

/* comp_compositor.c has a local `c` at its comp_renderer_draw() call. */
#define comp_renderer_draw(R) comp_swapchain_gpu_reuse_renderer_draw((R), &(c->base.layer_accum))

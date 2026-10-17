// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "xrt/xrt_frame.h"
#include "render/render_camera.h"
#include "xrt/xrt_defines.h"
struct comp_compositor;
struct comp_psvr2_passthrough;

struct comp_psvr2_passthrough_sample
{
	struct xrt_frame *frames[2];
	struct render_camera_params params;
};
struct comp_psvr2_passthrough *
comp_psvr2_passthrough_create(struct comp_compositor *c, bool is_service);
void
comp_psvr2_passthrough_destroy(struct comp_psvr2_passthrough **pt);
const float *
comp_psvr2_passthrough_map(struct comp_psvr2_passthrough *pt, uint32_t eye);
bool
comp_psvr2_passthrough_sample(struct comp_psvr2_passthrough *pt,
                              const struct xrt_quat *display_orientation,
                              bool has_app,
                              struct comp_psvr2_passthrough_sample *sample);
void
comp_psvr2_passthrough_sample_release(struct comp_psvr2_passthrough_sample *sample);

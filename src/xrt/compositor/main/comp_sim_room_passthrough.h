// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulated passthrough: a room drawn at the display pose, no cameras.
 * @ingroup comp_main
 */
#pragma once

#include "render/render_camera.h"

#include <stdbool.h>
#include <stdint.h>

struct comp_compositor;
struct comp_target_image;
struct comp_sim_room;

//! Per-eye ray maps resolution (texels per side).
#define COMP_SIM_ROOM_MAP_SIZE 256

//! Builds the per-eye ray maps from the HMD's distortion and field of view.
struct comp_sim_room *
comp_sim_room_create(struct comp_compositor *c);

void
comp_sim_room_destroy(struct comp_sim_room **room);

//! COMP_SIM_ROOM_MAP_SIZE^2 float4 texels: xy unused (-1), zw the view's tangent ray.
const float *
comp_sim_room_map(struct comp_sim_room *room, uint32_t eye);

//! Room parameters for the eye poses this image was timewarped to.
//! False when the image has no valid eye poses.
bool
comp_sim_room_sample(struct comp_sim_room *room,
                     const struct comp_target_image *image,
                     bool has_app,
                     struct render_camera_params *out_params);

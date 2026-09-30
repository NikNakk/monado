// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "psvr2_linear_prediction.h"

struct psvr2_continuity_params
{
	float tau_s;
	float limit_m;
};

/* A polynomial difference between the old and new prediction, decaying in
 * query HOST time. Target VTS must never drive the decay: it is in the future.
 * Fixed-size state, protected by the same data_lock as the SLAM relation. */
struct psvr2_continuity_prediction
{
	int64_t source_ns;
	int64_t received_ns;
	struct xrt_vec3 position;
	struct xrt_vec3 velocity;
	struct xrt_vec3 acceleration;
	struct xrt_vec3 correction_position;
	struct xrt_vec3 correction_velocity;
	struct xrt_vec3 correction_acceleration;
	bool valid;
};

static inline void
psvr2_continuity_update(struct psvr2_continuity_prediction *state,
                        const struct psvr2_continuity_params *params,
                        const struct psvr2_linear_prediction *linear,
                        const struct psvr2_linear_prediction_params *linear_params,
                        int64_t received_ns)
{
	struct xrt_vec3 a = {0};
	if (linear->ready && psvr2_linear_length(linear->velocity) >= linear_params->min_speed) {
		a = (struct xrt_vec3){linear->acceleration.x * linear_params->gain,
		                     linear->acceleration.y * linear_params->gain,
		                     linear->acceleration.z * linear_params->gain};
	}
	float half_interval = 0.5f * linear->interval_s;
	struct xrt_vec3 v = {linear->velocity.x + a.x * half_interval,
	                     linear->velocity.y + a.y * half_interval,
	                     linear->velocity.z + a.z * half_interval};
	float dt = (float)((double)(linear->timestamp_ns - state->source_ns) * 1e-9);
	float dh = (float)((double)(received_ns - state->received_ns) * 1e-9);
	bool contiguous = state->valid && linear->have_velocity && dt >= 0.005f && dt <= 0.035f &&
	                  dh >= 0.0f && dh <= 0.1f;
	if (contiguous) {
		float decay = expf(-dh / params->tau_s);
		// Translate the old quadratic and its remaining correction to the new
		// source timestamp, then subtract the new quadratic. The prediction is
		// continuous before applying bounds, within the acceleration horizon.
#define PSVR2_CONTINUITY_AXIS(axis)                                                                                     \
		state->correction_position.axis =                                                                               \
		    (state->position.axis - linear->position.axis) + state->velocity.axis * dt +                                  \
		    0.5f * state->acceleration.axis * dt * dt +                                                                   \
		    decay * (state->correction_position.axis + state->correction_velocity.axis * dt +                            \
		             0.5f * state->correction_acceleration.axis * dt * dt);                                              \
		state->correction_velocity.axis =                                                                               \
		    state->velocity.axis + state->acceleration.axis * dt - v.axis +                                              \
		    decay * (state->correction_velocity.axis + state->correction_acceleration.axis * dt);                         \
		state->correction_acceleration.axis =                                                                           \
		    state->acceleration.axis - a.axis + decay * state->correction_acceleration.axis
		PSVR2_CONTINUITY_AXIS(x);
		PSVR2_CONTINUITY_AXIS(y);
		PSVR2_CONTINUITY_AXIS(z);
#undef PSVR2_CONTINUITY_AXIS
	} else {
		state->correction_position = (struct xrt_vec3){0};
		state->correction_velocity = (struct xrt_vec3){0};
		state->correction_acceleration = (struct xrt_vec3){0};
	}
	state->position = linear->position;
	state->velocity = v;
	state->acceleration = a;
	state->source_ns = linear->timestamp_ns;
	state->received_ns = received_ns;
	state->valid = linear->have_velocity;
}

/* Add to the bounded-acceleration candidate, or its full-horizon raw fallback.
 * Velocity is the derivative with respect to target VTS at fixed query host
 * time. The radial-cap derivative is projected onto the sphere's tangent. */
static inline void
psvr2_continuity_predict(const struct psvr2_continuity_prediction *state,
                         const struct psvr2_continuity_params *params,
                         int64_t target_ns,
                         int64_t query_host_ns,
                         struct xrt_vec3 *position,
                         struct xrt_vec3 *velocity)
{
	if (!state->valid || target_ns <= state->source_ns) {
		return;
	}
	float h = (float)((double)(target_ns - state->source_ns) * 1e-9);
	float age = fmaxf(0.0f, (float)((double)(query_host_ns - state->received_ns) * 1e-9));
	float decay = expf(-age / params->tau_s);
	struct xrt_vec3 c, d;
#define PSVR2_CONTINUITY_AXIS(axis)                                                                                     \
	c.axis = decay * (state->correction_position.axis + state->correction_velocity.axis * h +                          \
	                  0.5f * state->correction_acceleration.axis * h * h);                                           \
	d.axis = decay * (state->correction_velocity.axis + state->correction_acceleration.axis * h)
	PSVR2_CONTINUITY_AXIS(x);
	PSVR2_CONTINUITY_AXIS(y);
	PSVR2_CONTINUITY_AXIS(z);
#undef PSVR2_CONTINUITY_AXIS
	float length = psvr2_linear_length(c);
	if (length > params->limit_m) {
		struct xrt_vec3 n = {c.x / length, c.y / length, c.z / length};
		float projection = n.x * d.x + n.y * d.y + n.z * d.z;
		float scale = params->limit_m / length;
		d = (struct xrt_vec3){scale * (d.x - n.x * projection), scale * (d.y - n.y * projection),
		                     scale * (d.z - n.z * projection)};
		c = (struct xrt_vec3){n.x * params->limit_m, n.y * params->limit_m, n.z * params->limit_m};
	}
	position->x += c.x;
	position->y += c.y;
	position->z += c.z;
	velocity->x += d.x;
	velocity->y += d.y;
	velocity->z += d.z;
}

// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_defines.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Fixed-cost, position-only experiment. Call under the PSVR2 data lock.
 * Raw relation-history velocity is an interval average, dated at its midpoint.
 * See doc/psvr2-position-prediction.md for the causal replay and limitations. */
struct psvr2_linear_prediction_params
{
	float alpha;
	float gain;
	float max_acceleration;
	float min_speed;
	float max_horizon_s;
};

struct psvr2_linear_prediction
{
	int64_t timestamp_ns;
	struct xrt_vec3 position;
	struct xrt_vec3 velocity;
	struct xrt_vec3 acceleration;
	float interval_s;
	bool have_velocity;
	bool ready;
};

static inline float
psvr2_linear_length(struct xrt_vec3 v)
{
	return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static inline bool
psvr2_linear_finite(struct xrt_vec3 v)
{
	return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static inline struct xrt_vec3
psvr2_linear_clip(struct xrt_vec3 v, float limit)
{
	float length = psvr2_linear_length(v);
	float scale = length > limit ? limit / length : 1.0f;
	return (struct xrt_vec3){v.x * scale, v.y * scale, v.z * scale};
}

static inline void
psvr2_linear_update(struct psvr2_linear_prediction *state,
                    const struct psvr2_linear_prediction_params *params,
                    int64_t timestamp_ns,
                    struct xrt_vec3 position,
                    struct xrt_vec3 velocity,
                    bool valid)
{
	float dt = (float)((double)(timestamp_ns - state->timestamp_ns) * 1e-9);
	bool finite = valid && psvr2_linear_finite(position) && psvr2_linear_finite(velocity);
	bool contiguous = finite && state->have_velocity && dt >= 0.005f && dt <= 0.035f;
	// Two valid intervals are needed to date the acceleration estimate.
	state->ready = contiguous && state->interval_s > 0.0f;
	if (state->ready) {
		float inverse_midpoint_dt = 2.0f / (dt + state->interval_s);
		struct xrt_vec3 a = {(velocity.x - state->velocity.x) * inverse_midpoint_dt,
		                     (velocity.y - state->velocity.y) * inverse_midpoint_dt,
		                     (velocity.z - state->velocity.z) * inverse_midpoint_dt};
		// Clip before filtering so a SLAM discontinuity cannot poison many updates.
		a = psvr2_linear_clip(a, params->max_acceleration);
		state->acceleration.x += params->alpha * (a.x - state->acceleration.x);
		state->acceleration.y += params->alpha * (a.y - state->acceleration.y);
		state->acceleration.z += params->alpha * (a.z - state->acceleration.z);
		state->acceleration = psvr2_linear_clip(state->acceleration, params->max_acceleration);
	} else {
		state->acceleration = (struct xrt_vec3){0};
	}
	state->timestamp_ns = timestamp_ns;
	state->position = position;
	state->velocity = velocity;
	state->interval_s = contiguous ? dt : 0.0f;
	state->have_velocity = finite;
}

/* Returns false when raw prediction should be retained. At long horizons only
 * the correction saturates; raw extrapolation is unchanged. Returned velocity
 * is the derivative of this positional prediction (right derivative at cap). */
static inline bool
psvr2_linear_predict(const struct psvr2_linear_prediction *state,
                     const struct psvr2_linear_prediction_params *params,
                     int64_t target_ns,
                     struct xrt_vec3 *position,
                     struct xrt_vec3 *velocity)
{
	if (!state->ready || target_ns <= state->timestamp_ns ||
	    psvr2_linear_length(state->velocity) < params->min_speed) {
		return false;
	}
	float dt = (float)((double)(target_ns - state->timestamp_ns) * 1e-9);
	float h = fminf(dt, params->max_horizon_s);
	// The h*interval term corrects the half-sample age of backward velocity.
	float pc = params->gain * 0.5f * h * (h + state->interval_s);
	float vc = dt < params->max_horizon_s ? params->gain * (h + 0.5f * state->interval_s) : 0.0f;
	*position = (struct xrt_vec3){state->position.x + state->velocity.x * dt + state->acceleration.x * pc,
	                            state->position.y + state->velocity.y * dt + state->acceleration.y * pc,
	                            state->position.z + state->velocity.z * dt + state->acceleration.z * pc};
	*velocity = (struct xrt_vec3){state->velocity.x + state->acceleration.x * vc,
	                            state->velocity.y + state->acceleration.y * vc,
	                            state->velocity.z + state->acceleration.z * vc};
	return true;
}

// Copyright 2026, Monado contributors.
// SPDX-License-Identifier: BSL-1.0
// Standalone regression/protocol harness; see scripts/psvr2_predictor_replay.py.
#include "psvr2_linear_prediction.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
	struct psvr2_linear_prediction state = {0};
	struct psvr2_linear_prediction_params params = {0.25f, 0.5f, 2.0f, 0.01f, 0.08f};
	struct xrt_vec3 p = {0}, v = {0};
	if (argc == 2 && strcmp(argv[1], "--replay") == 0) {
		char command;
		int64_t ts;
		unsigned int valid;
		while (scanf(" %c %" SCNi64, &command, &ts) == 2) {
			if (command == 'S') {
				assert(scanf("%f %f %f %f %f %f %u", &p.x, &p.y, &p.z, &v.x, &v.y, &v.z, &valid) == 7);
				psvr2_linear_update(&state, &params, ts, p, v, valid != 0);
			} else {
				assert(command == 'P');
				float dt = (float)((double)(ts - state.timestamp_ns) * 1e-9);
				p = (struct xrt_vec3){state.position.x + state.velocity.x * dt,
				                     state.position.y + state.velocity.y * dt,
				                     state.position.z + state.velocity.z * dt};
				v = state.velocity;
				psvr2_linear_predict(&state, &params, ts, &p, &v);
				printf("%.9g %.9g %.9g\n", p.x, p.y, p.z);
			}
		}
		return 0;
	}
	assert(!psvr2_linear_predict(&state, &params, 1, &p, &v));
	// Backward differences of constant acceleration: midpoint correction must
	// reproduce the analytic trajectory with gain=alpha=1, including uneven dt.
	params.alpha = params.gain = 1.0f;
	int64_t times[] = {0, 16000000, 33000000, 49000000, 67000000};
	for (unsigned int i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
		float t = (float)((double)times[i] * 1e-9);
		float previous = i > 0 ? (float)((double)times[i - 1] * 1e-9) : 0;
		p = (struct xrt_vec3){0.1f * t + 0.5f * t * t, 0, 0};
		v = (struct xrt_vec3){0.1f + 0.5f * (t + previous), 0, 0};
		psvr2_linear_update(&state, &params, times[i], p, v, i > 0);
	}
	assert(psvr2_linear_predict(&state, &params, 127000000, &p, &v));
	assert(fabsf(p.x - (0.1f * 0.127f + 0.5f * 0.127f * 0.127f)) < 1e-7f);
	assert(fabsf(v.x - 0.227f) < 1e-6f);
	assert(!psvr2_linear_predict(&state, &params, 67000000, &p, &v));
	assert(!psvr2_linear_predict(&state, &params, 66000000, &p, &v));
	// Saturating the correction must bound it even for very distant requests.
	assert(psvr2_linear_predict(&state, &params, 10067000000LL, &p, &v));
	float raw = state.position.x + state.velocity.x * 10.0f;
	assert(fabsf(p.x - raw) <= 0.5f * 2.0f * 0.08f * (0.08f + 0.035f));
	assert(v.x == state.velocity.x);
	// Gaps, duplicate/reordered timestamps, and invalid data reset history.
	psvr2_linear_update(&state, &params, 200000000, p, v, true);
	assert(!state.ready);
	psvr2_linear_update(&state, &params, 200000000, p, v, true);
	assert(!state.ready);
	psvr2_linear_update(&state, &params, 199000000, p, v, true);
	assert(!state.ready);
	p.x = NAN;
	psvr2_linear_update(&state, &params, 216000000, p, v, true);
	assert(!state.ready && !state.have_velocity);
	// Stationary fallback and clipping in a sudden reversal.
	state = (struct psvr2_linear_prediction){0};
	params.alpha = 0.25f;
	for (int i = 1; i <= 20; i++) {
		p = (struct xrt_vec3){0};
		v = (struct xrt_vec3){i < 10 ? 0.5f : -0.5f, 0, 0};
		psvr2_linear_update(&state, &params, i * 16683000LL, p, v, true);
		assert(psvr2_linear_length(state.acceleration) <= params.max_acceleration);
	}
	state.velocity = (struct xrt_vec3){0};
	assert(!psvr2_linear_predict(&state, &params, state.timestamp_ns + 60000000, &p, &v));
	puts("PSVR2 linear prediction tests passed");
	return 0;
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "tracking/t_dead_reckoning.h"
#include "math/m_filter_fifo.h"
#include "math/m_api.h"
#include "util/u_time.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Experimental PSVR2 orientation predictor, modelled on the GAV macOS player.
 * Keep Monado's ordinary dead reckoning for position and as the default path,
 * but optionally replace the predicted orientation by integrating every real
 * high-rate gyro sample newer than the latest SLAM pose, then extrapolating
 * only the remainder from the newest IMU sample to the requested timestamp.
 */
static inline bool
psvr2_explicit_gyro_integration_enabled(void)
{
	static int enabled = -1;
	if (enabled >= 0) {
		return enabled != 0;
	}

	const char *value = getenv("PSVR2_GYRO_INTEGRATION");
	enabled = value != NULL &&
	          (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0 ||
	           strcmp(value, "yes") == 0 || strcmp(value, "YES") == 0);
	if (enabled) {
		fprintf(stderr,
		        "PSVR2: explicit gyro integration enabled (SLAM -> 2 kHz IMU samples -> prediction target)\n");
	}
	return enabled != 0;
}

static inline struct xrt_quat
psvr2_quat_multiply(struct xrt_quat a, struct xrt_quat b)
{
	return (struct xrt_quat){
	    .x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	    .y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	    .z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	    .w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

static inline void
psvr2_integrate_gyro_step(struct xrt_quat *orientation, const struct xrt_vec3 *gyro, time_duration_ns dt_ns)
{
	if (dt_ns <= 0) {
		return;
	}

	const float dt_s = (float)((double)dt_ns / (double)U_TIME_1S_IN_NS);
	const float speed = sqrtf(gyro->x * gyro->x + gyro->y * gyro->y + gyro->z * gyro->z);
	struct xrt_quat delta;
	if (speed < 1e-8f) {
		delta = (struct xrt_quat){
		    .x = gyro->x * dt_s * 0.5f,
		    .y = gyro->y * dt_s * 0.5f,
		    .z = gyro->z * dt_s * 0.5f,
		    .w = 1.0f,
		};
	} else {
		const float half_angle = speed * dt_s * 0.5f;
		const float scale = sinf(half_angle) / speed;
		delta = (struct xrt_quat){
		    .x = gyro->x * scale,
		    .y = gyro->y * scale,
		    .z = gyro->z * scale,
		    .w = cosf(half_angle),
		};
	}

	*orientation = psvr2_quat_multiply(*orientation, delta);
	math_quat_normalize(orientation);
}

static inline void
psvr2_predict_mode(struct m_ff_vec3_f32 *gyro_ff,
                           struct m_ff_vec3_f32 *accel_ff,
                           const struct xrt_vec3 *gravity_correction,
                           timepoint_ns when_ns,
                           const struct xrt_space_relation *base_rel,
                           timepoint_ns base_rel_ts,
                           struct xrt_space_relation *out_relation,
                           bool explicit_integration)
{
	/* Preserve the existing Monado result, especially its position prediction. */
	t_apply_dead_reckoning(gyro_ff, accel_ff, gravity_correction, when_ns, base_rel, base_rel_ts, out_relation);

	if (!explicit_integration || gyro_ff == NULL || when_ns <= base_rel_ts) {
		return;
	}

	struct xrt_quat orientation = base_rel->pose.orientation;
	timepoint_ns cursor_ns = base_rel_ts;
	struct xrt_vec3 newest_gyro = {0};
	bool have_integrated_sample = false;

	/*
	 * FIFO index zero is newest. Walk it backwards so samples are integrated
	 * in chronological order, using their actual VTS-derived timestamps.
	 */
	const size_t sample_count = m_ff_vec3_f32_get_num(gyro_ff);
	for (size_t n = sample_count; n > 0; --n) {
		struct xrt_vec3 gyro;
		uint64_t sample_ts_u64 = 0;
		if (!m_ff_vec3_f32_get(gyro_ff, n - 1, &gyro, &sample_ts_u64)) {
			continue;
		}
		const timepoint_ns sample_ts = (timepoint_ns)sample_ts_u64;
		if (sample_ts <= base_rel_ts || sample_ts > when_ns) {
			continue;
		}
		if (sample_ts > cursor_ns) {
			psvr2_integrate_gyro_step(&orientation, &gyro, sample_ts - cursor_ns);
			cursor_ns = sample_ts;
			newest_gyro = gyro;
			have_integrated_sample = true;
		}
	}

	/*
	 * If no sample is newer than SLAM (the streams are independent), use the
	 * newest available gyro for the whole remainder, matching the existing
	 * driver's high-rate fallback. Otherwise extrapolate only after the newest
	 * measured sample.
	 */
	if (!have_integrated_sample) {
		uint64_t sample_ts = 0;
		if (!m_ff_vec3_f32_get(gyro_ff, 0, &newest_gyro, &sample_ts)) {
			return;
		}
	}

	if (when_ns > cursor_ns) {
		psvr2_integrate_gyro_step(&orientation, &newest_gyro, when_ns - cursor_ns);
	}

	out_relation->pose.orientation = orientation;
	math_quat_rotate_derivative(&orientation, &newest_gyro, &out_relation->angular_velocity);
	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    out_relation->relation_flags | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
}

static inline void
psvr2_apply_dead_reckoning(struct m_ff_vec3_f32 *gyro_ff,
                           struct m_ff_vec3_f32 *accel_ff,
                           const struct xrt_vec3 *gravity_correction,
                           timepoint_ns when_ns,
                           const struct xrt_space_relation *base_rel,
                           timepoint_ns base_rel_ts,
                           struct xrt_space_relation *out_relation)
{
	psvr2_predict_mode(gyro_ff, accel_ff, gravity_correction, when_ns, base_rel, base_rel_ts,
	                   out_relation, psvr2_explicit_gyro_integration_enabled());
}


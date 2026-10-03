// Copyright 2020-2021, Collabora, Ltd.
// Copyright 2023, Jan Schmidt
// Copyright 2024, Joel Valenciano
// Copyright 2025, Beyley Cardellio
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "psvr2_continuity_prediction.h"
#include "tracking/t_dead_reckoning.h"
#include "math/m_api.h"
#include "math/m_space.h"
#include <stddef.h>
#include <string.h>

#define PSVR2_TRACKING_SHARE_VERSION 2u
#define PSVR2_TRACKING_GYRO_CAPACITY 1024u
#define PSVR2_TRACKING_STALE_NS INT64_C(500000000)

/* Pointer-free state in the driver's hardware VTS domain. Protected by data_lock
 * in the producer, and a local mutex in each consumer. */
struct psvr2_tracking_state
{
	struct xrt_space_relation relation;
	int64_t slam_ns, hw2mono_vts, published_ns;
	int64_t imu_received_ns, imu_estimated_ns, slam_received_ns;
	struct xrt_vec3 last_gyro;
	struct xrt_pose T_imu_head, recenter_transform;
	struct psvr2_linear_prediction linear_prediction;
	struct psvr2_linear_prediction_params linear_prediction_params;
	struct psvr2_continuity_prediction continuity_prediction;
	struct psvr2_continuity_params continuity_params;
	float ipd_m;
	bool ready, acceleration_prediction_enabled, continuity_prediction_enabled;
	bool recenter_on_first_pose, recenter_initialized;
};
struct psvr2_tracking_gyro
{
	uint64_t timestamp_ns;
	struct xrt_vec3 value;
};
struct psvr2_tracking_snapshot
{
	struct psvr2_tracking_state state;
	uint32_t gyro_count;
	struct psvr2_tracking_gyro gyro[PSVR2_TRACKING_GYRO_CAPACITY]; // newest first
};
/* A snapshot includes every retained sample needed for future prediction.
 * Rebuild into the existing allocation: timestamp equality must not discard a
 * distinct sample (the driver's FIFO allows duplicate timestamps). */
static inline void
psvr2_tracking_snapshot_fill_gyro(const struct psvr2_tracking_snapshot *snapshot, struct m_ff_vec3_f32 *gyro)
{
	m_ff_vec3_f32_clear(gyro);
	for (uint32_t i = snapshot->gyro_count; i > 0; --i) {
		const struct psvr2_tracking_gyro *g = &snapshot->gyro[i - 1];
		m_ff_vec3_f32_push(gyro, &g->value, g->timestamp_ns);
	}
}

#define PSVR2_TRACKING_WORDS ((sizeof(struct psvr2_tracking_snapshot) + 7) / 8)
struct psvr2_tracking_share
{
	uint64_t sequence;
	uint64_t words[PSVR2_TRACKING_WORDS];
};

#ifndef _MSC_VER
/* All shared accesses are lock-free 64-bit atomics, including payload. This
 * avoids the C data race in seqlocks built from non-atomic memcpy. Sequential
 * consistency gives one ordering across sequence and payload on Apple Silicon.
 * Single producer holds data_lock; consumer retries are bounded. */
static inline size_t
psvr2_tracking_snapshot_words(const struct psvr2_tracking_snapshot *s)
{
	return (offsetof(struct psvr2_tracking_snapshot, gyro) + s->gyro_count * sizeof(s->gyro[0]) + 7) / 8;
}
static inline void
psvr2_tracking_share_publish(struct psvr2_tracking_share *share, const struct psvr2_tracking_snapshot *s)
{
	uint64_t sequence = __atomic_load_n(&share->sequence, __ATOMIC_SEQ_CST);
	__atomic_store_n(&share->sequence, sequence + 1, __ATOMIC_SEQ_CST);
	const unsigned char *bytes = (const unsigned char *)s;
	for (size_t i = 0; i < psvr2_tracking_snapshot_words(s); ++i) {
		uint64_t word = 0;
		size_t remaining = sizeof(*s) - i * 8;
		memcpy(&word, bytes + i * 8, remaining < 8 ? remaining : 8);
		__atomic_store_n(&share->words[i], word, __ATOMIC_SEQ_CST);
	}
	__atomic_store_n(&share->sequence, sequence + 2, __ATOMIC_SEQ_CST);
}
static inline bool
psvr2_tracking_share_read(const struct psvr2_tracking_share *share,
                          struct psvr2_tracking_snapshot *out,
                          uint64_t *out_sequence)
{
	for (unsigned attempt = 0; attempt < 3; ++attempt) {
		uint64_t sequence = __atomic_load_n(&share->sequence, __ATOMIC_SEQ_CST);
		if (sequence == 0 || (sequence & 1))
			continue;
		size_t words = (offsetof(struct psvr2_tracking_snapshot, gyro) + 7) / 8;
		for (size_t i = 0; i < words; ++i) {
			uint64_t word = __atomic_load_n(&share->words[i], __ATOMIC_SEQ_CST);
			memcpy((unsigned char *)out + i * 8, &word, 8);
		}
		if (out->gyro_count > PSVR2_TRACKING_GYRO_CAPACITY)
			continue;
		size_t total = psvr2_tracking_snapshot_words(out);
		for (size_t i = words; i < total; ++i) {
			uint64_t word = __atomic_load_n(&share->words[i], __ATOMIC_SEQ_CST);
			size_t remaining = sizeof(*out) - i * 8;
			memcpy((unsigned char *)out + i * 8, &word, remaining < 8 ? remaining : 8);
		}
		if (__atomic_load_n(&share->sequence, __ATOMIC_SEQ_CST) == sequence) {
			*out_sequence = sequence;
			return true;
		}
	}
	return false;
}

#endif // !_MSC_VER

/* Same future-pose calculation for driver and shared-state consumer. Historical
 * queries remain on the driver's full relation history. */
static inline void
psvr2_tracking_predict_raw(const struct psvr2_tracking_state *state,
                           struct m_ff_vec3_f32 *gyro,
                           int64_t at_timestamp_ns,
                           int64_t query_host_ns,
                           struct xrt_space_relation *out_relation)
{
	struct xrt_space_relation latest_relation = state->relation;
	int64_t source_host_ns = state->slam_ns + state->hw2mono_vts;
	if (source_host_ns > 0 && query_host_ns > source_host_ns &&
	    query_host_ns - source_host_ns > PSVR2_TRACKING_STALE_NS) {
		latest_relation.relation_flags = (enum xrt_space_relation_flags)(
		    latest_relation.relation_flags &
		    ~(XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
		      XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT | XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT));
		latest_relation.angular_velocity = XRT_C11_COMPOUND(struct xrt_vec3) XRT_VEC3_ZERO;
		latest_relation.linear_velocity = XRT_C11_COMPOUND(struct xrt_vec3) XRT_VEC3_ZERO;
		*out_relation = latest_relation;
		return;
	}
	// Status and SLAM transfers are independent. A pose query can arrive after a
	// new SLAM pose but just before the next status packet, leaving no gyro sample
	// newer than the pose. Seed prediction with the most recent high-rate gyro in
	// that case instead of relying on noisier velocity estimated from 60 Hz SLAM.
	math_quat_rotate_derivative(&latest_relation.pose.orientation, &state->last_gyro,
	                            &latest_relation.angular_velocity);
	latest_relation.relation_flags = (enum xrt_space_relation_flags)(latest_relation.relation_flags |
	                                                                 XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);

	// Predict forward using dead reckoning
	if (!t_apply_dead_reckoning( //
	        gyro,                //
	        NULL,                //
	        NULL,                //
	        at_timestamp_ns,     //
	        &latest_relation,    //
	        state->slam_ns,      //
	        out_relation)) {
		// If dead reckoning fails, return the latest SLAM pose
		*out_relation = latest_relation;
	}

	// Gyro-only dead reckoning advances integ_rel_ts without advancing position,
	// so predict linear motion over the complete SLAM->target interval.
	if ((latest_relation.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0 &&
	    (latest_relation.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) != 0) {
		float dt = (float)((double)(at_timestamp_ns - state->slam_ns) * 1e-9);
		out_relation->pose.position = XRT_C11_COMPOUND(struct xrt_vec3){
		    latest_relation.pose.position.x + latest_relation.linear_velocity.x * dt,
		    latest_relation.pose.position.y + latest_relation.linear_velocity.y * dt,
		    latest_relation.pose.position.z + latest_relation.linear_velocity.z * dt};
		out_relation->linear_velocity = latest_relation.linear_velocity;
	}

	if (state->acceleration_prediction_enabled && state->linear_prediction.timestamp_ns == state->slam_ns &&
	    (latest_relation.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0 &&
	    (latest_relation.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) != 0) {
		psvr2_linear_predict(&state->linear_prediction, &state->linear_prediction_params, at_timestamp_ns,
		                     &out_relation->pose.position, &out_relation->linear_velocity);
		if (state->continuity_prediction_enabled && state->continuity_prediction.source_ns == state->slam_ns) {
			psvr2_continuity_predict(&state->continuity_prediction, &state->continuity_params,
			                         at_timestamp_ns, query_host_ns, &out_relation->pose.position,
			                         &out_relation->linear_velocity);
		}
	}
}

static inline void
psvr2_tracking_predict_head(const struct psvr2_tracking_state *state,
                            struct m_ff_vec3_f32 *gyro,
                            int64_t target_vts,
                            int64_t query_host_ns,
                            struct xrt_space_relation *out)
{
	struct xrt_space_relation raw;
	psvr2_tracking_predict_raw(state, gyro, target_vts, query_host_ns, &raw);
	struct xrt_relation_chain chain;
	memset(&chain, 0, sizeof(chain));
	m_relation_chain_push_pose(&chain, &state->T_imu_head);
	*m_relation_chain_reserve(&chain) = raw;
	m_relation_chain_resolve(&chain, out);
	if (state->recenter_on_first_pose && state->recenter_initialized &&
	    (out->relation_flags &
	     (XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)) ==
	        (XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)) {
		math_pose_transform(&state->recenter_transform, &out->pose, &out->pose);
		struct xrt_vec3 linear, angular;
		math_quat_rotate_vec3(&state->recenter_transform.orientation, &out->linear_velocity, &linear);
		math_quat_rotate_vec3(&state->recenter_transform.orientation, &out->angular_velocity, &angular);
		out->linear_velocity = linear;
		out->angular_velocity = angular;
	}
}

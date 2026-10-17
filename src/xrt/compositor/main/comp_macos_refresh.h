// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "xrt/xrt_results.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

//! Active sessions apply on the compositor thread; idle requests apply under the same lock.
struct comp_macos_refresh_queue
{
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	bool pending, closing, active;
	uint32_t waiters;
	float hz;
	xrt_result_t result;
};

static inline bool
comp_macos_refresh_init(struct comp_macos_refresh_queue *q)
{
	if (pthread_mutex_init(&q->mutex, NULL) != 0) {
		return false;
	}
	if (pthread_cond_init(&q->cond, NULL) != 0) {
		pthread_mutex_destroy(&q->mutex);
		return false;
	}
	return true;
}

static inline xrt_result_t
comp_macos_refresh_request(
    struct comp_macos_refresh_queue *q, float hz, int64_t timeout_ns, xrt_result_t (*apply)(void *, float), void *ctx)
{
	if (!isfinite(hz) || hz <= 0 || timeout_ns <= 0) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	pthread_mutex_lock(&q->mutex);
	if (q->pending || q->waiters > 0 || q->closing) {
		pthread_mutex_unlock(&q->mutex);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	if (!q->active) {
		xrt_result_t result = apply(ctx, hz);
		pthread_mutex_unlock(&q->mutex);
		return result;
	}
	q->hz = hz;
	q->pending = true;
	q->waiters++;
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += timeout_ns / 1000000000;
	deadline.tv_nsec += timeout_ns % 1000000000;
	if (deadline.tv_nsec >= 1000000000) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000;
	}
	while (q->pending && !q->closing) {
		if (pthread_cond_timedwait(&q->cond, &q->mutex, &deadline) != 0 && q->pending && !q->closing) {
			q->pending = false;
			q->result = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
			break;
		}
	}
	xrt_result_t result = q->closing ? XRT_ERROR_OUTPUT_REQUEST_FAILURE : q->result;
	q->waiters--;
	pthread_cond_broadcast(&q->cond);
	pthread_mutex_unlock(&q->mutex);
	return result;
}

static inline void
comp_macos_refresh_process(struct comp_macos_refresh_queue *q, xrt_result_t (*apply)(void *, float), void *ctx)
{
	pthread_mutex_lock(&q->mutex);
	if (q->pending && !q->closing) {
		q->result = apply(ctx, q->hz);
		q->pending = false;
		pthread_cond_broadcast(&q->cond);
	}
	pthread_mutex_unlock(&q->mutex);
}

//! Called by the compositor at session boundaries, serialized with idle API requests.
static inline void
comp_macos_refresh_set_active(struct comp_macos_refresh_queue *q,
                              bool active,
                              xrt_result_t (*apply)(void *, float),
                              void *ctx)
{
	pthread_mutex_lock(&q->mutex);
	if (!active && q->pending && !q->closing) {
		q->result = apply(ctx, q->hz);
		q->pending = false;
		pthread_cond_broadcast(&q->cond);
	}
	q->active = active;
	pthread_mutex_unlock(&q->mutex);
}

//! Caller must prevent new API calls before finalizing the target.
static inline void
comp_macos_refresh_fini(struct comp_macos_refresh_queue *q)
{
	pthread_mutex_lock(&q->mutex);
	q->closing = true;
	q->pending = false;
	pthread_cond_broadcast(&q->cond);
	while (q->waiters > 0) {
		pthread_cond_wait(&q->cond, &q->mutex);
	}
	pthread_mutex_unlock(&q->mutex);
	pthread_cond_destroy(&q->cond);
	pthread_mutex_destroy(&q->mutex);
}

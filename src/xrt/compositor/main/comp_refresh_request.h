// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Hands display refresh rate requests to the compositor thread.
 *
 * A refresh rate change touches the target and its pacer, which the compositor
 * thread uses every frame. While a session runs, API threads queue a request and
 * the compositor thread applies it between frames. While no session runs the
 * compositor thread is idle, so the request is applied directly. Both paths, and
 * creating or destroying the target, hold the same mutex.
 *
 * @ingroup comp_main
 */

#pragma once

#include "xrt/xrt_results.h"
#include "os/os_threading.h"

#ifdef __cplusplus
extern "C" {
#endif

//! Applies a refresh rate, called with the request mutex held.
typedef xrt_result_t (*comp_refresh_apply_func_t)(void *ctx, float display_refresh_rate_hz);

struct comp_refresh_request
{
	struct os_mutex mutex;
	struct os_cond cond;

	//! Is the compositor thread rendering frames, and so applying requests?
	bool session_active;

	//! A request is waiting for the compositor thread.
	bool pending;

	//! Shutting down, fail any waiter.
	bool closing;

	//! API threads inside @ref comp_refresh_request_submit, only one request at a time.
	uint32_t waiters;

	float hz;
	xrt_result_t result;
};

static inline int
comp_refresh_request_init(struct comp_refresh_request *r)
{
	int ret = os_mutex_init(&r->mutex);
	if (ret != 0) {
		return ret;
	}
	ret = os_cond_init(&r->cond);
	if (ret != 0) {
		os_mutex_destroy(&r->mutex);
	}
	return ret;
}

/*!
 * Called from an API thread. Returns the apply result, or
 * XRT_ERROR_OUTPUT_REQUEST_FAILURE if another request is outstanding, the
 * compositor is shutting down, or the compositor thread did not apply it in time.
 */
static inline xrt_result_t
comp_refresh_request_submit(
    struct comp_refresh_request *r, float hz, uint64_t timeout_ns, comp_refresh_apply_func_t apply, void *ctx)
{
	os_mutex_lock(&r->mutex);
	if (r->closing || r->pending || r->waiters > 0) {
		os_mutex_unlock(&r->mutex);
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}

	if (!r->session_active) {
		xrt_result_t result = apply(ctx, hz);
		os_mutex_unlock(&r->mutex);
		return result;
	}

	r->hz = hz;
	r->pending = true;
	r->waiters++;

	uint64_t deadline_ns = (uint64_t)os_monotonic_get_ns() + timeout_ns;
	while (r->pending && !r->closing) {
		uint64_t now_ns = (uint64_t)os_monotonic_get_ns();
		if (now_ns >= deadline_ns) {
			break;
		}
		(void)os_cond_timedwait(&r->cond, &r->mutex, deadline_ns - now_ns);
	}

	xrt_result_t result = r->result;
	if (r->pending) {
		// Timed out or closing: withdraw it, so it is never applied late.
		r->pending = false;
		result = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}

	r->waiters--;
	os_cond_broadcast(&r->cond);
	os_mutex_unlock(&r->mutex);
	return result;
}

static inline void
comp_refresh_request_apply_pending_locked(struct comp_refresh_request *r, comp_refresh_apply_func_t apply, void *ctx)
{
	if (r->pending && !r->closing) {
		r->result = apply(ctx, r->hz);
		r->pending = false;
		os_cond_broadcast(&r->cond);
	}
}

//! Called by the compositor thread between frames.
static inline void
comp_refresh_request_process(struct comp_refresh_request *r, comp_refresh_apply_func_t apply, void *ctx)
{
	os_mutex_lock(&r->mutex);
	comp_refresh_request_apply_pending_locked(r, apply, ctx);
	os_mutex_unlock(&r->mutex);
}

/*!
 * Called by the compositor thread when it starts or stops rendering frames,
 * with the mutex held by @ref comp_refresh_request_lock. Stopping applies any
 * outstanding request first.
 */
static inline void
comp_refresh_request_set_session_active_locked(struct comp_refresh_request *r,
                                               bool active,
                                               comp_refresh_apply_func_t apply,
                                               void *ctx)
{
	if (!active) {
		comp_refresh_request_apply_pending_locked(r, apply, ctx);
	}
	r->session_active = active;
}

//! Serialise target creation and destruction with refresh requests.
static inline void
comp_refresh_request_lock(struct comp_refresh_request *r)
{
	os_mutex_lock(&r->mutex);
}

static inline void
comp_refresh_request_unlock(struct comp_refresh_request *r)
{
	os_mutex_unlock(&r->mutex);
}

//! Fails any waiter and waits for it to leave. No new requests may arrive after this.
static inline void
comp_refresh_request_fini(struct comp_refresh_request *r)
{
	os_mutex_lock(&r->mutex);
	r->closing = true;
	os_cond_broadcast(&r->cond);
	while (r->waiters > 0) {
		os_cond_wait(&r->cond, &r->mutex);
	}
	os_mutex_unlock(&r->mutex);
	os_cond_destroy(&r->cond);
	os_mutex_destroy(&r->mutex);
}

#ifdef __cplusplus
}
#endif

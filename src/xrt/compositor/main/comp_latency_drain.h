// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Decides when to drain one frame from a backed-up presentation queue.
 *
 * On macOS a presented frame normally reaches the display a fixed number of
 * refreshes after its desired time. After a display-side stall the queue can
 * stay one frame deeper, adding a refresh of latency until something drains
 * it. Skipping one present removes that frame at the cost of one repeated
 * refresh. This only decides when: the caller observes the backlog and skips.
 *
 * @ingroup comp_main
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct comp_latency_drain
{
	//! How long the backlog must last before draining.
	int64_t sustain_ns;

	//! Minimum time between drains.
	int64_t cooldown_ns;

	//! When the current run of backlogged observations began, 0 when none.
	int64_t backlog_since_ns;

	//! Time of the last drain, 0 before the first.
	int64_t last_drain_ns;

	uint32_t drains;
};

static inline void
comp_latency_drain_init(struct comp_latency_drain *d, int64_t sustain_ns, int64_t cooldown_ns)
{
	// Field assignment rather than a compound literal: the tests include this from C++.
	d->sustain_ns = sustain_ns;
	d->cooldown_ns = cooldown_ns;
	d->backlog_since_ns = 0;
	d->last_drain_ns = 0;
	d->drains = 0;
}

/*!
 * Record one observation, returning true when the caller should skip a present
 * now. Call only with a definite answer; skip unknown observations.
 */
static inline bool
comp_latency_drain_observe(struct comp_latency_drain *d, int64_t now_ns, bool backlogged)
{
	if (!backlogged) {
		d->backlog_since_ns = 0;
		return false;
	}
	if (d->backlog_since_ns == 0) {
		d->backlog_since_ns = now_ns;
	}
	if (now_ns - d->backlog_since_ns < d->sustain_ns) {
		return false;
	}
	if (d->last_drain_ns != 0 && now_ns - d->last_drain_ns < d->cooldown_ns) {
		return false;
	}
	d->last_drain_ns = now_ns;
	d->backlog_since_ns = 0;
	d->drains++;
	return true;
}

#ifdef __cplusplus
}
#endif

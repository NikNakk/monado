// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Host/device clock offset for a PS Sense controller, from input report arrival times.
 *
 * Each input report gives one sample: the controller's clock (remote) and the host's arrival time (local).
 * remote - local is the true offset minus the report's Bluetooth latency, so the largest recent value is the best
 * estimate. The default mapping keeps that maximum, drags it down at a fixed rate so it can follow drift, smooths
 * the result and snaps to it on a large gap.
 *
 * That mapping moves whenever the link's latency floor moves: it sags while fast reports are scarce and steps
 * when one arrives. The LED schedule is locked against the mapping, so every such move shifts the pulse against
 * the camera exposure. On 4-5 Oct, mid-session snaps of 250-980 us were followed by long ring losses (the lock
 * tolerates about 350 us). A controller clock drifts smoothly; it does not step.
 *
 * Steady mode (opt-in) therefore holds the mapping once the LED lock is established: from then on the offset only
 * advances at a fitted drift rate. Any constant bias is absorbed by the optical lock, and phase tracking corrects
 * what remains. The rate is the median of lagged differences between per-bucket best samples, so a latency step
 * contaminates only the few differences that straddle it.
 *
 * @author Nick Kennedy
 * @ingroup drv_pssense
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Best (lowest-latency) samples kept for the drift fit.
#define PSSENSE_CLOCK_MAX_BUCKETS 256

struct pssense_clock_options
{
	//! Downward drag of the max-tracked offset, so it can follow drift, in parts per million.
	double leak_ppm;
	//! Largest change of the smoothed offset per sample, in ns.
	double max_step_ns;
	//! Jump straight to the max-tracked offset when the smoothed one is further than this from it; 0 never.
	double snap_ns;

	//! Hold the mapping, advancing at the fitted drift rate, while pssense_clock_set_hold() is on.
	bool steady;
	//! Width of a best-sample bucket, in ns.
	int64_t bucket_ns;
	//! Buckets the drift fit uses (the newest ones).
	uint32_t fit_buckets;
	//! Lag between the paired buckets of each difference, in buckets; shortened to half the buckets available,
	//! but not below min_lag_buckets, while fewer have been collected.
	uint32_t fit_lag_buckets;
	uint32_t min_lag_buckets;
	//! Fewest lagged differences for a usable rate.
	uint32_t min_differences;
	//! Fitted rates are clamped to this, in parts per million.
	double max_rate_ppm;
};

struct pssense_clock_bucket
{
	int64_t local_ns;
	double offset_ns;
};

struct pssense_clock
{
	struct pssense_clock_options options;

	bool have_offset;
	//! The max-tracked offset (remote - local), dragged down at leak_ppm.
	double envelope_ns;
	//! The offset to use: smoothed towards envelope_ns, or held.
	double offset_ns;
	int64_t last_local_ns;

	//! The last push snapped, by this much.
	bool snapped;
	double snap_delta_ns;

	//! Hold requested by the driver (LED lock held).
	bool hold_requested;
	//! Hold in effect: offset_ns = hold_offset_ns + rate * (local - hold_local_ns).
	bool holding;
	double hold_offset_ns;
	int64_t hold_local_ns;

	//! Fitted drift (d offset / d local), and whether there is one yet.
	bool have_rate;
	double rate;

	//! Best sample of the bucket being filled.
	bool bucket_open;
	int64_t bucket_start_ns;
	struct pssense_clock_bucket bucket_best;
	//! Completed buckets, oldest first (a ring of bucket_count from bucket_head).
	struct pssense_clock_bucket buckets[PSSENSE_CLOCK_MAX_BUCKETS];
	uint32_t bucket_head;
	uint32_t bucket_count;
};

void
pssense_clock_default_options(struct pssense_clock_options *options);

void
pssense_clock_init(struct pssense_clock *clock, const struct pssense_clock_options *options);

/*!
 * Add one input report: @p local_ns its host arrival time, @p remote_ns the controller's clock in ns.
 * Afterwards @ref pssense_clock::offset_ns is the offset to use and @ref pssense_clock::snapped says whether it
 * jumped.
 */
void
pssense_clock_push(struct pssense_clock *clock, int64_t local_ns, int64_t remote_ns);

/*!
 * Ask for the mapping to be held (steady mode only), typically while the LED schedule is locked. The hold starts
 * once a drift rate has been fitted, and continues from the current offset.
 */
void
pssense_clock_set_hold(struct pssense_clock *clock, bool hold);

#ifdef __cplusplus
}
#endif

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Host/device clock offset for a PS Sense controller, see @ref pssense_clock.h.
 * @author Nick Kennedy
 * @ingroup drv_pssense
 */

#include "pssense_clock.h"

#include <stdlib.h>
#include <string.h>

void
pssense_clock_default_options(struct pssense_clock_options *options)
{
	*options = (struct pssense_clock_options){
	    // See: PSVR2Toolkit/projects/psvr2_openvr_driver_ex/libpad_hooks.cpp
	    .leak_ppm = 50.0,
	    .max_step_ns = 2500.0,
	    .snap_ns = 0.0,
	    .steady = false,
	    .bucket_ns = 1000000000,
	    // 90 s of 1 s buckets, differences 10 s apart; from 8 s of buckets, 4 s apart.
	    .fit_buckets = 90,
	    .fit_lag_buckets = 10,
	    .min_lag_buckets = 4,
	    .min_differences = 4,
	    // Measured drift is 9-28 ppm; the headset's own robust clock caps at 200.
	    .max_rate_ppm = 200.0,
	};
}

void
pssense_clock_init(struct pssense_clock *clock, const struct pssense_clock_options *options)
{
	memset(clock, 0, sizeof(*clock));
	clock->options = *options;
	if (clock->options.fit_buckets > PSSENSE_CLOCK_MAX_BUCKETS) {
		clock->options.fit_buckets = PSSENSE_CLOCK_MAX_BUCKETS;
	}
}

static int
compare_double(const void *a, const void *b)
{
	double x = *(const double *)a;
	double y = *(const double *)b;
	return (x > y) - (x < y);
}

static const struct pssense_clock_bucket *
bucket_at(const struct pssense_clock *clock, uint32_t i)
{
	return &clock->buckets[(clock->bucket_head + i) % PSSENSE_CLOCK_MAX_BUCKETS];
}

static void
fit_rate(struct pssense_clock *clock)
{
	uint32_t n = clock->bucket_count;
	uint32_t first = n > clock->options.fit_buckets ? n - clock->options.fit_buckets : 0;
	uint32_t lag = clock->options.fit_lag_buckets;
	if ((n - first) / 2 < lag) {
		lag = (n - first) / 2;
	}
	if (lag == 0 || lag < clock->options.min_lag_buckets) {
		return;
	}

	double rates[PSSENSE_CLOCK_MAX_BUCKETS];
	uint32_t count = 0;
	for (uint32_t i = first; i + lag < n; i++) {
		const struct pssense_clock_bucket *a = bucket_at(clock, i);
		const struct pssense_clock_bucket *b = bucket_at(clock, i + lag);
		double dt = (double)(b->local_ns - a->local_ns);
		if (dt <= 0.0) {
			continue;
		}
		rates[count++] = (b->offset_ns - a->offset_ns) / dt;
	}
	if (count < clock->options.min_differences || count == 0) {
		return;
	}
	qsort(rates, count, sizeof(rates[0]), compare_double);
	double rate = count % 2 ? rates[count / 2] : 0.5 * (rates[count / 2 - 1] + rates[count / 2]);
	double limit = clock->options.max_rate_ppm * 1e-6;
	clock->rate = rate > limit ? limit : (rate < -limit ? -limit : rate);
	clock->have_rate = true;
}

static void
close_bucket(struct pssense_clock *clock)
{
	if (!clock->bucket_open) {
		return;
	}
	uint32_t index;
	if (clock->bucket_count < PSSENSE_CLOCK_MAX_BUCKETS) {
		index = (clock->bucket_head + clock->bucket_count) % PSSENSE_CLOCK_MAX_BUCKETS;
		clock->bucket_count++;
	} else {
		index = clock->bucket_head;
		clock->bucket_head = (clock->bucket_head + 1) % PSSENSE_CLOCK_MAX_BUCKETS;
	}
	clock->buckets[index] = clock->bucket_best;
	clock->bucket_open = false;

	if (clock->holding) {
		// Re-base so a new rate continues from the current held offset.
		double held =
		    clock->hold_offset_ns + clock->rate * (double)(clock->bucket_best.local_ns - clock->hold_local_ns);
		fit_rate(clock);
		clock->hold_offset_ns = held;
		clock->hold_local_ns = clock->bucket_best.local_ns;
	} else {
		fit_rate(clock);
	}
}

static void
add_to_bucket(struct pssense_clock *clock, int64_t local_ns, double offset_ns)
{
	if (clock->bucket_open && local_ns - clock->bucket_start_ns >= clock->options.bucket_ns) {
		close_bucket(clock);
	}
	if (!clock->bucket_open) {
		clock->bucket_open = true;
		clock->bucket_start_ns = local_ns;
		clock->bucket_best = (struct pssense_clock_bucket){local_ns, offset_ns};
	} else if (offset_ns > clock->bucket_best.offset_ns) {
		clock->bucket_best = (struct pssense_clock_bucket){local_ns, offset_ns};
	}
}

void
pssense_clock_push(struct pssense_clock *clock, int64_t local_ns, int64_t remote_ns)
{
	const double sample = (double)(remote_ns - local_ns);
	clock->snapped = false;
	clock->snap_delta_ns = 0.0;

	if (!clock->have_offset) {
		clock->envelope_ns = sample;
		clock->offset_ns = sample;
		clock->have_offset = true;
		clock->last_local_ns = local_ns;
		add_to_bucket(clock, local_ns, sample);
		return;
	}

	// The default mapping always runs, so a hold can start from it and its envelope stays observable.
	double elapsed_ns = (double)(local_ns - clock->last_local_ns);
	clock->envelope_ns -= elapsed_ns * clock->options.leak_ppm * 1e-6;
	if (clock->envelope_ns < sample) {
		clock->envelope_ns = sample;
	}
	clock->last_local_ns = local_ns;
	add_to_bucket(clock, local_ns, sample);

	if (clock->options.steady && clock->hold_requested && !clock->holding && clock->have_rate) {
		clock->holding = true;
		clock->hold_offset_ns = clock->offset_ns;
		clock->hold_local_ns = local_ns;
	}
	if (clock->holding) {
		clock->offset_ns = clock->hold_offset_ns + clock->rate * (double)(local_ns - clock->hold_local_ns);
		return;
	}

	double delta = clock->envelope_ns - clock->offset_ns;
	if (clock->options.snap_ns > 0.0 && (delta > clock->options.snap_ns || -delta > clock->options.snap_ns)) {
		clock->snapped = true;
		clock->snap_delta_ns = delta;
	} else if (delta > clock->options.max_step_ns) {
		delta = clock->options.max_step_ns;
	} else if (delta < -clock->options.max_step_ns) {
		delta = -clock->options.max_step_ns;
	}
	clock->offset_ns += delta;
}

void
pssense_clock_set_hold(struct pssense_clock *clock, bool hold)
{
	clock->hold_requested = hold;
	if (!hold && clock->holding) {
		// Resume the default mapping from the held offset; it converges (or snaps) from here.
		clock->holding = false;
	}
}

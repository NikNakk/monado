// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Caller serializes access. All timestamps use Monado's monotonic clock.
 * This bounded mailbox owns no renderer, GPU resource or diagnostic file. */
struct macos_present_sample
{
	uint64_t generation, frame, timeline;
	int64_t desired_ns, presented_ns, observed_ns, period_ns;
};

struct macos_present_feedback
{
	uint64_t generation, serial;
	struct macos_present_sample latest;
	/* Observation only: the lowest repeatedly observed stable latency band is
	 * a provisional anchor, not proof of the minimum physical pipeline delay.
	 * Startup may already be buffered. Never increase it merely because every
	 * sample in a rolling window is buffered. */
	int64_t baseline_ns, candidate_ns;
	uint32_t candidate_samples;
	int64_t extra_refreshes, candidate_extra;
	uint32_t extra_samples;
	bool baseline_ready, extra_ready;
};

static inline void
macos_present_feedback_reset(struct macos_present_feedback *f)
{
	uint64_t generation = f->generation + 1;
	uint64_t serial = f->serial + 1;
	memset(f, 0, sizeof(*f));
	f->generation = generation;
	f->serial = serial;
}

static inline int64_t
macos_present_feedback_distance(int64_t a, int64_t b)
{
	return a > b ? a - b : b - a;
}

static inline bool
macos_present_feedback_observe(struct macos_present_feedback *f, const struct macos_present_sample *s)
{
	/* Four periods is also the legacy EMA's maximum accepted latency. Reject
	 * delayed callbacks rather than using an old presentation as current state. */
	if (s->generation != f->generation || s->frame == 0 || s->period_ns <= 0 || s->period_ns > 1000000000 ||
	    s->desired_ns <= 0 || s->presented_ns <= s->desired_ns || s->observed_ns < s->presented_ns ||
	    s->observed_ns - s->presented_ns > 4 * s->period_ns || s->presented_ns - s->desired_ns > 4 * s->period_ns ||
	    (f->latest.frame != 0 && (s->frame <= f->latest.frame || s->presented_ns <= f->latest.presented_ns))) {
		return false;
	}
	if (f->latest.frame != 0 &&
	    macos_present_feedback_distance(s->period_ns, f->latest.period_ns) > f->latest.period_ns / 100) {
		return false; // A substantive period change requires a new generation.
	}
	if (f->latest.frame != 0 && s->presented_ns - f->latest.presented_ns > 4 * s->period_ns) {
		/* A visibility/presentation gap invalidates qualification. Do not change
		 * the generation here: later frames already submitted still belong to it. */
		f->baseline_ready = false;
		f->candidate_samples = f->extra_samples = 0;
		f->extra_ready = false;
	}
	f->latest = *s;
	f->serial++;
	int64_t latency = s->presented_ns - s->desired_ns;
	int64_t tolerance = s->period_ns / 8;
	if (!f->baseline_ready || latency < f->baseline_ns - tolerance) {
		if (f->candidate_samples == 0 ||
		    macos_present_feedback_distance(latency, f->candidate_ns) > tolerance) {
			f->candidate_ns = latency;
			f->candidate_samples = 1;
		} else {
			f->candidate_ns += (latency - f->candidate_ns) / 8;
			f->candidate_samples++;
		}
		if (f->candidate_samples >= 8) {
			f->baseline_ns = f->candidate_ns;
			f->baseline_ready = true;
			f->candidate_samples = 0;
			f->extra_ready = false;
			f->extra_samples = 0;
		}
	} else {
		f->candidate_samples = 0;
		if (macos_present_feedback_distance(latency, f->baseline_ns) <= tolerance) {
			f->baseline_ns += (latency - f->baseline_ns) / 8;
		}
	}
	if (!f->baseline_ready) {
		return true;
	}
	int64_t excess = latency - f->baseline_ns;
	int64_t extra = excess > 0 ? (excess + s->period_ns / 2) / s->period_ns : 0;
	if (macos_present_feedback_distance(latency, f->baseline_ns + extra * s->period_ns) > tolerance) {
		f->extra_samples = 0;
		f->extra_ready = false;
		return true;
	}
	if (f->extra_samples == 0 || extra != f->candidate_extra) {
		f->candidate_extra = extra;
		f->extra_samples = 1;
	} else if (f->extra_samples < 3) {
		f->extra_samples++;
	}
	/* Three consistent observations qualify a change. Retain the previous
	 * qualified state during a candidate run; isolated misses cannot change it. */
	if (f->extra_samples >= 3) {
		f->extra_refreshes = extra;
		f->extra_ready = true;
	}
	return true;
}

/* Legacy predictor calibration, deliberately separate from the observation-only
 * split estimator. Consume at most the newest sample once per compositor tick,
 * just as the old atomic mailbox did, regardless of diagnostic sinks. */
struct macos_present_calibration
{
	uint64_t generation, serial;
	int64_t presented_ns, offset_ns;
	uint32_t count;
};

static inline bool
macos_present_calibration_consume(struct macos_present_calibration *c,
                                  const struct macos_present_feedback *f,
                                  int64_t now_ns,
                                  int64_t period_ns)
{
	if (period_ns <= 0 || period_ns > 1000000000) {
		return false;
	}
	if (c->generation != f->generation ||
	    (c->presented_ns != 0 && f->latest.presented_ns > c->presented_ns + 4 * period_ns)) {
		c->generation = f->generation;
		c->count = 0;
		c->offset_ns = 0;
	}
	if (c->serial == f->serial) {
		return false;
	}
	c->serial = f->serial;
	c->presented_ns = f->latest.presented_ns;
	int64_t latency = f->latest.presented_ns - f->latest.desired_ns;
	if (f->latest.frame == 0 || f->latest.generation != f->generation || latency <= 0 || latency > 4 * period_ns ||
	    now_ns < f->latest.presented_ns || now_ns - f->latest.presented_ns > 4 * period_ns) {
		return false;
	}
	c->offset_ns = c->count == 0 ? latency : (c->offset_ns * 7 + latency) / 8;
	if (c->count < UINT32_MAX) {
		c->count++;
	}
	return c->count >= 8;
}

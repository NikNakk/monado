// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Brightness-driven LED phase bootstrap for constellation-tracked controllers.
 *
 * The pose-driven @ref t_led_sync_refinement only learns that a controller is lit once a pose has been
 * solved. On PS VR2 that is too strict: the lit window is narrow, a single camera often sees only a few LEDs,
 * and a bootstrap that depends on successful pose solves can scan forever over dark frames.
 *
 * This bootstrap instead scores each commanded LED phase by the raw blob counts reported by every camera.
 * It first scans the whole camera period with a wide pulse, then scans the neighbourhood of the best wide
 * phase with a narrow pulse to find the edges of the lit window, and finally locks the pulse centre in the
 * middle of that window. It holds no locks of its own; the caller serialises every call.
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "util/u_time.h"
#include "util/u_logging.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define T_LED_PHASE_BOOTSTRAP_MAX_CAMERAS 8
#define T_LED_PHASE_BOOTSTRAP_MAX_STEPS 128

enum t_led_phase_bootstrap_state
{
	//! Not scanning. The LEDs should be held off. Waiting for @ref t_led_phase_bootstrap_start.
	T_LED_PHASE_BOOTSTRAP_IDLE = 0,
	//! Stepping a wide pulse over the whole camera period.
	T_LED_PHASE_BOOTSTRAP_WIDE_SCAN = 1,
	//! Stepping a narrow pulse around the best wide phase to find the lit window's edges.
	T_LED_PHASE_BOOTSTRAP_NARROW_SCAN = 2,
	//! Holding the pulse centred in the measured lit window.
	T_LED_PHASE_BOOTSTRAP_LOCKED = 3,
};

struct t_led_phase_bootstrap_options
{
	//! Logging level and a short label (e.g. "L"/"R") used in log lines.
	enum u_logging_level log_level;
	char label;

	//! Number of cameras that will report blob counts, at most @ref T_LED_PHASE_BOOTSTRAP_MAX_CAMERAS.
	uint32_t camera_count;

	//! Pulse width and step for the full-period wide scan.
	time_duration_ns wide_blink_ns;
	time_duration_ns wide_step_ns;

	//! Pulse width and step for the narrow edge scan.
	time_duration_ns narrow_blink_ns;
	time_duration_ns narrow_step_ns;
	//! How far beyond the wide pulse on each side the narrow scan extends.
	time_duration_ns narrow_margin_ns;

	//! Pulse width used once locked.
	time_duration_ns lock_blink_ns;

	//! Exposures to ignore after changing the phase, while the new setting reaches the controller.
	uint32_t settle_frames;
	//! Exposures measured for each step.
	uint32_t measure_frames;
	//! Extra exposures to wait for late blob reports before scoring a step.
	uint32_t grace_frames;

	//! A camera frame counts as lit when it reports at least this many blobs.
	uint32_t min_blobs_per_camera;
	//! The best step must reach this score (camera-equivalents of lit frames) to be accepted.
	float min_peak_score;
	//! The best wide step must exceed the median wide step score by this much.
	float min_peak_contrast;

	//! Once locked, rescan after this many exposures without any lit camera frame.
	uint32_t lost_frames;
};

//! Result of one scan step, for logging and tests.
struct t_led_phase_bootstrap_step
{
	time_duration_ns fudge_offset_ns;
	time_duration_ns blink_ns;
	//! Sum over cameras of the fraction of reported frames that were lit.
	float score;
	//! Mean blob count per reported frame, summed over cameras.
	float mean_blobs;
	uint32_t reported[T_LED_PHASE_BOOTSTRAP_MAX_CAMERAS];
	uint32_t lit[T_LED_PHASE_BOOTSTRAP_MAX_CAMERAS];
};

struct t_led_phase_bootstrap
{
	struct t_led_phase_bootstrap_options options;

	enum t_led_phase_bootstrap_state state;
	//! Camera period used for the current scan.
	time_duration_ns period_ns;

	//! Output: what the driver should program.
	time_duration_ns fudge_offset_ns;
	time_duration_ns blink_ns;
	//! Incremented whenever the output changes, so the driver can latch a new LED sequence.
	uint32_t output_generation;

	//! Steps of the current scan.
	struct t_led_phase_bootstrap_step steps[T_LED_PHASE_BOOTSTRAP_MAX_STEPS];
	uint32_t step_count;
	uint32_t step_index;
	time_duration_ns scan_start_ns;
	time_duration_ns scan_step_ns;

	//! Exposure accounting for the current step.
	uint32_t exposures_in_step;
	bool window_pending;
	bool window_open;
	int64_t window_start_ns;
	int64_t window_end_ns;
	uint64_t blob_sum[T_LED_PHASE_BOOTSTRAP_MAX_CAMERAS];

	//! Result of the last completed bootstrap.
	bool have_lock;
	time_duration_ns lit_start_ns;
	time_duration_ns lit_end_ns;
	uint32_t scans_attempted;
	uint32_t locks_acquired;

	//! Exposures left before a failed scan may be retried.
	uint32_t idle_backoff_frames;

	//! Locked-state monitoring.
	uint32_t frames_since_lit;
	uint32_t locked_reports;
	uint32_t locked_lit_reports;
};

//! Fills in the defaults used by the PS Sense driver (PS VR2 mode-4 cameras).
void
t_led_phase_bootstrap_default_options(struct t_led_phase_bootstrap_options *options);

void
t_led_phase_bootstrap_init(struct t_led_phase_bootstrap *b, const struct t_led_phase_bootstrap_options *options);

//! Begin (or restart) a full scan using the given camera period.
void
t_led_phase_bootstrap_start(struct t_led_phase_bootstrap *b, time_duration_ns period_ns);

//! Stop scanning and return to IDLE; the lock result is kept for diagnostics.
void
t_led_phase_bootstrap_stop(struct t_led_phase_bootstrap *b);

/*!
 * Push one camera exposure event. Advances the scan once a step's measurement window has completed.
 *
 * @return true if the output (phase or pulse width) changed and must be sent to the controller.
 */
bool
t_led_phase_bootstrap_push_exposure(struct t_led_phase_bootstrap *b, int64_t exposure_timestamp_ns);

//! Push the number of blobs one camera saw in the frame exposed at @p exposure_timestamp_ns.
void
t_led_phase_bootstrap_push_blob_count(struct t_led_phase_bootstrap *b,
                                      uint32_t camera_index,
                                      int64_t exposure_timestamp_ns,
                                      uint32_t blob_count);

//! True while the bootstrap wants the IR emitters lit (scanning or locked).
static inline bool
t_led_phase_bootstrap_leds_enabled(const struct t_led_phase_bootstrap *b)
{
	return b->state != T_LED_PHASE_BOOTSTRAP_IDLE;
}

//! True when idle and not backing off after a failed scan, i.e. the caller may call start.
static inline bool
t_led_phase_bootstrap_ready_to_scan(const struct t_led_phase_bootstrap *b)
{
	return b->state == T_LED_PHASE_BOOTSTRAP_IDLE && b->idle_backoff_frames == 0;
}

//! True while a scan is in progress (the scanning controller must be the only one lit).
static inline bool
t_led_phase_bootstrap_is_scanning(const struct t_led_phase_bootstrap *b)
{
	return b->state == T_LED_PHASE_BOOTSTRAP_WIDE_SCAN || b->state == T_LED_PHASE_BOOTSTRAP_NARROW_SCAN;
}

//! Wrap an offset into [0, period).
time_duration_ns
t_led_phase_bootstrap_wrap(time_duration_ns offset_ns, time_duration_ns period_ns);

#ifdef __cplusplus
}
#endif

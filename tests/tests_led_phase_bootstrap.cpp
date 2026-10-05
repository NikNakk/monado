// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "catch_amalgamated.hpp"
#include "t_led_phase_bootstrap.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>


namespace {

constexpr int64_t kPeriod = 16683000;

//! A simulated controller + four cameras. The LEDs are lit in a frame when the programmed pulse, shifted by an
//! unknown latency, overlaps the exposure.
struct Sim
{
	int64_t latency_ns;
	int64_t exposure_start_ns = 0;
	int64_t exposure_ns = 400000;
	uint32_t apply_delay_frames = 3;
	uint32_t report_delay_frames = 2;
	uint32_t lit_blobs = 5;
	uint32_t dark_blobs = 1;
	bool visible = true;
	uint32_t visible_cameras = 4;
	//! Blobs each camera sees from other light sources (windows, lamps) whatever the LEDs do.
	std::array<uint32_t, 4> background{};
	//! Latency change per frame, as the host clock mappings wander.
	int64_t latency_drift_ns_per_frame = 0;
	//! Grant tracking probes as soon as they are wanted, as the driver does when no other controller scans.
	bool grant_probes = true;
	//! Extra blobs on every camera, redrawn from 0..noise_blobs every noise_block_frames (a moving hand, the
	//! other controller's ring passing through view).
	uint32_t noise_blobs = 0;
	uint32_t noise_block_frames = 30;
	//! Also push a pose coverage for each lit frame, as the driver does for each joint solve.
	bool push_coverage = false;
	//! One block in this many (of noise_block_frames) loses every solve, as when the hand hides the ring.
	uint32_t solve_dropout_one_in = 0;
	//! Push each camera's own matched blob count, as the joint tracker does (lit frames match 5).
	bool push_own = false;
	//! From this frame on the ring is lit whatever it is told (the always-lit fault); UINT32_MAX never.
	uint32_t stuck_from_frame = UINT32_MAX;
	//! Every this many frames a visible ring counts as lit whatever its phase (a reflection, a hand passing
	//! through the edge of the window); 0 never.
	uint32_t stray_lit_every = 0;
	//! The ring stays dark while the applied fudge (wrapped) lies in (dark_from_ns, dark_to_ns]: a step that a hand
	//! or a turn darkened. Disabled when equal.
	int64_t dark_from_ns = 0;
	int64_t dark_to_ns = 0;
	uint32_t frames_run = 0;
	uint32_t frames_lit = 0;

	std::deque<std::pair<int64_t, int64_t>> pending_outputs{}; // (fudge, blink) queued for delayed application
	//! (timestamp, lit) awaiting delayed report; kept across run() calls so short runs lose none.
	std::deque<std::pair<int64_t, bool>> reports{};
	int64_t applied_fudge = 0;
	int64_t applied_blink = 0;

	bool
	lit(int64_t fudge, int64_t blink) const
	{
		if (!visible || blink <= 0) {
			return false;
		}
		if (dark_to_ns != dark_from_ns) {
			int64_t f = ((fudge % kPeriod) + kPeriod) % kPeriod;
			int64_t from = ((dark_from_ns % kPeriod) + kPeriod) % kPeriod;
			int64_t d = ((f - from) % kPeriod + kPeriod) % kPeriod;
			if (d > 0 && d <= dark_to_ns - dark_from_ns) {
				return false;
			}
		}
		// Compare in a frame one period either side to handle wrap.
		for (int k = -1; k <= 1; k++) {
			int64_t start = fudge + latency_ns + k * kPeriod;
			start = ((start % kPeriod) + kPeriod) % kPeriod + k * kPeriod;
			int64_t end = start + blink;
			if (start < exposure_start_ns + exposure_ns && end > exposure_start_ns) {
				return true;
			}
		}
		return false;
	}

	//! Runs @p frames exposures through the bootstrap.
	void
	run(t_led_phase_bootstrap &b, uint32_t frames, uint32_t &frame_index)
	{
		for (uint32_t i = 0; i < frames; i++, frame_index++) {
			int64_t ts = (int64_t)frame_index * kPeriod;

			// Settings take a few frames to reach the controller.
			pending_outputs.emplace_back(b.fudge_offset_ns,
			                             t_led_phase_bootstrap_leds_enabled(&b) ? b.blink_ns : 0);
			if (pending_outputs.size() > apply_delay_frames) {
				applied_fudge = pending_outputs.front().first;
				applied_blink = pending_outputs.front().second;
				pending_outputs.pop_front();
			}

			bool frame_lit = lit(applied_fudge, applied_blink) ||
			                 (visible && frame_index >= stuck_from_frame) ||
			                 (visible && stray_lit_every > 0 && frame_index % stray_lit_every == 0);
			frames_run++;
			frames_lit += frame_lit ? 1 : 0;
			reports.emplace_back(ts, frame_lit);
			t_led_phase_bootstrap_push_exposure(&b, ts);
			if (grant_probes && t_led_phase_bootstrap_wants_probe(&b)) {
				t_led_phase_bootstrap_begin_probe(&b);
			}
			latency_ns += latency_drift_ns_per_frame;

			// Blob reports lag the exposure event.
			while (reports.size() > report_delay_frames) {
				auto [rts, rlit] = reports.front();
				reports.pop_front();
				// A fixed hash of the block index, so runs are repeatable.
				uint64_t block = (uint64_t)(rts / kPeriod) / noise_block_frames;
				uint64_t h = (block + 1) * 0x9E3779B97F4A7C15ull;
				h ^= h >> 29;
				uint32_t noise = noise_blobs > 0 ? (uint32_t)(h % (noise_blobs + 1)) : 0;
				bool dropped = solve_dropout_one_in > 0 && (h >> 7) % solve_dropout_one_in == 0;
				if (push_coverage && rlit && visible_cameras >= 2 && !dropped) {
					t_led_phase_bootstrap_push_pose_coverage(&b, rts, 1.0f);
				}
				for (uint32_t c = 0; c < 4; c++) {
					bool cam_lit = rlit && c < visible_cameras;
					t_led_phase_bootstrap_push_blob_count(
					    &b, c, rts, background[c] + noise + (cam_lit ? lit_blobs : dark_blobs));
					if (push_own) {
						t_led_phase_bootstrap_push_own_matched(
						    &b, c, rts, cam_lit && visible_cameras >= 2 ? 5 : 0);
					}
				}
			}
		}
	}
};

t_led_phase_bootstrap_options
test_options()
{
	t_led_phase_bootstrap_options options;
	t_led_phase_bootstrap_default_options(&options);
	options.log_level = U_LOGGING_WARN;
	options.label = 'T';
	return options;
}

//! Distance on the period circle.
int64_t
circular_distance(int64_t a, int64_t b)
{
	int64_t d = t_led_phase_bootstrap_wrap(a - b, kPeriod);
	return std::min(d, kPeriod - d);
}

} // namespace


TEST_CASE("LED phase bootstrap locks the pulse centre onto the exposure centre")
{
	// Latencies chosen to put the lit window at, and across, the period wrap.
	// 2.1 ms is the protocol maximum the wide scan used; 1.6 ms (period id 32) is what the session script uses now.
	for (int64_t wide_blink : {int64_t(2100000), int64_t(1600000)})
		for (int64_t latency :
		     {int64_t(0), int64_t(3600000), int64_t(-1100000), int64_t(14000000), int64_t(9000000)}) {
			CAPTURE(wide_blink, latency);
			t_led_phase_bootstrap_options options = test_options();
			options.wide_blink_ns = wide_blink;
			t_led_phase_bootstrap b;
			t_led_phase_bootstrap_init(&b, &options);
			REQUIRE(t_led_phase_bootstrap_ready_to_scan(&b));
			t_led_phase_bootstrap_start(&b, kPeriod);
			REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_BASELINE);
			REQUIRE_FALSE(t_led_phase_bootstrap_leds_enabled(&b));
			REQUIRE(t_led_phase_bootstrap_is_scanning(&b));

			Sim sim{.latency_ns = latency};
			uint32_t frame = 0;
			sim.run(b, 2000, frame);

			REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
			REQUIRE(b.have_lock);
			REQUIRE(b.blink_ns == options.lock_blink_ns);

			int64_t pulse_centre = b.fudge_offset_ns + latency + b.blink_ns / 2;
			int64_t exposure_centre = sim.exposure_start_ns + sim.exposure_ns / 2;
			CHECK(circular_distance(pulse_centre, exposure_centre) <= options.narrow_step_ns);

			// A lock should hold while the controller stays visible.
			sim.run(b, 1000, frame);
			CHECK(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
			CHECK(b.locks_acquired == 1);
		}
}

TEST_CASE("LED phase bootstrap centres its lock across one dark narrow step")
{
	/*
	 * A lit window about seven narrow steps wide, as on hardware (1.2 ms exposure + 0.45 ms pulse), with the
	 * second lit step dark. Every lit step scores the same, so the peak is the window's first step, and the old
	 * rule (stop at the first weak step) locked on that step alone: the window's edge, as the left did on 3 Oct.
	 */
	const int64_t latency = 3600000;
	for (uint32_t gap : {0u, 1u}) {
		CAPTURE(gap);
		t_led_phase_bootstrap_options options = test_options();
		options.narrow_gap_steps = gap;
		// Without bridging the run is the edge step alone, which the minimum window would now reject outright.
		options.narrow_min_lit_steps = gap == 0 ? 1 : options.narrow_min_lit_steps;
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);

		Sim sim{.latency_ns = latency};
		sim.exposure_ns = 1200000;
		// Lit starts lie in (-narrow_blink - latency, exposure - latency); darken the second narrow step in it.
		sim.dark_from_ns = -options.narrow_blink_ns - latency + options.narrow_step_ns;
		sim.dark_to_ns = sim.dark_from_ns + options.narrow_step_ns;
		uint32_t frame = 0;
		sim.run(b, 2000, frame);
		REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);

		int64_t pulse_centre = b.fudge_offset_ns + latency + b.blink_ns / 2;
		int64_t exposure_centre = sim.exposure_start_ns + sim.exposure_ns / 2;
		int64_t error = circular_distance(pulse_centre, exposure_centre);
		CAPTURE(error);
		if (gap == 0) {
			CHECK(error > 2 * options.narrow_step_ns);
		} else {
			CHECK(error <= options.narrow_step_ns);
		}
	}
}

TEST_CASE("LED phase bootstrap rejects a narrow scan that saw the ring for a single step")
{
	/*
	 * 4 Oct (A-steady, OpenBrush): the right's narrow scan lit one step only, the ring being out of view for the
	 * rest, and it locked on that step about 1 ms from the window's centre. Here the hardware-sized window (1.2 ms
	 * exposure) is dark everywhere but its first step.
	 */
	const int64_t latency = 3600000;
	for (uint32_t min_steps : {1u, 3u}) {
		CAPTURE(min_steps);
		t_led_phase_bootstrap_options options = test_options();
		options.narrow_min_lit_steps = min_steps;
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);

		Sim sim{.latency_ns = latency};
		sim.exposure_ns = 1200000;
		uint32_t frame = 0;
		// The wide stage sees the whole window; the ring then turns away for all but the first narrow step.
		while (frame < 4000 && b.state != T_LED_PHASE_BOOTSTRAP_NARROW_SCAN) {
			sim.run(b, 1, frame);
		}
		REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_NARROW_SCAN);
		// Lit starts lie in (-narrow_blink - latency, exposure - latency); keep only the first narrow step.
		sim.dark_from_ns = -options.narrow_blink_ns - latency + options.narrow_step_ns;
		sim.dark_to_ns = sim.dark_from_ns + sim.exposure_ns + options.narrow_blink_ns;
		while (frame < 8000 && b.locks_acquired == 0 && b.consecutive_failures == 0) {
			sim.run(b, 1, frame);
		}

		int64_t exposure_centre = sim.exposure_start_ns + sim.exposure_ns / 2;
		if (min_steps == 1) {
			// The old behaviour: a lock well off the exposure centre.
			REQUIRE(b.locks_acquired == 1);
			int64_t pulse_centre = b.fudge_offset_ns + latency + b.blink_ns / 2;
			CHECK(circular_distance(pulse_centre, exposure_centre) > 2 * options.narrow_step_ns);
			continue;
		}

		// The scan fails instead, and once the whole window is visible the next one locks on its centre.
		REQUIRE(b.locks_acquired == 0);
		CHECK(b.consecutive_failures == 1);
		sim.dark_from_ns = sim.dark_to_ns = 0;
		// The driver restarts a failed bootstrap once its back-off has passed.
		while (frame < 12000 && !t_led_phase_bootstrap_ready_to_scan(&b)) {
			sim.run(b, 1, frame);
		}
		REQUIRE(t_led_phase_bootstrap_ready_to_scan(&b));
		t_led_phase_bootstrap_start(&b, kPeriod);
		sim.run(b, 4000, frame);
		REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
		int64_t pulse_centre = b.fudge_offset_ns + latency + b.blink_ns / 2;
		CHECK(circular_distance(pulse_centre, exposure_centre) <= options.narrow_step_ns);
	}
}

TEST_CASE("LED phase bootstrap rescans a lock that stray lit frames keep from timing out")
{
	/*
	 * 5 Oct: after a 0.9 ms clock step the left's lock no longer lit the ring, but an occasional lit frame kept
	 * resetting frames_since_lit, so it never rescanned and stayed dark for 25 s.
	 */
	for (float fraction : {0.0f, 0.1f}) {
		CAPTURE(fraction);
		t_led_phase_bootstrap_options options = test_options();
		options.lost_lit_fraction = fraction;
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);

		Sim sim{.latency_ns = 3600000};
		sim.exposure_ns = 1200000;
		uint32_t frame = 0;
		sim.run(b, 2000, frame);
		REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
		REQUIRE(b.locks_acquired == 1);

		// The pulse now lands 2 ms away from the exposure; one frame in 20 still looks lit.
		sim.latency_ns += 2000000;
		sim.stray_lit_every = 20;
		for (uint32_t i = 0; i < 3000 && b.locks_acquired == 1; i++) {
			sim.run(b, 1, frame);
			if (t_led_phase_bootstrap_ready_to_scan(&b)) {
				t_led_phase_bootstrap_start(&b, kPeriod);
			}
		}
		if (fraction == 0.0f) {
			// frames_since_lit alone never times out: still on the stale lock.
			CHECK(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
			CHECK(b.locks_acquired == 1);
			continue;
		}
		REQUIRE(b.locks_acquired == 2);
		int64_t pulse_centre = b.fudge_offset_ns + sim.latency_ns + b.blink_ns / 2;
		int64_t exposure_centre = sim.exposure_start_ns + sim.exposure_ns / 2;
		CHECK(circular_distance(pulse_centre, exposure_centre) <= options.narrow_step_ns);
	}
}

TEST_CASE("LED phase bootstrap checks a narrow run lit to the end of the scan for a stuck ring")
{
	// 5 Oct: the left became stuck lit midway through a narrow scan, which then "locked" on a run lit from its
	// sixth step to the end.
	for (uint32_t check_steps : {0u, 8u}) {
		CAPTURE(check_steps);
		t_led_phase_bootstrap_options options = test_options();
		options.detect_stuck_lit = true;
		options.stuck_check_unbounded_steps = check_steps;
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);

		Sim sim{.latency_ns = 3600000};
		sim.push_own = true;
		uint32_t frame = 0;
		while (frame < 4000 && !(b.state == T_LED_PHASE_BOOTSTRAP_NARROW_SCAN && b.step_index == 3)) {
			sim.run(b, 1, frame);
		}
		REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_NARROW_SCAN);
		sim.stuck_from_frame = frame;
		while (frame < 8000 && b.state != T_LED_PHASE_BOOTSTRAP_LOCKED &&
		       b.state != T_LED_PHASE_BOOTSTRAP_STUCK_LIT) {
			sim.run(b, 1, frame);
		}
		if (check_steps == 0) {
			// The old behaviour: a lock on the stuck ring.
			CHECK(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
		} else {
			CHECK(b.state == T_LED_PHASE_BOOTSTRAP_STUCK_LIT);
			CHECK(b.locks_acquired == 0);
		}
	}
}

TEST_CASE("LED phase bootstrap stuck check passes a healthy ring and then locks")
{
	// A healthy run lit to the end of the scan (a long exposure) passes the dark check and locks centred.
	t_led_phase_bootstrap_options options = test_options();
	options.detect_stuck_lit = true;
	options.stuck_check_unbounded_steps = 4;
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);
	t_led_phase_bootstrap_start(&b, kPeriod);
	Sim sim{.latency_ns = 3600000};
	sim.push_own = true;
	sim.exposure_ns = 2600000;
	uint32_t frame = 0;
	sim.run(b, 3000, frame);
	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
	CHECK(b.locks_acquired == 1);
}

TEST_CASE("LED phase bootstrap locks a weak hinted ring instead of falling back to the full scan")
{
	/*
	 * 5 Oct (083028): the right ring, seen by two cameras, lit only 1-2 steps of every hinted scan; after the
	 * retries the full scan's wide pulses preceded the always-lit fault. Here the window is dark everywhere but
	 * one narrow step, at the hinted place.
	 */
	const int64_t latency = 3600000;
	t_led_phase_bootstrap_options options = test_options();
	options.hint_retries = 2;
	options.failed_backoff_frames = 10;
	// Lit starts lie in (-narrow_blink - latency, exposure - latency): hint the window's middle.
	options.hint_fudge_ns = t_led_phase_bootstrap_wrap(-latency + 300000, kPeriod);
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);
	t_led_phase_bootstrap_start(&b, kPeriod);

	Sim sim{.latency_ns = latency};
	sim.exposure_ns = 1200000;
	sim.dark_from_ns = -options.narrow_blink_ns - latency + options.narrow_step_ns;
	sim.dark_to_ns = sim.dark_from_ns + sim.exposure_ns + options.narrow_blink_ns;
	uint32_t frame = 0;
	bool wide = false;
	for (uint32_t i = 0; i < 6000 && b.locks_acquired == 0; i++) {
		sim.run(b, 1, frame);
		wide = wide || b.state == T_LED_PHASE_BOOTSTRAP_WIDE_SCAN;
		if (t_led_phase_bootstrap_ready_to_scan(&b)) {
			t_led_phase_bootstrap_start(&b, kPeriod);
		}
	}
	CHECK_FALSE(wide);
	REQUIRE(b.locks_acquired == 1);
	CHECK(b.hint_failures == 0);
	// Locked on the lit step, within the window though not centred.
	sim.dark_from_ns = sim.dark_to_ns = 0;
	sim.run(b, 120, frame);
	CHECK(sim.lit(sim.applied_fudge, sim.applied_blink));
}

TEST_CASE("LED phase bootstrap without full-scan fallback keeps retrying the hinted scan")
{
	// 5 Oct: every always-lit fault began on entering a full scan. With a hint, retry hinted scans instead.
	const int64_t latency = 3600000;
	for (bool fallback : {true, false}) {
		CAPTURE(fallback);
		t_led_phase_bootstrap_options options = test_options();
		options.hint_retries = 2;
		options.failed_backoff_frames = 10;
		options.max_failed_backoff_frames = 40;
		options.full_scan_fallback = fallback;
		options.hint_fudge_ns = t_led_phase_bootstrap_wrap(-latency + 300000, kPeriod);
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);

		// Out of view for long enough to exhaust the retries several times over.
		Sim sim{.latency_ns = latency};
		sim.visible = false;
		uint32_t frame = 0;
		bool wide = false;
		for (uint32_t i = 0; i < 3000; i++) {
			sim.run(b, 1, frame);
			wide = wide || b.state == T_LED_PHASE_BOOTSTRAP_WIDE_SCAN;
			if (t_led_phase_bootstrap_ready_to_scan(&b)) {
				t_led_phase_bootstrap_start(&b, kPeriod);
			}
		}
		CHECK(wide == fallback);
		if (fallback) {
			continue;
		}
		CHECK(b.hint_failures > options.hint_retries);

		// Back in view: the next hinted scan locks on the exposure centre.
		sim.visible = true;
		for (uint32_t i = 0; i < 3000 && b.locks_acquired == 0; i++) {
			sim.run(b, 1, frame);
			wide = wide || b.state == T_LED_PHASE_BOOTSTRAP_WIDE_SCAN;
			if (t_led_phase_bootstrap_ready_to_scan(&b)) {
				t_led_phase_bootstrap_start(&b, kPeriod);
			}
		}
		CHECK_FALSE(wide);
		REQUIRE(b.locks_acquired == 1);
		int64_t pulse_centre = b.fudge_offset_ns + latency + b.blink_ns / 2;
		int64_t exposure_centre = sim.exposure_start_ns + sim.exposure_ns / 2;
		CHECK(circular_distance(pulse_centre, exposure_centre) <= options.narrow_step_ns);
	}
}

TEST_CASE("LED phase bootstrap measures against each camera's background")
{
	// 24 Sep slow-movement run: windows put 3-7 blobs in cameras 0 and 2 with the LEDs dark, which made those
	// cameras count as lit at every phase and widened the measured lit window to 5.4 ms.
	for (int64_t latency : {int64_t(0), int64_t(14000000)}) {
		CAPTURE(latency);
		t_led_phase_bootstrap_options options = test_options();
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);

		Sim sim{.latency_ns = latency};
		sim.background = {6, 0, 7, 0};
		uint32_t frame = 0;
		sim.run(b, 2000, frame);

		REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
		CHECK(b.baseline_blobs[0] == 7);
		CHECK(b.baseline_blobs[1] == 1);
		CHECK(b.baseline_blobs[2] == 8);
		int64_t pulse_centre = b.fudge_offset_ns + latency + b.blink_ns / 2;
		int64_t exposure_centre = sim.exposure_start_ns + sim.exposure_ns / 2;
		CHECK(circular_distance(pulse_centre, exposure_centre) <= options.narrow_step_ns);

		// Loss must still be detected: the background alone is not a lit controller.
		sim.visible = false;
		sim.run(b, options.lost_frames + 50, frame);
		CHECK(b.state != T_LED_PHASE_BOOTSTRAP_LOCKED);
	}
}

TEST_CASE("LED phase bootstrap baseline ignores a controller that is still going dark")
{
	// 24 Sep two-controller run: the left controller kept emitting for up to ~250 ms after yielding, which set
	// the right controller's baseline to 17-21 blobs so that none of its own lit frames could ever count.
	t_led_phase_bootstrap_options options = test_options();
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);
	t_led_phase_bootstrap_start(&b, kPeriod);

	Sim sim{.latency_ns = 3000000};
	uint32_t frame = 0;
	sim.background = {12, 12, 12, 1};
	sim.run(b, 16, frame); // ~270 ms of the other controller's light
	sim.background = {};
	sim.run(b, 2000, frame);

	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
	for (uint32_t c = 0; c < 4; c++) {
		CHECK(b.baseline_blobs[c] == sim.dark_blobs);
	}
}

TEST_CASE("LED phase bootstrap backs off longer after each consecutive failure")
{
	t_led_phase_bootstrap_options options = test_options();
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);

	Sim sim{.latency_ns = 0};
	sim.visible = false;
	uint32_t frame = 0;
	uint32_t expected[] = {60, 120, 240, 480, 600, 600};
	for (uint32_t expect : expected) {
		CAPTURE(expect);
		REQUIRE(t_led_phase_bootstrap_ready_to_scan(&b));
		t_led_phase_bootstrap_start(&b, kPeriod);
		while (b.state != T_LED_PHASE_BOOTSTRAP_IDLE) {
			sim.run(b, 1, frame);
		}
		CHECK(b.idle_backoff_frames == expect);
		sim.run(b, expect, frame);
	}
	CHECK(b.consecutive_failures == 6);
}

namespace {

//! Locks, then drifts the latency for @p frames and returns the fraction of those frames that were lit.
float
run_drifting_lock(t_led_phase_bootstrap &b, Sim &sim, int64_t drift_ns_per_frame, uint32_t frames)
{
	uint32_t frame = 0;
	t_led_phase_bootstrap_start(&b, kPeriod);
	sim.run(b, 1000, frame);
	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
	sim.latency_drift_ns_per_frame = drift_ns_per_frame;
	sim.frames_run = 0;
	sim.frames_lit = 0;
	sim.run(b, frames, frame);
	return (float)sim.frames_lit / (float)sim.frames_run;
}

} // namespace

TEST_CASE("LED phase bootstrap tracking follows a drifting lit window")
{
	// 24 Sep keep-lock run: the host clock mappings wandered ~1 ms in 25 s and the open-loop locks faded
	// from 81% to 45% lit. Here the latency drifts 60 us/s (1 us per frame) for 50 s, 3 ms in all.
	for (int64_t drift : {int64_t(1000), int64_t(-1000)}) {
		CAPTURE(drift);

		t_led_phase_bootstrap_options open_loop = test_options();
		t_led_phase_bootstrap b_open;
		t_led_phase_bootstrap_init(&b_open, &open_loop);
		Sim sim_open{.latency_ns = 5000000};
		float open_fraction = run_drifting_lock(b_open, sim_open, drift, 3000);

		t_led_phase_bootstrap_options tracked = test_options();
		tracked.track_interval_frames = 120;
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &tracked);
		Sim sim{.latency_ns = 5000000};
		float tracked_fraction = run_drifting_lock(b, sim, drift, 3000);

		CHECK(open_fraction < 0.5f);
		CHECK(tracked_fraction > 0.9f);
		CHECK(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
		CHECK(b.locks_acquired == 1);
		CHECK(b.track_moves > 0);
		// The lock followed the drift: 3 ms of latency means the fudge moved about -3 ms.
		CHECK(std::llabs(b.track_total_shift_ns + drift * 3000) < 700000);
	}
}

TEST_CASE("LED phase bootstrap tracking holds still without drift and ignores another controller's light")
{
	t_led_phase_bootstrap_options options = test_options();
	options.track_interval_frames = 120;
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);
	Sim sim{.latency_ns = 9000000};
	uint32_t frame = 0;
	t_led_phase_bootstrap_start(&b, kPeriod);
	sim.run(b, 1000, frame);
	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
	int64_t lock = b.lock_fudge_ns;

	// A second controller locks and stays lit after this one's baseline was measured.
	sim.background = {6, 7, 6, 0};
	sim.frames_run = 0;
	sim.frames_lit = 0;
	sim.run(b, 2000, frame);
	CHECK(b.track_cycles >= 5);
	CHECK(circular_distance(b.lock_fudge_ns, lock) <= options.narrow_step_ns);
	CHECK((float)sim.frames_lit / (float)sim.frames_run > 0.95f);

	// It still follows drift with that light present.
	sim.latency_drift_ns_per_frame = 1000;
	sim.frames_run = 0;
	sim.frames_lit = 0;
	sim.run(b, 3000, frame);
	CHECK((float)sim.frames_lit / (float)sim.frames_run > 0.85f);
}

TEST_CASE("LED phase bootstrap tolerates cameras that cannot see the controller")
{
	t_led_phase_bootstrap_options options = test_options();
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);
	t_led_phase_bootstrap_start(&b, kPeriod);

	Sim sim{.latency_ns = 5000000};
	sim.visible_cameras = 2;
	uint32_t frame = 0;
	sim.run(b, 2000, frame);

	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
}

TEST_CASE("LED phase bootstrap fails and backs off when the controller is never lit")
{
	t_led_phase_bootstrap_options options = test_options();
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);
	t_led_phase_bootstrap_start(&b, kPeriod);

	Sim sim{.latency_ns = 0};
	sim.visible = false;
	uint32_t frame = 0;
	// One dark baseline step (36 exposures), then 17 wide steps of 20 exposures each.
	sim.run(b, 386, frame);

	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_IDLE);
	REQUIRE_FALSE(b.have_lock);
	REQUIRE_FALSE(t_led_phase_bootstrap_ready_to_scan(&b));

	sim.run(b, 60, frame);
	REQUIRE(t_led_phase_bootstrap_ready_to_scan(&b));
}

TEST_CASE("LED phase bootstrap rejects a flat scan caused by a constantly lit background")
{
	t_led_phase_bootstrap_options options = test_options();
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);
	t_led_phase_bootstrap_start(&b, kPeriod);

	// Another emitter lights every frame regardless of phase, as a second locked controller would.
	Sim sim{.latency_ns = 0};
	sim.visible = false;
	sim.dark_blobs = 6;
	uint32_t frame = 0;
	sim.run(b, 400, frame);

	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_IDLE);
	REQUIRE_FALSE(b.have_lock);
}

TEST_CASE("LED phase bootstrap rescans after losing the controller")
{
	t_led_phase_bootstrap_options options = test_options();
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);
	t_led_phase_bootstrap_start(&b, kPeriod);

	Sim sim{.latency_ns = 2000000};
	uint32_t frame = 0;
	sim.run(b, 2000, frame);
	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);

	sim.visible = false;
	sim.run(b, options.lost_frames + 10, frame);
	CHECK(t_led_phase_bootstrap_is_scanning(&b));
	CHECK(b.scans_attempted == 2);

	// Once visible again, and the latency has drifted, it relocks at the new phase.
	sim.visible = true;
	sim.latency_ns = 2600000;
	sim.run(b, 2000, frame);
	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
	int64_t pulse_centre = b.fudge_offset_ns + sim.latency_ns + b.blink_ns / 2;
	CHECK(circular_distance(pulse_centre, sim.exposure_ns / 2) <= options.narrow_step_ns);
}

TEST_CASE("LED phase bootstrap with a minimum lock peak rejects a controller only one camera sees")
{
	// A lock from one camera's worth of lit frames sits wherever that camera's view happened to be lit.
	t_led_phase_bootstrap_options loose = test_options();
	t_led_phase_bootstrap b_loose;
	t_led_phase_bootstrap_init(&b_loose, &loose);
	t_led_phase_bootstrap_start(&b_loose, kPeriod);
	Sim sim_loose{.latency_ns = 5000000};
	sim_loose.visible_cameras = 1;
	uint32_t frame = 0;
	sim_loose.run(b_loose, 2000, frame);
	CHECK(b_loose.have_lock);

	t_led_phase_bootstrap_options strict = test_options();
	strict.min_lock_peak_score = 2.0f;
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &strict);
	t_led_phase_bootstrap_start(&b, kPeriod);
	Sim sim{.latency_ns = 5000000};
	sim.visible_cameras = 1;
	frame = 0;
	sim.run(b, 2000, frame);
	CHECK_FALSE(b.have_lock);
	CHECK(b.consecutive_failures >= 1);

	// Two cameras are enough.
	t_led_phase_bootstrap b_two;
	t_led_phase_bootstrap_init(&b_two, &strict);
	t_led_phase_bootstrap_start(&b_two, kPeriod);
	Sim sim_two{.latency_ns = 5000000};
	sim_two.visible_cameras = 2;
	frame = 0;
	sim_two.run(b_two, 2000, frame);
	CHECK(b_two.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
}

TEST_CASE("LED phase bootstrap tracking holds its lock while background light changes")
{
	// Background changing by 0-4 blobs every 30 frames (a hand, another ring passing through view) makes single
	// probes disagree; the lock must stay in the lit window. (Requiring two agreeing probes before moving was tried
	// and was worse: 83% lit here against 97%, and it could not follow 60 us/s drift.)
	t_led_phase_bootstrap_options options = test_options();
	options.track_interval_frames = 120;
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);
	Sim sim{.latency_ns = 9000000};
	uint32_t frame = 0;
	t_led_phase_bootstrap_start(&b, kPeriod);
	sim.run(b, 1000, frame);
	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
	sim.noise_blobs = 4;
	sim.frames_run = 0;
	sim.frames_lit = 0;
	sim.run(b, 4000, frame);
	CHECK(b.track_cycles >= 10);
	CHECK((float)sim.frames_lit / (float)sim.frames_run > 0.95f);
}

TEST_CASE("LED phase bootstrap coverage probes follow drift and ignore background and lost solves")
{
	for (int64_t drift : {int64_t(1000), int64_t(-1000)}) {
		CAPTURE(drift);
		t_led_phase_bootstrap_options options = test_options();
		options.track_interval_frames = 120;
		options.track_use_pose_coverage = true;
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		Sim sim{.latency_ns = 5000000};
		sim.push_coverage = true;
		float fraction = run_drifting_lock(b, sim, drift, 3000);
		CHECK(fraction > 0.9f);
		CHECK(b.track_moves > 0);
	}

	// No drift, background changing by 0-8 blobs and one block in four losing every solve: the lock stays put.
	t_led_phase_bootstrap_options options = test_options();
	options.track_interval_frames = 120;
	options.track_use_pose_coverage = true;
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);
	Sim sim{.latency_ns = 9000000};
	sim.push_coverage = true;
	uint32_t frame = 0;
	t_led_phase_bootstrap_start(&b, kPeriod);
	sim.run(b, 1000, frame);
	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
	int64_t lock = b.lock_fudge_ns;
	sim.noise_blobs = 8;
	sim.solve_dropout_one_in = 4;
	sim.frames_run = 0;
	sim.frames_lit = 0;
	sim.run(b, 4000, frame);
	CHECK(b.track_cycles >= 10);
	CHECK((float)sim.frames_lit / (float)sim.frames_run > 0.95f);
	CHECK(circular_distance(b.lock_fudge_ns, lock) <= 400000);
}

TEST_CASE("LED phase bootstrap coverage probes ignore solve dropouts that the blob counts do not confirm")
{
	// 25 Sep (234101): whole probe stages lost every solve while the ring stayed lit (a hand, fast motion), read
	// coverage 0, and moved the left's lock 400 us at a time. With one block in three dropping out and no drift,
	// the lock must stay put.
	for (bool confirm : {false, true}) {
		CAPTURE(confirm);
		t_led_phase_bootstrap_options options = test_options();
		options.track_interval_frames = 120;
		options.track_use_pose_coverage = true;
		options.track_coverage_min_blob_imbalance = confirm ? 0.25f : 0.0f;
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		Sim sim{.latency_ns = 9000000};
		sim.push_coverage = true;
		uint32_t frame = 0;
		t_led_phase_bootstrap_start(&b, kPeriod);
		sim.run(b, 1000, frame);
		REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
		sim.solve_dropout_one_in = 3;
		sim.run(b, 6000, frame);
		CHECK(b.track_cycles >= 15);
		if (confirm) {
			CHECK(b.track_moves == 0);
		} else {
			CHECK(b.track_moves > 0);
		}
	}
}

TEST_CASE("LED phase bootstrap coverage probes fall back to blob counts when the ring has slid out of view")
{
	// The lit window jumps 0.9 ms (the left slid ~0.9 ms on 25 Sep): the lock is now dark, nothing solves, and only
	// the blob counts can show which way to move.
	for (bool fallback : {false, true}) {
		CAPTURE(fallback);
		t_led_phase_bootstrap_options options = test_options();
		options.track_interval_frames = 120;
		options.track_use_pose_coverage = true;
		options.track_blob_fallback = fallback;
		options.lost_frames = 100000; // keep the lock, so only tracking can recover
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		Sim sim{.latency_ns = 5000000};
		sim.push_coverage = true;
		uint32_t frame = 0;
		t_led_phase_bootstrap_start(&b, kPeriod);
		sim.run(b, 1000, frame);
		REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
		sim.latency_ns -= 900000; // pulses land 0.9 ms earlier; the simulated ring is lit or dark, never dim
		sim.frames_run = 0;
		sim.frames_lit = 0;
		sim.run(b, 3000, frame);
		float lit = (float)sim.frames_lit / (float)sim.frames_run;
		if (fallback) {
			CHECK(lit > 0.7f);
			CHECK(b.track_moves > 0);
		} else {
			CHECK(lit < 0.3f);
		}
	}
}

TEST_CASE("LED phase bootstrap blob fallback does not steer a ring that is out of view")
{
	// The ring leaves view: every probe stage sees only a stray blob or two. The lock must not wander.
	t_led_phase_bootstrap_options options = test_options();
	options.track_interval_frames = 120;
	options.track_use_pose_coverage = true;
	options.track_blob_fallback = true;
	options.lost_frames = 100000;
	t_led_phase_bootstrap b;
	t_led_phase_bootstrap_init(&b, &options);
	Sim sim{.latency_ns = 5000000};
	sim.push_coverage = true;
	uint32_t frame = 0;
	t_led_phase_bootstrap_start(&b, kPeriod);
	sim.run(b, 1000, frame);
	REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
	int64_t lock = b.lock_fudge_ns;
	sim.visible = false;
	sim.noise_blobs = 1; // a stray blob that comes and goes (the real out-of-view stages read 0.1-2 of a 4.9 ring)
	sim.run(b, 4000, frame);
	CHECK(b.track_cycles >= 10);
	CHECK(b.track_moves == 0);
	CHECK(b.lock_fudge_ns == lock);
}

TEST_CASE("LED phase bootstrap detects a controller stuck lit and stops scanning")
{
	t_led_phase_bootstrap_options options = test_options();
	options.detect_stuck_lit = true;

	// Stuck from the start: its own ring is solved during the dark baseline.
	{
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);
		Sim sim{.latency_ns = 5000000};
		sim.push_own = true;
		sim.stuck_from_frame = 0;
		uint32_t frame = 0;
		sim.run(b, 2000, frame);
		CHECK(t_led_phase_bootstrap_is_stuck_lit(&b));
		CHECK_FALSE(b.have_lock);
		CHECK(b.scans_attempted == 1);
		CHECK_FALSE(t_led_phase_bootstrap_is_scanning(&b));
		CHECK_FALSE(t_led_phase_bootstrap_ready_to_scan(&b));
		t_led_phase_bootstrap_start(&b, kPeriod); // ignored once stuck
		CHECK(t_led_phase_bootstrap_is_stuck_lit(&b));
	}

	// Sticks during the wide scan (as on 24 Sep at wide step 3): caught when the wide scan ends.
	{
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);
		Sim sim{.latency_ns = 5000000};
		sim.push_own = true;
		sim.stuck_from_frame = 36 + 2 * 20;
		uint32_t frame = 0;
		sim.run(b, 2000, frame);
		CHECK(t_led_phase_bootstrap_is_stuck_lit(&b));
		CHECK_FALSE(b.have_lock);
	}

	// Sticks at the start of the narrow scan (as in the 25 Sep rotation sweep): caught when the narrow scan ends.
	{
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);
		Sim sim{.latency_ns = 5000000};
		sim.push_own = true;
		sim.stuck_from_frame = 36 + 17 * 20 + 5;
		uint32_t frame = 0;
		sim.run(b, 2000, frame);
		CHECK(t_led_phase_bootstrap_is_stuck_lit(&b));
	}

	// A healthy controller seen by every camera, and one seen by two, still lock.
	for (uint32_t cameras : {4u, 2u}) {
		CAPTURE(cameras);
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);
		Sim sim{.latency_ns = 5000000};
		sim.push_own = true;
		sim.visible_cameras = cameras;
		uint32_t frame = 0;
		sim.run(b, 2000, frame);
		CHECK(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
		CHECK(b.stuck_detections == 0);
	}
}

TEST_CASE("LED phase bootstrap hinted scan locks with far fewer setting changes, and falls back when the hint is stale")
{
	// Find the true lock first, as a previous run would have.
	t_led_phase_bootstrap_options full = test_options();
	t_led_phase_bootstrap reference;
	t_led_phase_bootstrap_init(&reference, &full);
	t_led_phase_bootstrap_start(&reference, kPeriod);
	Sim ref_sim{.latency_ns = 5000000};
	uint32_t frame = 0;
	ref_sim.run(reference, 2000, frame);
	REQUIRE(reference.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
	const int64_t true_hint = reference.lock_fudge_ns + full.lock_blink_ns / 2 - full.narrow_blink_ns / 2;

	for (int64_t error_ns : {int64_t(0), int64_t(600000), int64_t(-900000)}) {
		CAPTURE(error_ns);
		t_led_phase_bootstrap_options options = test_options();
		options.hint_fudge_ns = t_led_phase_bootstrap_wrap(true_hint + error_ns, kPeriod);
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);
		Sim sim{.latency_ns = 5000000};
		frame = 0;
		// Without tracking, every output change is a scan step, the start or the lock.
		uint32_t first_generation = b.output_generation;
		sim.run(b, 1000, frame);
		uint32_t changes = b.output_generation - first_generation;
		REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
		CHECK(circular_distance(b.lock_fudge_ns, reference.lock_fudge_ns) <= full.narrow_step_ns);
		// The full scan makes ~38 changes; the hinted one ~13.
		CHECK(changes <= 16);
	}

	// A hint 5 ms off finds nothing and falls back to the full scan, which still locks; with a retry, after a
	// second short scan.
	for (uint32_t retries : {0u, 1u, 4u}) {
		CAPTURE(retries);
		t_led_phase_bootstrap_options options = test_options();
		options.hint_retries = retries;
		options.hint_fudge_ns = t_led_phase_bootstrap_wrap(true_hint + 5000000, kPeriod);
		t_led_phase_bootstrap b;
		t_led_phase_bootstrap_init(&b, &options);
		t_led_phase_bootstrap_start(&b, kPeriod);
		Sim sim{.latency_ns = 5000000};
		frame = 0;
		// Restart whenever the bootstrap is idle and ready, as the driver does.
		for (int chunk = 0; chunk < 60 && b.state != T_LED_PHASE_BOOTSTRAP_LOCKED; chunk++) {
			if (t_led_phase_bootstrap_ready_to_scan(&b)) {
				t_led_phase_bootstrap_start(&b, kPeriod);
			}
			sim.run(b, 100, frame);
		}
		REQUIRE(b.state == T_LED_PHASE_BOOTSTRAP_LOCKED);
		CHECK(circular_distance(b.lock_fudge_ns, reference.lock_fudge_ns) <= full.narrow_step_ns);
		CHECK(b.consecutive_failures == 0);
		CHECK(b.scans_attempted == 1 + retries);
	}
}

TEST_CASE("LED phase bootstrap wraps offsets into the period")
{
	CHECK(t_led_phase_bootstrap_wrap(-1, kPeriod) == kPeriod - 1);
	CHECK(t_led_phase_bootstrap_wrap(kPeriod, kPeriod) == 0);
	CHECK(t_led_phase_bootstrap_wrap(kPeriod + 5, kPeriod) == 5);
}

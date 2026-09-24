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
	uint32_t frames_run = 0;
	uint32_t frames_lit = 0;

	std::deque<std::pair<int64_t, int64_t>> pending_outputs{}; // (fudge, blink) queued for delayed application
	int64_t applied_fudge = 0;
	int64_t applied_blink = 0;

	bool
	lit(int64_t fudge, int64_t blink) const
	{
		if (!visible || blink <= 0) {
			return false;
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
		std::deque<std::pair<int64_t, bool>> reports; // (timestamp, lit) awaiting delayed report
		for (uint32_t i = 0; i < frames; i++, frame_index++) {
			int64_t ts = (int64_t)frame_index * kPeriod;

			// Settings take a few frames to reach the controller.
			pending_outputs.emplace_back(b.fudge_offset_ns, t_led_phase_bootstrap_leds_enabled(&b) ? b.blink_ns : 0);
			if (pending_outputs.size() > apply_delay_frames) {
				applied_fudge = pending_outputs.front().first;
				applied_blink = pending_outputs.front().second;
				pending_outputs.pop_front();
			}

			bool frame_lit = lit(applied_fudge, applied_blink);
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
				for (uint32_t c = 0; c < 4; c++) {
					bool cam_lit = rlit && c < visible_cameras;
					t_led_phase_bootstrap_push_blob_count(&b, c, rts,
					                                      background[c] + (cam_lit ? lit_blobs : dark_blobs));
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
	for (int64_t latency : {int64_t(0), int64_t(3600000), int64_t(-1100000), int64_t(14000000), int64_t(9000000)}) {
		CAPTURE(latency);
		t_led_phase_bootstrap_options options = test_options();
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

TEST_CASE("LED phase bootstrap wraps offsets into the period")
{
	CHECK(t_led_phase_bootstrap_wrap(-1, kPeriod) == kPeriod - 1);
	CHECK(t_led_phase_bootstrap_wrap(kPeriod, kPeriod) == 0);
	CHECK(t_led_phase_bootstrap_wrap(kPeriod + 5, kPeriod) == 5);
}

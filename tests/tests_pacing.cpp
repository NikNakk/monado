// Copyright 2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Frame pacing tests.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 */

#include <util/u_pacing.h>

#include "catch_amalgamated.hpp"

#include "time_utils.hpp"

#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <queue>
#include <cstdlib>

using namespace std::chrono_literals;
using namespace std::chrono;

static constexpr unanoseconds frame_interval_ns(16ms);

namespace {

int64_t
getNextPresentAfterTimestampAndKnownPresent(int64_t timestamp_ns, int64_t known_present_ns)
{

	while (known_present_ns < timestamp_ns) {
		known_present_ns += frame_interval_ns.count();
	}
	return known_present_ns;
}
int64_t
getPresentBefore(int64_t timestamp_ns, int64_t known_present_ns)
{

	while (known_present_ns >= timestamp_ns && known_present_ns > frame_interval_ns.count()) {
		known_present_ns -= frame_interval_ns.count();
	}
	return known_present_ns;
}
int64_t
getNextPresentAfterTimestamp(int64_t timestamp_ns, int64_t known_present_ns)
{
	auto present_before_ns = getPresentBefore(timestamp_ns, known_present_ns);
	return getNextPresentAfterTimestampAndKnownPresent(timestamp_ns, present_before_ns);
}

struct CompositorPredictions
{
	int64_t frame_id{0};
	int64_t wake_up_time_ns{0};
	int64_t desired_present_time_ns{0};
	int64_t present_slop_ns{0};
	int64_t predicted_display_time_ns{0};
	int64_t predicted_display_period_ns{0};
	int64_t min_display_period_ns{0};
};
} // namespace

static void
basicPredictionConsistencyChecks(int64_t now_ns, CompositorPredictions const &predictions)
{
	INFO(predictions.frame_id);
	INFO(now_ns);
	CHECK(predictions.wake_up_time_ns >= now_ns);
	CHECK(predictions.desired_present_time_ns > now_ns);
	CHECK(predictions.desired_present_time_ns > predictions.wake_up_time_ns);
	CHECK(predictions.predicted_display_time_ns > now_ns);
	CHECK(predictions.predicted_display_time_ns > predictions.desired_present_time_ns);
	// display period predicted to be +- 2ms (arbitrary)
	CHECK(unanoseconds(predictions.predicted_display_period_ns) < (frame_interval_ns + unanoseconds(2ms)));
	CHECK(unanoseconds(predictions.predicted_display_period_ns) > (frame_interval_ns - unanoseconds(2ms)));
}

struct SimulatedDisplayTimingData
{
	SimulatedDisplayTimingData(int64_t id, int64_t desired_present_time, int64_t gpu_finish, int64_t now)
	    : frame_id(id), desired_present_time_ns(desired_present_time),
	      actual_present_time_ns(getNextPresentAfterTimestampAndKnownPresent(gpu_finish, desired_present_time)),
	      earliest_present_time_ns(getNextPresentAfterTimestamp(gpu_finish, desired_present_time)),
	      present_margin_ns(earliest_present_time_ns - gpu_finish), now_ns(now)
	{}

	int64_t frame_id;
	int64_t desired_present_time_ns;
	int64_t actual_present_time_ns;
	int64_t earliest_present_time_ns;
	int64_t present_margin_ns;
	int64_t now_ns;
	void
	call_u_pc_info(u_pacing_compositor *upc) const
	{
		std::cout << "frame_id:                 " << frame_id << '\n';
		std::cout << "desired_present_time_ns:  " << desired_present_time_ns << '\n';
		std::cout << "actual_present_time_ns:   " << actual_present_time_ns << '\n';
		std::cout << "earliest_present_time_ns: " << earliest_present_time_ns << '\n';
		std::cout << "present_margin_ns:        " << present_margin_ns << '\n';
		std::cout << "now_ns:                   " << now_ns << "\n" << '\n';
		u_pc_info(upc, frame_id, desired_present_time_ns, actual_present_time_ns, earliest_present_time_ns,
		          present_margin_ns, now_ns);
	}
};
static inline bool
operator>(SimulatedDisplayTimingData const &lhs, SimulatedDisplayTimingData const &rhs)
{
	return lhs.now_ns > rhs.now_ns;
}
using SimulatedDisplayTimingQueue = std::priority_queue<SimulatedDisplayTimingData,
                                                        std::vector<SimulatedDisplayTimingData>,
                                                        std::greater<SimulatedDisplayTimingData>>;

//! Process all simulated timing data in the queue that should be processed by now.
static void
processDisplayTimingQueue(SimulatedDisplayTimingQueue &display_timing_queue, int64_t now_ns, u_pacing_compositor *upc)
{
	while (!display_timing_queue.empty() && display_timing_queue.top().now_ns <= now_ns) {
		display_timing_queue.top().call_u_pc_info(upc);
		display_timing_queue.pop();
	}
}
//! Process all remaining simulated timing data in the queue and return the timestamp of the last one.
static int64_t
drainDisplayTimingQueue(SimulatedDisplayTimingQueue &display_timing_queue, int64_t now_ns, u_pacing_compositor *upc)
{
	while (!display_timing_queue.empty()) {
		now_ns = display_timing_queue.top().now_ns;
		display_timing_queue.top().call_u_pc_info(upc);
		display_timing_queue.pop();
	}
	return now_ns;
}

static void
doFrame(SimulatedDisplayTimingQueue &display_timing_queue,
        u_pacing_compositor *upc,
        MockClock &clock,
        int64_t wake_time_ns,
        int64_t desired_present_time_ns,
        int64_t frame_id,
        unanoseconds wake_delay,
        unanoseconds begin_delay,
        unanoseconds draw_delay,
        unanoseconds submit_delay,
        unanoseconds gpu_time_after_submit)
{
	REQUIRE(clock.now() <= wake_time_ns);
	// wake up (after delay)
	clock.advance_to(wake_time_ns);
	clock.advance(wake_delay);
	processDisplayTimingQueue(display_timing_queue, clock.now(), upc);
	u_pc_mark_point(upc, U_TIMING_POINT_WAKE_UP, frame_id, clock.now());

	// begin (after delay)
	clock.advance(begin_delay);
	processDisplayTimingQueue(display_timing_queue, clock.now(), upc);
	u_pc_mark_point(upc, U_TIMING_POINT_BEGIN, frame_id, clock.now());

	// spend cpu time drawing
	clock.advance(draw_delay);
	u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, clock.now());

	// spend cpu time before submit
	clock.advance(submit_delay);
	processDisplayTimingQueue(display_timing_queue, clock.now(), upc);
	u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_END, frame_id, clock.now());

	// spend gpu time before present
	clock.advance(gpu_time_after_submit);
	auto gpu_finish = clock.now();
	auto next_scanout_timepoint = getNextPresentAfterTimestampAndKnownPresent(gpu_finish, desired_present_time_ns);

	REQUIRE(next_scanout_timepoint >= gpu_finish);

	// our wisdom arrives after scanout
	MockClock infoClock;
	infoClock.advance_to(next_scanout_timepoint);
	infoClock.advance(1ms);
	display_timing_queue.push({frame_id, desired_present_time_ns, gpu_finish, infoClock.now()});
}

// u_pc is for the compositor, we should take way less than a frame to do our job.
static constexpr auto wakeDelay = microseconds(20);

static constexpr auto shortBeginDelay = microseconds(20);
static constexpr auto shortDrawDelay = 150us;
static constexpr auto shortSubmitDelay = 50us;
static constexpr auto shortGpuTime = 1ms;


static constexpr auto longBeginDelay = 1ms;
static constexpr auto longDrawDelay = 2ms;
static constexpr auto longGpuTime = 2ms;

TEST_CASE("u_pacing_compositor_display_timing")
{
	struct u_pc_display_timing_config config = u_pc_display_timing_get_default_config();
	u_pacing_compositor *upc = nullptr;
	MockClock clock;
	REQUIRE(XRT_SUCCESS == u_pc_display_timing_create(frame_interval_ns.count(), &config, &upc));
	REQUIRE(upc != nullptr);

	clock.advance(1ms);

	CompositorPredictions predictions;
	u_pc_predict(upc, clock.now(), &predictions.frame_id, &predictions.wake_up_time_ns,
	             &predictions.desired_present_time_ns, &predictions.present_slop_ns,
	             &predictions.predicted_display_time_ns, &predictions.predicted_display_period_ns,
	             &predictions.min_display_period_ns);
	basicPredictionConsistencyChecks(clock.now(), predictions);

	auto frame_id = predictions.frame_id;
	SimulatedDisplayTimingQueue queue;


	SECTION("faster than expected")
	{
		// We have a 16ms period
		// wake promptly
		clock.advance(wakeDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_WAKE_UP, frame_id, clock.now());

		// start promptly
		clock.advance(shortBeginDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_BEGIN, frame_id, clock.now());

		// spend cpu time drawing
		clock.advance(shortDrawDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, clock.now());

		// spend a little cpu time submitting the work to the GPU
		clock.advance(shortSubmitDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_END, frame_id, clock.now());

		// spend time in gpu rendering until present
		clock.advance(shortGpuTime);
		auto gpu_finish = clock.now();

		auto next_scanout_timepoint =
		    getNextPresentAfterTimestampAndKnownPresent(gpu_finish, predictions.desired_present_time_ns);

		// our wisdom arrives after scanout
		MockClock infoClock;
		infoClock.advance_to(next_scanout_timepoint);
		infoClock.advance(1ms);
		queue.push({frame_id, predictions.desired_present_time_ns, gpu_finish, infoClock.now()});

		// Do basically the same thing a few more frames.
		for (int i = 0; i < 20; ++i) {
			CompositorPredictions loopPred;
			u_pc_predict(upc, clock.now(), &loopPred.frame_id, &loopPred.wake_up_time_ns,
			             &loopPred.desired_present_time_ns, &loopPred.present_slop_ns,
			             &loopPred.predicted_display_time_ns, &loopPred.predicted_display_period_ns,
			             &loopPred.min_display_period_ns);
			CHECK(loopPred.frame_id > i);
			INFO("frame id" << loopPred.frame_id);
			INFO(clock.now());
			basicPredictionConsistencyChecks(clock.now(), loopPred);
			doFrame(queue, upc, clock, loopPred.wake_up_time_ns, loopPred.desired_present_time_ns,
			        loopPred.frame_id, wakeDelay, shortBeginDelay, shortDrawDelay, shortSubmitDelay,
			        shortGpuTime);
		}
		// we should now get a shorter time before present to wake up.
		CompositorPredictions newPred;
		u_pc_predict(upc, clock.now(), &newPred.frame_id, &newPred.wake_up_time_ns,
		             &newPred.desired_present_time_ns, &newPred.present_slop_ns,
		             &newPred.predicted_display_time_ns, &newPred.predicted_display_period_ns,
		             &newPred.min_display_period_ns);
		basicPredictionConsistencyChecks(clock.now(), newPred);
		CHECK(unanoseconds(newPred.desired_present_time_ns - newPred.wake_up_time_ns) <
		      unanoseconds(predictions.desired_present_time_ns - predictions.wake_up_time_ns));
		CHECK(unanoseconds(newPred.desired_present_time_ns - newPred.wake_up_time_ns) >
		      unanoseconds(shortDrawDelay + shortSubmitDelay + shortGpuTime));
	}

	SECTION("slower than desired")
	{
		// We have a 16ms period
		// wake promptly
		clock.advance(wakeDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_WAKE_UP, frame_id, clock.now());

		// waste time before beginframe
		clock.advance(longBeginDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_BEGIN, frame_id, clock.now());

		// spend cpu time drawing
		clock.advance(longDrawDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_BEGIN, frame_id, clock.now());

		// spend a little cpu time submitting the work to the GPU
		clock.advance(shortSubmitDelay);
		u_pc_mark_point(upc, U_TIMING_POINT_SUBMIT_END, frame_id, clock.now());

		// spend time in gpu rendering until present
		clock.advance(longGpuTime);
		auto gpu_finish = clock.now();

		auto next_scanout_timepoint =
		    getNextPresentAfterTimestampAndKnownPresent(gpu_finish, predictions.desired_present_time_ns);

		REQUIRE(next_scanout_timepoint > gpu_finish);


		// our wisdom arrives after scanout
		MockClock infoClock;
		infoClock.advance_to(next_scanout_timepoint);
		infoClock.advance(1ms);
		queue.push({frame_id, predictions.desired_present_time_ns, gpu_finish, infoClock.now()});

		// Do basically the same thing a few more frames.
		for (int i = 0; i < 50; ++i) {
			CompositorPredictions loopPred;
			u_pc_predict(upc, clock.now(), &loopPred.frame_id, &loopPred.wake_up_time_ns,
			             &loopPred.desired_present_time_ns, &loopPred.present_slop_ns,
			             &loopPred.predicted_display_time_ns, &loopPred.predicted_display_period_ns,
			             &loopPred.min_display_period_ns);
			INFO(loopPred.frame_id);
			INFO(clock.now());
			basicPredictionConsistencyChecks(clock.now(), loopPred);
			doFrame(queue, upc, clock, loopPred.wake_up_time_ns, loopPred.desired_present_time_ns,
			        loopPred.frame_id, wakeDelay, longBeginDelay, longDrawDelay, shortSubmitDelay,
			        longGpuTime);
		}

		// we should now get a bigger time before present to wake up.
		CompositorPredictions newPred;
		u_pc_predict(upc, clock.now(), &newPred.frame_id, &newPred.wake_up_time_ns,
		             &newPred.desired_present_time_ns, &newPred.present_slop_ns,
		             &newPred.predicted_display_time_ns, &newPred.predicted_display_period_ns,
		             &newPred.min_display_period_ns);
		basicPredictionConsistencyChecks(clock.now(), newPred);
		CHECK(unanoseconds(newPred.desired_present_time_ns - newPred.wake_up_time_ns) >
		      unanoseconds(longBeginDelay + longDrawDelay + shortSubmitDelay + longGpuTime));
	}

	u_pc_destroy(&upc);
}

TEST_CASE("u_pacing_compositor_fake")
{
	MockClock clock;
	u_pacing_compositor *upc = nullptr;
	REQUIRE(XRT_SUCCESS == u_pc_fake_create(frame_interval_ns.count(), clock.now(), &upc));
	REQUIRE(upc != nullptr);

	clock.advance(1ms);

	SECTION("Standalone predictions")
	{
		CompositorPredictions predictions;
		u_pc_predict(upc, clock.now(), &predictions.frame_id, &predictions.wake_up_time_ns,
		             &predictions.desired_present_time_ns, &predictions.present_slop_ns,
		             &predictions.predicted_display_time_ns, &predictions.predicted_display_period_ns,
		             &predictions.min_display_period_ns);
		basicPredictionConsistencyChecks(clock.now(), predictions);
	}
	SECTION("Consistency in loop")
	{
		SimulatedDisplayTimingQueue queue;
		SECTION("Fast")
		{

			for (int i = 0; i < 10; ++i) {
				CompositorPredictions predictions;
				u_pc_predict(upc, clock.now(), &predictions.frame_id, &predictions.wake_up_time_ns,
				             &predictions.desired_present_time_ns, &predictions.present_slop_ns,
				             &predictions.predicted_display_time_ns,
				             &predictions.predicted_display_period_ns,
				             &predictions.min_display_period_ns);
				INFO(predictions.frame_id);
				INFO(clock.now());
				basicPredictionConsistencyChecks(clock.now(), predictions);
				doFrame(queue, upc, clock, predictions.wake_up_time_ns,
				        predictions.desired_present_time_ns, predictions.frame_id, wakeDelay,
				        shortBeginDelay, shortDrawDelay, shortSubmitDelay, shortGpuTime);
			}
			drainDisplayTimingQueue(queue, clock.now(), upc);
		}
		SECTION("Slow")
		{
			for (int i = 0; i < 10; ++i) {
				CompositorPredictions predictions;
				u_pc_predict(upc, clock.now(), &predictions.frame_id, &predictions.wake_up_time_ns,
				             &predictions.desired_present_time_ns, &predictions.present_slop_ns,
				             &predictions.predicted_display_time_ns,
				             &predictions.predicted_display_period_ns,
				             &predictions.min_display_period_ns);
				INFO(predictions.frame_id);
				INFO(clock.now());
				basicPredictionConsistencyChecks(clock.now(), predictions);
				doFrame(queue, upc, clock, predictions.wake_up_time_ns,
				        predictions.desired_present_time_ns, predictions.frame_id, wakeDelay,
				        longBeginDelay, longDrawDelay, shortSubmitDelay, longGpuTime);
			}
			drainDisplayTimingQueue(queue, clock.now(), upc);
		}
	}
	u_pc_destroy(&upc);
}

TEST_CASE("Fake pacer refines refresh period without resetting outstanding frame IDs")
{
	struct u_pacing_compositor *upc = nullptr;
	const int64_t now_ns = 1000000000;
	REQUIRE(u_pc_fake_create(8333333, now_ns, &upc) == XRT_SUCCESS);
	CompositorPredictions before, after;
	auto predict = [&](CompositorPredictions &p) {
		u_pc_predict(upc, now_ns, &p.frame_id, &p.wake_up_time_ns, &p.desired_present_time_ns,
		             &p.present_slop_ns, &p.predicted_display_time_ns, &p.predicted_display_period_ns,
		             &p.min_display_period_ns);
	};
	predict(before);
	u_pc_update_present_offset(upc, before.frame_id, 25000000);
	u_pc_update_vblank_from_display_control(upc, now_ns);
	REQUIRE(u_pc_update_frame_period(upc, 8341708));
	predict(after);
	CHECK(after.frame_id == before.frame_id + 1);
	CHECK(after.predicted_display_period_ns == 8341708);
	CHECK(after.min_display_period_ns == 8341708);
	CHECK(after.predicted_display_time_ns - after.desired_present_time_ns == 25000000);
	CHECK((after.desired_present_time_ns - now_ns) % 8341708 == 0);
	// Feedback for the frame predicted before refinement must still be accepted.
	u_pc_mark_point(upc, U_TIMING_POINT_WAKE_UP, before.frame_id, before.wake_up_time_ns);
	u_pc_destroy(&upc);
}

TEST_CASE("Display-timing pacer changes refresh period without resetting frame IDs")
{
	struct u_pc_display_timing_config config = u_pc_display_timing_get_default_config();
	struct u_pacing_compositor *upc = nullptr;
	MockClock clock;
	REQUIRE(u_pc_display_timing_create(11111111, &config, &upc) == XRT_SUCCESS);
	clock.advance(1ms);
	CompositorPredictions before, after;
	auto predict = [&](CompositorPredictions &p) {
		u_pc_predict(upc, clock.now(), &p.frame_id, &p.wake_up_time_ns, &p.desired_present_time_ns,
		             &p.present_slop_ns, &p.predicted_display_time_ns, &p.predicted_display_period_ns,
		             &p.min_display_period_ns);
	};
	predict(before);
	CHECK(before.predicted_display_period_ns == 11111111);
	REQUIRE(u_pc_update_frame_period(upc, 8333333));
	predict(after);
	CHECK(after.frame_id == before.frame_id + 1);
	CHECK(after.predicted_display_period_ns == 8333333);
	CHECK_FALSE(u_pc_update_frame_period(upc, 0));
	u_pc_destroy(&upc);
}

TEST_CASE("Fake pacing exposes the acquire placement deadline tradeoff", "[pacing][acquire-phase]")
{
	constexpr int64_t period = 11122208;
	constexpr int64_t previous_present = 1000000000;
	u_pacing_compositor *pc = nullptr;
	REQUIRE(u_pc_fake_create(period, previous_present, &pc) == XRT_SUCCESS);
	u_pc_update_vblank_from_display_control(pc, previous_present);
	CompositorPredictions early, late;
	auto predict = [&](int64_t now, CompositorPredictions &p) {
		u_pc_predict(pc, now, &p.frame_id, &p.wake_up_time_ns, &p.desired_present_time_ns,
		             &p.present_slop_ns, &p.predicted_display_time_ns, &p.predicted_display_period_ns,
		             &p.min_display_period_ns);
	};
	predict(previous_present + 1000000, early);
	REQUIRE(early.desired_present_time_ns == previous_present + period);
	const int64_t budget = early.desired_present_time_ns - early.wake_up_time_ns;
	REQUIRE(budget > 1000000);
	// Image readiness can miss the conservative wake budget but still allow rendering.
	const int64_t ready = early.wake_up_time_ns + budget / 2;
	predict(ready, late);
	CHECK(late.desired_present_time_ns == early.desired_present_time_ns + period);
	CHECK(late.frame_id > early.frame_id);
	const int64_t short_render = budget / 4;
	CHECK(ready + short_render < early.desired_present_time_ns);
	// Keeping a target cannot rescue work whose image arrives after its deadline.
	const int64_t too_late = early.desired_present_time_ns + 1000000;
	CHECK(too_late + short_render > early.desired_present_time_ns);
	u_pc_destroy(&pc);
}

TEST_CASE("Refresh period updates preserve budget, offset and phase")
{
	for (bool display_timing : {false, true}) {
		struct u_pacing_compositor *pc = nullptr;
		struct u_pc_display_timing_config config = u_pc_display_timing_get_default_config();
		config.comp_time_fraction = 22;
		config.comp_time_max_fraction = 65;
		config.present_to_display_offset_ns = 12000000;
		config.margin_ns = 2300000;
		REQUIRE((display_timing ? u_pc_display_timing_create(11111111, &config, &pc)
		                        : u_pc_fake_create(11111111, 1000000000, &pc)) == XRT_SUCCESS);
		int64_t id, wake, desired, slop, display, period, minimum;
		u_pc_predict(pc, 1000000000, &id, &wake, &desired, &slop, &display, &period, &minimum);
		CHECK(period == 11111111);
		int64_t first_id = id;
		int64_t original_budget = desired - wake;
		int64_t original_phase = desired - period;
		if (!display_timing) {
			u_pc_update_present_offset(pc, first_id, 25000000);
		}
		REQUIRE(u_pc_update_frame_period(pc, 8333333));
		u_pc_info_gpu(pc, first_id, 1000000000, 1000100000, 1001000000);
		u_pc_predict(pc, 1002000000, &id, &wake, &desired, &slop, &display, &period, &minimum);
		CHECK(id > first_id);
		CHECK(period == 8333333);
		CHECK(desired - wake == original_budget);
		if (display_timing) {
			CHECK(display - desired == config.present_to_display_offset_ns);
		} else {
			CHECK(desired == original_phase + period);
			CHECK(display - desired == 25000000);
		}
		CHECK_FALSE(u_pc_update_frame_period(pc, 0));
		CHECK_FALSE(u_pc_update_frame_period(pc, -1));
		REQUIRE(u_pc_update_frame_period(pc, 11111111));
		first_id = id;
		u_pc_predict(pc, 1004000000, &id, &wake, &desired, &slop, &display, &period, &minimum);
		CHECK(id > first_id);
		CHECK(period == 11111111);
		u_pc_destroy(&pc);
	}
}

namespace {

struct AppCadenceResult
{
	int frames{0};
	int off_cadence{0};
	int64_t min_step{INT64_MAX};
	int64_t max_step{0};
};

/*
 * Deterministic model of a GPU-bound application under the multi compositor:
 * the compositor samples every display period, the app's CPU work does not
 * wait for its own GPU work, and GPU work is serialized on one queue.
 */
AppCadenceResult
run_app_cadence(int64_t period_ns,
                int64_t extra_ns,
                int64_t cpu_ns,
                int64_t draw_ns,
                int64_t gpu_ns,
                int64_t post_ns,
                int64_t expected_periods,
                int64_t jitter_ns = 0)
{
	uint32_t lcg = 12345;
	auto jitter = [&]() -> int64_t {
		if (jitter_ns == 0) {
			return 0;
		}
		lcg = lcg * 1664525u + 1013904223u;
		return (int64_t)(lcg >> 8) % jitter_ns;
	};
	struct u_pacing_app *upa = nullptr;
	struct u_pacing_app_factory *upaf = nullptr;
	REQUIRE(u_pa_factory_create(&upaf) == XRT_SUCCESS);
	u_paf_create(upaf, &upa);
	REQUIRE(upa != nullptr);

	const int64_t origin_ns = 1000000000;
	int64_t now_ns = origin_ns;
	int64_t gpu_free_ns = 0;
	int64_t last_info_k = -1;
	std::queue<std::pair<int64_t, int64_t>> gpu_done; // (when, frame id)

	auto feed = [&](int64_t until_ns) {
		// Compositor wakes at k * period - extra and predicts display at k * period.
		int64_t k = (until_ns + extra_ns) / period_ns;
		while (!gpu_done.empty() && gpu_done.front().first <= until_ns) {
			u_pa_mark_gpu_done(upa, gpu_done.front().second, gpu_done.front().first);
			gpu_done.pop();
		}
		if (k != last_info_k) {
			u_pa_info(upa, k * period_ns, period_ns, extra_ns);
			last_info_k = k;
		}
	};

	AppCadenceResult r;
	int64_t last_display_ns = 0;
	for (int i = 0; i < 400; i++) {
		feed(now_ns);
		int64_t frame_id, wake_ns, display_ns, app_period_ns;
		u_pa_predict(upa, now_ns, &frame_id, &wake_ns, &display_ns, &app_period_ns);
		if (wake_ns > now_ns) {
			now_ns = wake_ns;
		}
		feed(now_ns);
		u_pa_mark_point(upa, frame_id, U_TIMING_POINT_WAKE_UP, now_ns);
		now_ns += cpu_ns + jitter();
		u_pa_mark_point(upa, frame_id, U_TIMING_POINT_BEGIN, now_ns);
		now_ns += draw_ns;
		u_pa_mark_delivered(upa, frame_id, now_ns, display_ns);
		gpu_free_ns = std::max(gpu_free_ns, now_ns) + gpu_ns + jitter();
		gpu_done.push({gpu_free_ns, frame_id});
		now_ns += post_ns + jitter();

		if (i >= 100) {
			int64_t step = display_ns - last_display_ns;
			r.frames++;
			r.min_step = std::min(r.min_step, step);
			r.max_step = std::max(r.max_step, step);
			int64_t periods = (step + period_ns / 2) / period_ns;
			if (periods != expected_periods) {
				r.off_cadence++;
			}
		}
		last_display_ns = display_ns;
	}

	u_pa_destroy(&upa);
	u_paf_destroy(&upaf);
	return r;
}

} // namespace

TEST_CASE("u_pacing_app keeps a steady divisor for GPU-bound apps")
{
	const int64_t period_ns = 8333333; // 120 Hz
	const int64_t ms = 1000000;

	struct Case
	{
		int64_t gpu_ns;
		int64_t expected_periods;
	} cases[] = {
	    {6 * ms, 1},  // fits a single refresh
	    {10 * ms, 2}, // Alyx-like heavy scene: should hold 60 fps
	    {12 * ms, 2},
	    {15 * ms, 2},
	    {18 * ms, 3},
	};

	for (const Case &c : cases) {
		for (int64_t extra_ms : {2, 4, 5, 7}) {
			for (int64_t post_us : {300, 1000, 2500, 4000}) {
				AppCadenceResult r = run_app_cadence(period_ns, extra_ms * ms, 3 * ms, ms / 2, c.gpu_ns,
				                                     post_us * 1000, c.expected_periods);
				INFO("gpu " << c.gpu_ns / ms << " ms, compositor extra " << extra_ms << " ms, post "
				            << post_us << " us: " << r.off_cadence << "/" << r.frames
				            << " frames off cadence, steps " << r.min_step << ".." << r.max_step);
				CHECK(r.off_cadence == 0);
			}
		}
	}
}

TEST_CASE("u_pacing_app holds 60 Hz on a 120 Hz display with jittery GPU-bound frames")
{
	const int64_t period_ns = 8333333;
	const int64_t ms = 1000000;
	for (int64_t gpu_ms : {10, 11, 12}) {
		for (int64_t post_ms : {1, 2, 3, 4}) {
			// Up to 1.5 ms of extra CPU, GPU and post-frame time per frame.
			AppCadenceResult r =
			    run_app_cadence(period_ns, 5 * ms, 3 * ms, ms / 2, gpu_ms * ms, post_ms * ms, 2, 3 * ms / 2);
			INFO("gpu " << gpu_ms << " ms, post " << post_ms << " ms: " << r.off_cadence << "/" << r.frames
			            << " frames off cadence, steps " << r.min_step << ".." << r.max_step);
			CHECK(r.off_cadence <= r.frames / 20);
		}
	}
}

namespace {

struct OnTimeResult
{
	int64_t frames = 0;
	int64_t late = 0;
	double mean_wake_to_display_ms = 0;
};

/*!
 * Frames are on time when their GPU work finishes before the compositor's
 * latch, display time minus @p extra_ns. GPU time has a heavy tail: one frame
 * in @p slow_every takes @p slow_extra_ns longer.
 */
OnTimeResult
run_app_on_time(bool adaptive, int64_t slow_every, int64_t slow_extra_ns, int64_t base_gpu_ns)
{
	const int64_t ms = 1000000;
	const int64_t period_ns = 11111111; // 90 Hz
	const int64_t extra_ns = 7 * ms;
	// Set either way: the default differs by platform.
	setenv("U_PACING_APP_ADAPTIVE_MARGIN", adaptive ? "1" : "0", 1);
	struct u_pacing_app_factory *upaf = nullptr;
	struct u_pacing_app *upa = nullptr;
	REQUIRE(u_pa_factory_create(&upaf) == XRT_SUCCESS);
	u_paf_create(upaf, &upa);
	unsetenv("U_PACING_APP_ADAPTIVE_MARGIN");
	REQUIRE(upa != nullptr);

	uint32_t lcg = 777;
	auto rnd = [&](int64_t range) -> int64_t {
		lcg = lcg * 1664525u + 1013904223u;
		return range == 0 ? 0 : (int64_t)(lcg >> 8) % range;
	};

	int64_t now_ns = 1000000000;
	int64_t gpu_free_ns = 0;
	int64_t last_info_k = -1;
	std::queue<std::pair<int64_t, int64_t>> gpu_done;
	auto feed = [&](int64_t until_ns) {
		int64_t k = (until_ns + extra_ns) / period_ns;
		while (!gpu_done.empty() && gpu_done.front().first <= until_ns) {
			u_pa_mark_gpu_done(upa, gpu_done.front().second, gpu_done.front().first);
			gpu_done.pop();
		}
		if (k != last_info_k) {
			u_pa_info(upa, k * period_ns, period_ns, extra_ns);
			last_info_k = k;
		}
	};

	OnTimeResult r;
	double wake_to_display_sum = 0;
	for (int i = 0; i < 3000; i++) {
		feed(now_ns);
		int64_t frame_id, wake_ns, display_ns, app_period_ns;
		u_pa_predict(upa, now_ns, &frame_id, &wake_ns, &display_ns, &app_period_ns);
		if (wake_ns > now_ns) {
			now_ns = wake_ns;
		}
		feed(now_ns);
		int64_t woke_ns = now_ns;
		u_pa_mark_point(upa, frame_id, U_TIMING_POINT_WAKE_UP, now_ns);
		now_ns += ms;
		u_pa_mark_point(upa, frame_id, U_TIMING_POINT_BEGIN, now_ns);
		now_ns += 2 * ms;
		u_pa_mark_delivered(upa, frame_id, now_ns, display_ns);
		int64_t gpu_ns = base_gpu_ns + rnd(ms) + (slow_every > 0 && rnd(slow_every) == 0 ? slow_extra_ns : 0);
		gpu_free_ns = std::max(gpu_free_ns, now_ns) + gpu_ns;
		gpu_done.push({gpu_free_ns, frame_id});
		now_ns += ms / 2;

		if (i >= 500) {
			r.frames++;
			r.late += gpu_free_ns > display_ns - extra_ns;
			wake_to_display_sum += (double)(display_ns - woke_ns) / (double)ms;
		}
	}
	r.mean_wake_to_display_ms = wake_to_display_sum / (double)r.frames;

	u_pa_destroy(&upa);
	u_paf_destroy(&upaf);
	return r;
}

} // namespace

TEST_CASE("u_pacing_app adaptive margin covers occasional slow frames", "[pacing][adaptive-margin]")
{
	const int64_t ms = 1000000;
	// One frame in ten takes 4 ms longer: a fixed 2 ms margin misses most of them.
	OnTimeResult fixed = run_app_on_time(false, 10, 4 * ms, 4 * ms);
	OnTimeResult adaptive = run_app_on_time(true, 10, 4 * ms, 4 * ms);
	INFO("fixed late " << fixed.late << "/" << fixed.frames << ", wake->display " << fixed.mean_wake_to_display_ms
	                   << " ms; adaptive late " << adaptive.late << "/" << adaptive.frames << ", wake->display "
	                   << adaptive.mean_wake_to_display_ms << " ms");
	CHECK(fixed.late > fixed.frames / 20);
	CHECK(adaptive.late <= adaptive.frames / 50);
	CHECK(adaptive.late * 3 < fixed.late);
}

TEST_CASE("u_pacing_app adaptive margin adds no latency for steady frames", "[pacing][adaptive-margin]")
{
	const int64_t ms = 1000000;
	OnTimeResult fixed = run_app_on_time(false, 0, 0, 4 * ms);
	OnTimeResult adaptive = run_app_on_time(true, 0, 0, 4 * ms);
	INFO("fixed late " << fixed.late << ", wake->display " << fixed.mean_wake_to_display_ms << " ms; adaptive late "
	                   << adaptive.late << ", wake->display " << adaptive.mean_wake_to_display_ms << " ms");
	CHECK(fixed.late == 0);
	CHECK(adaptive.late == 0);
	CHECK(adaptive.mean_wake_to_display_ms <= fixed.mean_wake_to_display_ms + 0.5);
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-extensions"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../src/xrt/drivers/psvr2/psvr2_tracking_share.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "math/m_filter_fifo.h"
#include "math/m_predict.h"
#include "catch_amalgamated.hpp"
#include <atomic>
#include <thread>
#include <memory>
#include <cmath>

TEST_CASE("Tracking snapshots remain coherent under concurrent publication")
{
	auto share = std::make_unique<psvr2_tracking_share>();
	auto out = std::make_unique<psvr2_tracking_snapshot>();
	uint64_t sequence = 0;
	REQUIRE_FALSE(psvr2_tracking_share_read(share.get(), out.get(), &sequence));
	std::atomic<bool> done{false};
	std::thread producer([&] {
		auto s = std::make_unique<psvr2_tracking_snapshot>();
		for (unsigned generation = 1; generation <= 10000; ++generation) {
			s->state.published_ns = generation;
			s->state.slam_ns = generation;
			s->state.imu_received_ns = generation * 17;
			s->state.imu_estimated_ns = generation * 19;
			s->state.slam_received_ns = generation * 23;
			s->gyro_count = generation % PSVR2_TRACKING_GYRO_CAPACITY;
			for (unsigned i = 0; i < s->gyro_count; ++i) {
				s->gyro[i].timestamp_ns = generation;
				s->gyro[i].value.x = (float)generation;
			}
			psvr2_tracking_share_publish(share.get(), s.get());
		}
		done = true;
	});
	bool coherent = true;
	do {
		if (psvr2_tracking_share_read(share.get(), out.get(), &sequence)) {
			auto generation = out->state.published_ns;
			coherent &= out->state.slam_ns == generation && !(sequence & 1);
			coherent &= out->state.imu_received_ns == generation * 17;
			coherent &= out->state.imu_estimated_ns == generation * 19;
			coherent &= out->state.slam_received_ns == generation * 23;
			coherent &= out->gyro_count == generation % PSVR2_TRACKING_GYRO_CAPACITY;
			for (unsigned i = 0; i < out->gyro_count; ++i)
				coherent &= out->gyro[i].timestamp_ns == (uint64_t)generation &&
				            out->gyro[i].value.x == generation;
		}
	} while (!done);
	producer.join();
	REQUIRE(coherent);
	REQUIRE(psvr2_tracking_share_read(share.get(), out.get(), &sequence));
	REQUIRE(out->state.published_ns == 10000);
	// An in-progress producer returns in bounded time, without exposing payload.
	__atomic_store_n(&share->sequence, sequence + 1, __ATOMIC_SEQ_CST);
	REQUIRE_FALSE(psvr2_tracking_share_read(share.get(), out.get(), &sequence));
}

TEST_CASE("Shared PS VR2 future prediction matches full gyro FIFO and position horizon")
{
	struct m_ff_vec3_f32 *full = nullptr, *copied = nullptr;
	m_ff_vec3_f32_alloc(&full, 1024);
	m_ff_vec3_f32_alloc(&copied, 1024);
	psvr2_tracking_state s{};
	s.relation.pose.orientation = {0, 0, 0, 1};
	s.relation.pose.position = {1, 2, 3};
	s.relation.linear_velocity = {0.25f, -0.5f, 1.0f};
	s.relation.relation_flags = (xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                       XRT_SPACE_RELATION_POSITION_VALID_BIT |
	                                                       XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                                                       XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	                                                       XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);
	s.slam_ns = 1000000000;
	s.hw2mono_vts = 3000000000;
	for (int i = -10; i <= 40; ++i) {
		xrt_vec3 g{0.1f + i * 0.001f, -0.2f, 0.05f};
		m_ff_vec3_f32_push(full, &g, s.slam_ns + i * 500000);
		if (i >= -1)
			m_ff_vec3_f32_push(copied, &g, s.slam_ns + i * 500000);
		s.last_gyro = g;
	}
	for (int horizon_us : {1, 250, 500, 17500, 20000, 25000, 50000, 150000}) {
		int64_t target = s.slam_ns + horizon_us * 1000;
		xrt_space_relation expected = s.relation, actual{};
		math_quat_rotate_derivative(&expected.pose.orientation, &s.last_gyro, &expected.angular_velocity);
		expected.relation_flags =
		    (xrt_space_relation_flags)(expected.relation_flags | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
		auto seed = expected;
		REQUIRE(t_apply_dead_reckoning(full, nullptr, nullptr, target, &seed, s.slam_ns, &expected));
		// Original driver's position uses the full SLAM->target interval.
		float dt = horizon_us * 1e-6f;
		expected.pose.position = {1 + .25f * dt, 2 - .5f * dt, 3 + dt};
		expected.linear_velocity = seed.linear_velocity;
		psvr2_tracking_predict_raw(&s, copied, target, s.slam_ns + s.hw2mono_vts + 20000000, &actual);
		REQUIRE(actual.pose.orientation.x == Catch::Approx(expected.pose.orientation.x).margin(1e-6));
		REQUIRE(actual.pose.orientation.y == Catch::Approx(expected.pose.orientation.y).margin(1e-6));
		REQUIRE(actual.pose.orientation.z == Catch::Approx(expected.pose.orientation.z).margin(1e-6));
		REQUIRE(actual.pose.orientation.w == Catch::Approx(expected.pose.orientation.w).margin(1e-6));
		REQUIRE(actual.pose.position.x == Catch::Approx(expected.pose.position.x));
		REQUIRE(actual.pose.position.y == Catch::Approx(expected.pose.position.y));
		REQUIRE(actual.pose.position.z == Catch::Approx(expected.pose.position.z));
		REQUIRE(actual.relation_flags == expected.relation_flags);
	}
	SECTION("Source staleness is measured in host time and freezes rather than extrapolates")
	{
		xrt_space_relation result{};
		psvr2_tracking_predict_raw(&s, copied, s.slam_ns + 900000000,
		                           s.slam_ns + s.hw2mono_vts + PSVR2_TRACKING_STALE_NS + 1, &result);
		REQUIRE(result.pose.position.x == 1);
		REQUIRE(result.pose.orientation.w == 1);
		REQUIRE_FALSE(result.relation_flags & XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
		REQUIRE_FALSE(result.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
		REQUIRE_FALSE(result.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);
		REQUIRE(result.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT);
		REQUIRE(result.linear_velocity.x == 0);
	}
	SECTION("Recenter rotates pose and velocities after the tracker-to-head offset")
	{
		s.T_imu_head = {{0, 0, 0, 1}, {0, .1f, 0}};
		s.recenter_transform = {{0, 0, 1, 0}, {10, 20, 30}};
		s.recenter_initialized = true;
		xrt_space_relation result{}, raw{}, uncentered{};
		psvr2_tracking_predict_raw(&s, copied, s.slam_ns + 25000000, s.slam_ns + s.hw2mono_vts, &raw);
		psvr2_tracking_predict_head(&s, copied, s.slam_ns + 25000000, s.slam_ns + s.hw2mono_vts, &uncentered);
		s.recenter_on_first_pose = true;
		psvr2_tracking_predict_head(&s, copied, s.slam_ns + 25000000, s.slam_ns + s.hw2mono_vts, &result);
		REQUIRE(result.linear_velocity.x == Catch::Approx(-uncentered.linear_velocity.x));
		REQUIRE(result.linear_velocity.y == Catch::Approx(-uncentered.linear_velocity.y));
		REQUIRE(result.linear_velocity.z == Catch::Approx(uncentered.linear_velocity.z));
		REQUIRE(result.pose.position.x < 10);
		REQUIRE(result.pose.position.y < 20);
		REQUIRE(result.pose.position.z > 30);
	}

	SECTION("Acceleration and continuity state survive snapshot transfer")
	{
		s.acceleration_prediction_enabled = s.continuity_prediction_enabled = true;
		s.linear_prediction.timestamp_ns = s.slam_ns;
		s.linear_prediction.position = s.relation.pose.position;
		s.linear_prediction.velocity = s.relation.linear_velocity;
		s.linear_prediction.acceleration = {.3f, -.2f, .1f};
		s.linear_prediction.interval_s = .016f;
		s.linear_prediction.ready = s.linear_prediction.have_velocity = true;
		s.linear_prediction_params = {.25f, .5f, 10.f, .01f, .1f};
		s.continuity_prediction.source_ns = s.slam_ns;
		s.continuity_prediction.received_ns = s.slam_ns + s.hw2mono_vts;
		s.continuity_prediction.correction_position = {.001f, -.002f, .003f};
		s.continuity_prediction.valid = true;
		s.continuity_params = {.02f, .01f};
		auto share = std::make_unique<psvr2_tracking_share>();
		auto snapshot = std::make_unique<psvr2_tracking_snapshot>();
		auto received = std::make_unique<psvr2_tracking_snapshot>();
		snapshot->state = s;
		psvr2_tracking_share_publish(share.get(), snapshot.get());
		uint64_t sequence;
		REQUIRE(psvr2_tracking_share_read(share.get(), received.get(), &sequence));
		int64_t target = s.slam_ns + 25000000, query = s.slam_ns + s.hw2mono_vts + 10000000;
		xrt_vec3 position{}, velocity{};
		REQUIRE(psvr2_linear_predict(&s.linear_prediction, &s.linear_prediction_params, target, &position,
		                             &velocity));
		psvr2_continuity_predict(&s.continuity_prediction, &s.continuity_params, target, query, &position,
		                         &velocity);
		xrt_space_relation result{};
		psvr2_tracking_predict_raw(&received->state, copied, target, query, &result);
		REQUIRE(result.pose.position.x == Catch::Approx(position.x));
		REQUIRE(result.pose.position.y == Catch::Approx(position.y));
		REQUIRE(result.pose.position.z == Catch::Approx(position.z));
		REQUIRE(result.linear_velocity.x == Catch::Approx(velocity.x));
		REQUIRE(result.linear_velocity.y == Catch::Approx(velocity.y));
		REQUIRE(result.linear_velocity.z == Catch::Approx(velocity.z));
	}
	m_ff_vec3_f32_free(&full);
	m_ff_vec3_f32_free(&copied);
}

TEST_CASE("Snapshot FIFO import retains equal timestamps and replaces old samples")
{
	struct m_ff_vec3_f32 *fifo = nullptr;
	m_ff_vec3_f32_alloc(&fifo, 1024);
	psvr2_tracking_snapshot snapshot{};
	snapshot.gyro_count = 3;
	snapshot.gyro[0] = {1000001000, {2, 3, 4}};
	snapshot.gyro[1] = {1000001000, {1, 0, 0}};
	snapshot.gyro[2] = {1000000000, {0, 1, 0}};
	psvr2_tracking_snapshot_fill_gyro(&snapshot, fifo);
	for (unsigned i = 0; i < snapshot.gyro_count; ++i) {
		xrt_vec3 gyro{};
		uint64_t timestamp = 0;
		REQUIRE(m_ff_vec3_f32_get(fifo, i, &gyro, &timestamp));
		REQUIRE(timestamp == snapshot.gyro[i].timestamp_ns);
		REQUIRE(gyro.x == snapshot.gyro[i].value.x);
		REQUIRE(gyro.y == snapshot.gyro[i].value.y);
		REQUIRE(gyro.z == snapshot.gyro[i].value.z);
	}
	snapshot.gyro_count = 1;
	snapshot.gyro[0] = {2000000000, {5, 6, 7}};
	psvr2_tracking_snapshot_fill_gyro(&snapshot, fifo);
	uint64_t timestamp = 1;
	REQUIRE(m_ff_vec3_f32_get_timestamp(fifo, 1, &timestamp));
	REQUIRE(timestamp == 0);
	snapshot.gyro_count = 0;
	psvr2_tracking_snapshot_fill_gyro(&snapshot, fifo);
	REQUIRE(m_ff_vec3_f32_get_timestamp(fifo, 0, &timestamp));
	REQUIRE(timestamp == 0);
	m_ff_vec3_f32_free(&fifo);
}

// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Test the public C API of m_relation_history.
 */

#include "xrt/xrt_defines.h"

#include "math/m_relation_history.h"

#include "util/u_time.h"

#include "catch_amalgamated.hpp"

using Catch::Approx;


/*
 *
 * Helpers
 *
 */

constexpr xrt_space_relation_flags kPoseFlags = (xrt_space_relation_flags)( //
    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |                              //
    XRT_SPACE_RELATION_POSITION_VALID_BIT);                                 //

constexpr xrt_space_relation_flags kPoseLinVelFlags = (xrt_space_relation_flags)( //
    kPoseFlags |                                                                  //
    XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);                                //

static xrt_space_relation
make_relation(float x, xrt_space_relation_flags flags = kPoseFlags)
{
	xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	rel.relation_flags = flags;
	rel.pose.position = {x, 0.0f, 0.0f};
	return rel;
}

static xrt_space_relation
make_relation_with_velocity(float x, float vx)
{
	xrt_space_relation rel = make_relation(x, kPoseLinVelFlags);
	rel.linear_velocity = {vx, 0.0f, 0.0f};
	return rel;
}

struct History
{
	m_relation_history *rh = nullptr;

	History()
	{
		m_relation_history_create(&rh);
	}
	~History()
	{
		m_relation_history_destroy(&rh);
	}

	History(const History &) = delete;
	History &
	operator=(const History &) = delete;
};


/*
 *
 * Tests
 *
 */

TEST_CASE("m_relation_history create/destroy")
{
	m_relation_history *rh = nullptr;
	m_relation_history_create(&rh);
	REQUIRE(rh != nullptr);
	CHECK(m_relation_history_get_size(rh) == 0);

	m_relation_history_destroy(&rh);
	CHECK(rh == nullptr);

	// Destroying an already destroyed object is a no-op.
	m_relation_history_destroy(&rh);
	CHECK(rh == nullptr);
}

TEST_CASE("m_relation_history empty")
{
	History h;
	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;

	CHECK(m_relation_history_get(h.rh, U_TIME_1S_IN_NS, &out) == M_RELATION_HISTORY_RESULT_INVALID);
	CHECK(out.relation_flags == 0);

	int64_t ts = 0;
	CHECK_FALSE(m_relation_history_get_latest(h.rh, &ts, &out));
}

TEST_CASE("m_relation_history push")
{
	History h;
	xrt_space_relation rel = make_relation(1.0f);

	CHECK(m_relation_history_push(h.rh, &rel, 2 * U_TIME_1S_IN_NS));
	CHECK(m_relation_history_get_size(h.rh) == 1);

	// Newer is accepted.
	CHECK(m_relation_history_push(h.rh, &rel, 3 * U_TIME_1S_IN_NS));
	CHECK(m_relation_history_get_size(h.rh) == 2);

	// Same and older timestamps are rejected.
	CHECK_FALSE(m_relation_history_push(h.rh, &rel, 3 * U_TIME_1S_IN_NS));
	CHECK_FALSE(m_relation_history_push(h.rh, &rel, 1 * U_TIME_1S_IN_NS));
	CHECK(m_relation_history_get_size(h.rh) == 2);
}

TEST_CASE("m_relation_history push_or_update")
{
	History h;
	xrt_space_relation a = make_relation(1.0f);
	xrt_space_relation b = make_relation(2.0f);
	xrt_space_relation c = make_relation(3.0f);

	REQUIRE(m_relation_history_push_or_update(h.rh, &a, 1 * U_TIME_1S_IN_NS));
	REQUIRE(m_relation_history_push_or_update(h.rh, &b, 2 * U_TIME_1S_IN_NS));
	REQUIRE(m_relation_history_push_or_update(h.rh, &c, 3 * U_TIME_1S_IN_NS));
	REQUIRE(m_relation_history_get_size(h.rh) == 3);

	// Update an old entry in place.
	xrt_space_relation replacement = make_relation(20.0f);
	CHECK(m_relation_history_push_or_update(h.rh, &replacement, 2 * U_TIME_1S_IN_NS));
	CHECK(m_relation_history_get_size(h.rh) == 3);

	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;
	CHECK(m_relation_history_get(h.rh, 2 * U_TIME_1S_IN_NS, &out) == M_RELATION_HISTORY_RESULT_EXACT);
	CHECK(out.pose.position.x == Approx(20.0f));

	// Update the latest entry in place.
	replacement = make_relation(30.0f);
	CHECK(m_relation_history_push_or_update(h.rh, &replacement, 3 * U_TIME_1S_IN_NS));
	CHECK(m_relation_history_get_size(h.rh) == 3);

	int64_t ts = 0;
	REQUIRE(m_relation_history_get_latest(h.rh, &ts, &out));
	CHECK(ts == 3 * U_TIME_1S_IN_NS);
	CHECK(out.pose.position.x == Approx(30.0f));

	// A timestamp between two entries is neither new nor an existing one.
	CHECK_FALSE(m_relation_history_push_or_update(h.rh, &replacement, 2 * U_TIME_1S_IN_NS + 1));
	// Nor is one older than everything.
	CHECK_FALSE(m_relation_history_push_or_update(h.rh, &replacement, 500));
	CHECK(m_relation_history_get_size(h.rh) == 3);

	// Plain push never updates.
	CHECK_FALSE(m_relation_history_push(h.rh, &replacement, 2 * U_TIME_1S_IN_NS));
	CHECK(m_relation_history_get(h.rh, 2 * U_TIME_1S_IN_NS, &out) == M_RELATION_HISTORY_RESULT_EXACT);
	CHECK(out.pose.position.x == Approx(20.0f));
}

TEST_CASE("m_relation_history get_latest")
{
	History h;
	xrt_space_relation a = make_relation(1.0f);
	xrt_space_relation b = make_relation(2.0f);

	REQUIRE(m_relation_history_push(h.rh, &a, 1 * U_TIME_1S_IN_NS));
	REQUIRE(m_relation_history_push(h.rh, &b, 2 * U_TIME_1S_IN_NS));

	int64_t ts = 0;
	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;
	REQUIRE(m_relation_history_get_latest(h.rh, &ts, &out));
	CHECK(ts == 2 * U_TIME_1S_IN_NS);
	CHECK(out.pose.position.x == Approx(2.0f));

	// Doesn't consume anything.
	CHECK(m_relation_history_get_size(h.rh) == 2);
}

TEST_CASE("m_relation_history get exact")
{
	History h;
	xrt_space_relation a = make_relation(1.0f);
	xrt_space_relation b = make_relation(2.0f);

	REQUIRE(m_relation_history_push(h.rh, &a, 1 * U_TIME_1S_IN_NS));
	REQUIRE(m_relation_history_push(h.rh, &b, 2 * U_TIME_1S_IN_NS));

	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;

	CHECK(m_relation_history_get(h.rh, 1 * U_TIME_1S_IN_NS, &out) == M_RELATION_HISTORY_RESULT_EXACT);
	CHECK(out.pose.position.x == Approx(1.0f));
	CHECK(out.relation_flags == kPoseFlags);

	CHECK(m_relation_history_get(h.rh, 2 * U_TIME_1S_IN_NS, &out) == M_RELATION_HISTORY_RESULT_EXACT);
	CHECK(out.pose.position.x == Approx(2.0f));
}

TEST_CASE("m_relation_history get timestamp zero is invalid")
{
	History h;
	xrt_space_relation a = make_relation(1.0f);
	REQUIRE(m_relation_history_push(h.rh, &a, 1 * U_TIME_1S_IN_NS));

	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;
	CHECK(m_relation_history_get(h.rh, 0, &out) == M_RELATION_HISTORY_RESULT_INVALID);
}

TEST_CASE("m_relation_history get interpolated")
{
	History h;
	xrt_space_relation a = make_relation(0.0f);
	xrt_space_relation b = make_relation(10.0f);

	REQUIRE(m_relation_history_push(h.rh, &a, 1 * U_TIME_1S_IN_NS));
	REQUIRE(m_relation_history_push(h.rh, &b, 2 * U_TIME_1S_IN_NS));

	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;

	CHECK(m_relation_history_get(h.rh, U_TIME_1S_IN_NS + U_TIME_1S_IN_NS / 2, &out) ==
	      M_RELATION_HISTORY_RESULT_INTERPOLATED);
	CHECK(out.pose.position.x == Approx(5.0f));
	CHECK(out.relation_flags == kPoseFlags);

	CHECK(m_relation_history_get(h.rh, U_TIME_1S_IN_NS + U_TIME_1S_IN_NS / 4, &out) ==
	      M_RELATION_HISTORY_RESULT_INTERPOLATED);
	CHECK(out.pose.position.x == Approx(2.5f));
}

TEST_CASE("m_relation_history get interpolated only keeps shared flags")
{
	History h;
	xrt_space_relation a = make_relation_with_velocity(0.0f, 1.0f);
	xrt_space_relation b = make_relation(10.0f); // No velocity.

	REQUIRE(m_relation_history_push(h.rh, &a, 1 * U_TIME_1S_IN_NS));
	REQUIRE(m_relation_history_push(h.rh, &b, 2 * U_TIME_1S_IN_NS));

	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;
	CHECK(m_relation_history_get(h.rh, U_TIME_1S_IN_NS + U_TIME_1S_IN_NS / 2, &out) ==
	      M_RELATION_HISTORY_RESULT_INTERPOLATED);
	CHECK(out.relation_flags == kPoseFlags);
}

TEST_CASE("m_relation_history get predicted")
{
	History h;
	xrt_space_relation a = make_relation_with_velocity(0.0f, 2.0f);
	xrt_space_relation b = make_relation_with_velocity(2.0f, 2.0f);

	REQUIRE(m_relation_history_push(h.rh, &a, 1 * U_TIME_1S_IN_NS));
	REQUIRE(m_relation_history_push(h.rh, &b, 2 * U_TIME_1S_IN_NS));

	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;

	// Half a second past the newest entry, moving at 2 m/s.
	CHECK(m_relation_history_get(h.rh, 2 * U_TIME_1S_IN_NS + U_TIME_1S_IN_NS / 2, &out) ==
	      M_RELATION_HISTORY_RESULT_PREDICTED);
	CHECK(out.pose.position.x == Approx(3.0f));
}

TEST_CASE("m_relation_history get reverse predicted")
{
	History h;
	// Distinct positions and the same velocity, so that predicting backwards from the oldest entry gives a
	// different answer than predicting backwards from the newest.
	xrt_space_relation a = make_relation_with_velocity(0.0f, 2.0f);
	xrt_space_relation b = make_relation_with_velocity(100.0f, 2.0f);

	REQUIRE(m_relation_history_push(h.rh, &a, 1 * U_TIME_1S_IN_NS));
	REQUIRE(m_relation_history_push(h.rh, &b, 2 * U_TIME_1S_IN_NS));

	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;

	// Half a second before the oldest entry, so we expect to extrapolate back from the oldest entry.
	CHECK(m_relation_history_get(h.rh, U_TIME_1S_IN_NS / 2, &out) == M_RELATION_HISTORY_RESULT_REVERSE_PREDICTED);
	CHECK(out.pose.position.x == Approx(-1.0f));
}

TEST_CASE("m_relation_history clear")
{
	History h;
	xrt_space_relation a = make_relation(1.0f);

	REQUIRE(m_relation_history_push(h.rh, &a, 5 * U_TIME_1S_IN_NS));
	REQUIRE(m_relation_history_get_size(h.rh) == 1);

	m_relation_history_clear(h.rh);
	CHECK(m_relation_history_get_size(h.rh) == 0);

	int64_t ts = 0;
	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;
	CHECK_FALSE(m_relation_history_get_latest(h.rh, &ts, &out));
	CHECK(m_relation_history_get(h.rh, 5 * U_TIME_1S_IN_NS, &out) == M_RELATION_HISTORY_RESULT_INVALID);

	// Clearing resets the monotonic timestamp requirement.
	CHECK(m_relation_history_push(h.rh, &a, 1 * U_TIME_1S_IN_NS));
	CHECK(m_relation_history_get_size(h.rh) == 1);
}

TEST_CASE("m_relation_history push_with_motion_estimation")
{
	History h;

	// First entry has no previous one to estimate from, stored as is.
	xrt_space_relation a = make_relation(0.0f);
	REQUIRE(m_relation_history_push_with_motion_estimation(h.rh, &a, 1 * U_TIME_1S_IN_NS));

	int64_t ts = 0;
	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;
	REQUIRE(m_relation_history_get_latest(h.rh, &ts, &out));
	CHECK(out.relation_flags == kPoseFlags);

	// Moved one meter in one second, so we expect 1 m/s.
	xrt_space_relation b = make_relation(1.0f);
	REQUIRE(m_relation_history_push_with_motion_estimation(h.rh, &b, 2 * U_TIME_1S_IN_NS));

	REQUIRE(m_relation_history_get_latest(h.rh, &ts, &out));
	CHECK(ts == 2 * U_TIME_1S_IN_NS);
	CHECK((out.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) != 0);
	CHECK((out.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT) != 0);
	CHECK(out.linear_velocity.x == Approx(1.0f));
	CHECK(out.linear_velocity.y == Approx(0.0f));
	CHECK(out.linear_velocity.z == Approx(0.0f));
	CHECK(out.angular_velocity.x == Approx(0.0f).margin(1e-5));
	CHECK(out.angular_velocity.y == Approx(0.0f).margin(1e-5));
	CHECK(out.angular_velocity.z == Approx(0.0f).margin(1e-5));

	// Existing velocity is kept as is.
	xrt_space_relation c = make_relation_with_velocity(5.0f, 42.0f);
	REQUIRE(m_relation_history_push_with_motion_estimation(h.rh, &c, 3 * U_TIME_1S_IN_NS));

	REQUIRE(m_relation_history_get_latest(h.rh, &ts, &out));
	CHECK(out.linear_velocity.x == Approx(42.0f));

	// Rejected like a regular push when not newer.
	CHECK_FALSE(m_relation_history_push_with_motion_estimation(h.rh, &c, 3 * U_TIME_1S_IN_NS));
	CHECK_FALSE(m_relation_history_push_with_motion_estimation(h.rh, &c, 2 * U_TIME_1S_IN_NS));
	CHECK(m_relation_history_get_size(h.rh) == 3);
}

TEST_CASE("m_relation_history is a bounded ring buffer")
{
	History h;
	xrt_space_relation rel = make_relation(0.0f);

	constexpr int64_t kCount = 10000;
	for (int64_t i = 1; i <= kCount; i++) {
		REQUIRE(m_relation_history_push(h.rh, &rel, i * 1000));
	}

	// Oldest entries were dropped, newest are kept.
	CHECK(m_relation_history_get_size(h.rh) < kCount);

	int64_t ts = 0;
	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;
	REQUIRE(m_relation_history_get_latest(h.rh, &ts, &out));
	CHECK(ts == kCount * 1000);

	CHECK(m_relation_history_get(h.rh, kCount * 1000, &out) == M_RELATION_HISTORY_RESULT_EXACT);
	CHECK(m_relation_history_get(h.rh, 1000, &out) == M_RELATION_HISTORY_RESULT_REVERSE_PREDICTED);
}

TEST_CASE("m_relation_history get_exact")
{
	History h;
	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;

	// Empty history has nothing.
	CHECK_FALSE(m_relation_history_get_exact(h.rh, U_TIME_1S_IN_NS, &out));

	for (int64_t i = 1; i <= 3; i++) {
		xrt_space_relation rel = make_relation((float)i);
		REQUIRE(m_relation_history_push(h.rh, &rel, i * U_TIME_1S_IN_NS));
	}

	SECTION("hits return the stored relation, including the first and last")
	{
		for (int64_t i = 1; i <= 3; i++) {
			out = XRT_SPACE_RELATION_ZERO;
			REQUIRE(m_relation_history_get_exact(h.rh, i * U_TIME_1S_IN_NS, &out));
			CHECK(out.pose.position.x == Approx((float)i));
			CHECK(out.relation_flags == kPoseFlags);
		}
	}

	SECTION("misses fail and leave the output untouched, never returning a neighbouring entry")
	{
		out = make_relation(-1.0f);

		CHECK_FALSE(m_relation_history_get_exact(h.rh, 0, &out));
		CHECK_FALSE(m_relation_history_get_exact(h.rh, 1 * U_TIME_1S_IN_NS - 1, &out));
		CHECK_FALSE(m_relation_history_get_exact(h.rh, 1 * U_TIME_1S_IN_NS + 1, &out));
		CHECK_FALSE(m_relation_history_get_exact(h.rh, 2 * U_TIME_1S_IN_NS + U_TIME_1S_IN_NS / 2, &out));
		CHECK_FALSE(m_relation_history_get_exact(h.rh, 3 * U_TIME_1S_IN_NS + 1, &out));
		CHECK_FALSE(m_relation_history_get_exact(h.rh, 10 * U_TIME_1S_IN_NS, &out));

		CHECK(out.pose.position.x == Approx(-1.0f));
	}

	SECTION("sees updates to an existing entry")
	{
		xrt_space_relation updated = make_relation(20.0f);
		REQUIRE(m_relation_history_push_or_update(h.rh, &updated, 2 * U_TIME_1S_IN_NS));

		REQUIRE(m_relation_history_get_exact(h.rh, 2 * U_TIME_1S_IN_NS, &out));
		CHECK(out.pose.position.x == Approx(20.0f));
	}

	SECTION("does not find entries after clear")
	{
		m_relation_history_clear(h.rh);
		CHECK_FALSE(m_relation_history_get_exact(h.rh, 2 * U_TIME_1S_IN_NS, &out));
	}
}

TEST_CASE("m_relation_history get_exact after the ring buffer wraps")
{
	History h;
	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;

	constexpr int64_t kCount = 10000;
	for (int64_t i = 1; i <= kCount; i++) {
		xrt_space_relation rel = make_relation((float)i);
		REQUIRE(m_relation_history_push(h.rh, &rel, i * 1000));
	}

	// Newest is kept, oldest was dropped.
	REQUIRE(m_relation_history_get_exact(h.rh, kCount * 1000, &out));
	CHECK(out.pose.position.x == Approx((float)kCount));

	CHECK_FALSE(m_relation_history_get_exact(h.rh, 1000, &out));
}

TEST_CASE("m_relation_history get_nearest")
{
	History h;
	xrt_space_relation out = XRT_SPACE_RELATION_ZERO;
	int64_t out_ts = 0;

	constexpr int64_t kSlop = U_TIME_1MS_IN_NS;

	// Empty history has nothing.
	CHECK_FALSE(m_relation_history_get_nearest(h.rh, U_TIME_1S_IN_NS, kSlop, &out_ts, &out));

	for (int64_t i = 1; i <= 3; i++) {
		xrt_space_relation rel = make_relation((float)i);
		REQUIRE(m_relation_history_push(h.rh, &rel, i * U_TIME_1S_IN_NS));
	}

	SECTION("exact hits return the stored relation, even with zero slop")
	{
		for (int64_t i = 1; i <= 3; i++) {
			out = XRT_SPACE_RELATION_ZERO;
			REQUIRE(m_relation_history_get_nearest(h.rh, i * U_TIME_1S_IN_NS, 0, &out_ts, &out));
			CHECK(out_ts == i * U_TIME_1S_IN_NS);
			CHECK(out.pose.position.x == Approx((float)i));
			CHECK(out.relation_flags == kPoseFlags);
		}
	}

	SECTION("near misses within slop return the neighbouring entry unmodified")
	{
		// Just after an entry.
		REQUIRE(m_relation_history_get_nearest(h.rh, 2 * U_TIME_1S_IN_NS + 10, kSlop, &out_ts, &out));
		CHECK(out_ts == 2 * U_TIME_1S_IN_NS);
		CHECK(out.pose.position.x == Approx(2.0f));

		// Just before an entry.
		REQUIRE(m_relation_history_get_nearest(h.rh, 2 * U_TIME_1S_IN_NS - 10, kSlop, &out_ts, &out));
		CHECK(out_ts == 2 * U_TIME_1S_IN_NS);
		CHECK(out.pose.position.x == Approx(2.0f));

		// Before the front and after the back.
		REQUIRE(m_relation_history_get_nearest(h.rh, 1 * U_TIME_1S_IN_NS - 10, kSlop, &out_ts, &out));
		CHECK(out_ts == 1 * U_TIME_1S_IN_NS);
		REQUIRE(m_relation_history_get_nearest(h.rh, 3 * U_TIME_1S_IN_NS + 10, kSlop, &out_ts, &out));
		CHECK(out_ts == 3 * U_TIME_1S_IN_NS);
	}

	SECTION("slop bound is inclusive")
	{
		REQUIRE(m_relation_history_get_nearest(h.rh, 2 * U_TIME_1S_IN_NS + kSlop, kSlop, &out_ts, &out));
		CHECK(out_ts == 2 * U_TIME_1S_IN_NS);
		REQUIRE(m_relation_history_get_nearest(h.rh, 2 * U_TIME_1S_IN_NS - kSlop, kSlop, &out_ts, &out));
		CHECK(out_ts == 2 * U_TIME_1S_IN_NS);
	}

	SECTION("picks the closer of the two neighbours")
	{
		constexpr int64_t kBigSlop = U_TIME_1S_IN_NS;

		REQUIRE(m_relation_history_get_nearest(h.rh, 1 * U_TIME_1S_IN_NS + U_TIME_1S_IN_NS / 4, kBigSlop,
		                                       &out_ts, &out));
		CHECK(out_ts == 1 * U_TIME_1S_IN_NS);

		REQUIRE(m_relation_history_get_nearest(h.rh, 1 * U_TIME_1S_IN_NS + 3 * U_TIME_1S_IN_NS / 4, kBigSlop,
		                                       &out_ts, &out));
		CHECK(out_ts == 2 * U_TIME_1S_IN_NS);

		// Ties go to the older entry.
		REQUIRE(m_relation_history_get_nearest(h.rh, 1 * U_TIME_1S_IN_NS + U_TIME_1S_IN_NS / 2, kBigSlop,
		                                       &out_ts, &out));
		CHECK(out_ts == 1 * U_TIME_1S_IN_NS);
	}

	SECTION("misses outside slop fail and leave the outputs untouched")
	{
		out = make_relation(-1.0f);
		out_ts = -1;

		CHECK_FALSE(m_relation_history_get_nearest(h.rh, 0, kSlop, &out_ts, &out));
		CHECK_FALSE(
		    m_relation_history_get_nearest(h.rh, 1 * U_TIME_1S_IN_NS - kSlop - 1, kSlop, &out_ts, &out));
		CHECK_FALSE(
		    m_relation_history_get_nearest(h.rh, 2 * U_TIME_1S_IN_NS + kSlop + 1, kSlop, &out_ts, &out));
		CHECK_FALSE(m_relation_history_get_nearest(h.rh, 2 * U_TIME_1S_IN_NS + U_TIME_1S_IN_NS / 2, kSlop,
		                                           &out_ts, &out));
		CHECK_FALSE(
		    m_relation_history_get_nearest(h.rh, 3 * U_TIME_1S_IN_NS + kSlop + 1, kSlop, &out_ts, &out));
		CHECK_FALSE(m_relation_history_get_nearest(h.rh, 2 * U_TIME_1S_IN_NS + 1, 0, &out_ts, &out));

		// Negative slop never matches, even exactly.
		CHECK_FALSE(m_relation_history_get_nearest(h.rh, 2 * U_TIME_1S_IN_NS, -1, &out_ts, &out));

		CHECK(out.pose.position.x == Approx(-1.0f));
		CHECK(out_ts == -1);
	}

	SECTION("does not find entries after clear")
	{
		m_relation_history_clear(h.rh);
		CHECK_FALSE(m_relation_history_get_nearest(h.rh, 2 * U_TIME_1S_IN_NS, kSlop, &out_ts, &out));
	}
}

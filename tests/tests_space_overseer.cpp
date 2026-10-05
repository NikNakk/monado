// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Space overseer reference-space tests.
 */

#include "xrt/xrt_space.h"
#include "xrt/xrt_session.h"
#include "os/os_time.h"
#include "b_space_overseer.h"

#include "catch_amalgamated.hpp"

namespace {

struct CountingSink
{
	struct xrt_session_event_sink base;
	int ref_changes = 0;
};

xrt_result_t
push_event(struct xrt_session_event_sink *xses, const union xrt_session_event *xse)
{
	auto *sink = reinterpret_cast<CountingSink *>(xses);
	if (xse->type == XRT_SESSION_EVENT_REFERENCE_SPACE_CHANGE_PENDING) {
		sink->ref_changes++;
	}
	return XRT_SUCCESS;
}

float
height_in_root(struct xrt_space_overseer *xso, struct xrt_space *space)
{
	struct xrt_pose identity = XRT_POSE_IDENTITY;
	struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
	REQUIRE(xrt_space_overseer_locate_space(xso, xso->semantic.root, &identity, os_monotonic_get_ns(), space,
	                                        &identity, &rel) == XRT_SUCCESS);
	REQUIRE((rel.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0);
	return rel.pose.position.y;
}

struct xrt_space_overseer *
create_overseer(CountingSink &sink, bool per_app_local_spaces)
{
	sink.base.push_event = push_event;
	struct b_space_overseer *uso = b_space_overseer_create(&sink.base);
	struct xrt_pose local_offset = XRT_POSE_IDENTITY;
	local_offset.position.y = 1.6f;
	// No devices or head: the stage is managed by the overseer.
	b_space_overseer_legacy_setup(uso, nullptr, 0, nullptr, &local_offset, false, per_app_local_spaces);
	return reinterpret_cast<struct xrt_space_overseer *>(uso);
}

} // namespace

TEST_CASE("Floor calibration of a managed stage moves LOCAL_FLOOR")
{
	for (bool per_app : {false, true}) {
		DYNAMIC_SECTION("per-app local spaces: " << per_app)
		{
			CountingSink sink;
			struct xrt_space_overseer *xso = create_overseer(sink, per_app);

			// An application created before the calibration.
			struct xrt_space *local = nullptr;
			struct xrt_space *local_floor = nullptr;
			REQUIRE(xrt_space_overseer_create_local_space(xso, &local, &local_floor) == XRT_SUCCESS);
			CHECK(height_in_root(xso, local_floor) == Catch::Approx(0.0f).margin(1e-5));
			const float local_height = height_in_root(xso, local);

			// Calibrate: the physical floor is 1.2 m below the root origin.
			struct xrt_pose stage = XRT_POSE_IDENTITY;
			stage.position.y = -1.2f;
			REQUIRE(xrt_space_overseer_set_reference_space_offset(xso, XRT_SPACE_REFERENCE_TYPE_STAGE,
			                                                      &stage) == XRT_SUCCESS);
			CHECK(sink.ref_changes == 2); // STAGE and LOCAL_FLOOR

			CHECK(height_in_root(xso, xso->semantic.stage) == Catch::Approx(-1.2f).margin(1e-5));
			CHECK(height_in_root(xso, local_floor) == Catch::Approx(-1.2f).margin(1e-5));
			// LOCAL is head-relative and unaffected by the floor.
			CHECK(height_in_root(xso, local) == Catch::Approx(local_height).margin(1e-5));

			// An application created after the calibration sees the same floor.
			struct xrt_space *later_local = nullptr;
			struct xrt_space *later_floor = nullptr;
			REQUIRE(xrt_space_overseer_create_local_space(xso, &later_local, &later_floor) == XRT_SUCCESS);
			CHECK(height_in_root(xso, later_floor) == Catch::Approx(-1.2f).margin(1e-5));

			xrt_space_reference(&later_floor, nullptr);
			xrt_space_reference(&later_local, nullptr);
			xrt_space_reference(&local_floor, nullptr);
			xrt_space_reference(&local, nullptr);
			xrt_space_overseer_destroy(&xso);
		}
	}
}

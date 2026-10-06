// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Automatic floor calibration tests.
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_session.h"
#include "xrt/xrt_tracking.h"
#include "math/m_api.h"
#include "util/u_floor_calibration.h"
#include "b_space_overseer.h"

#include "catch_amalgamated.hpp"

#include <cmath>

namespace {

constexpr int64_t kStepNs = 100 * 1000 * 1000;

struct xrt_pose
head_at(float y, float pitch_deg = 0.0f)
{
	struct xrt_pose pose = XRT_POSE_IDENTITY;
	struct xrt_vec3 axis = {1, 0, 0};
	math_quat_from_angle_vector(pitch_deg * (float)M_PI / 180.0f, &axis, &pose.orientation);
	pose.position.y = y;
	return pose;
}

/*!
 * Feed samples every 100 ms; returns the step at which the floor was found, or -1.
 */
int
feed(struct u_floor_calibration &fc, int steps, float *out_floor, struct u_floor_calibration_sample sample)
{
	for (int i = 0; i < steps; i++) {
		sample.timestamp_ns = i * kStepNs;
		if (u_floor_calibration_push(&fc, &sample, out_floor)) {
			return i;
		}
	}
	return -1;
}

} // namespace

TEST_CASE("Floor calibration waits for a worn, tracked, level and steady head")
{
	struct u_floor_calibration fc;
	float floor = 0;
	struct u_floor_calibration_sample sample = {0, true, true, head_at(1.0f)};

	SECTION("steady for a second")
	{
		u_floor_calibration_init(&fc, 1.75f);
		CHECK(feed(fc, 20, &floor, sample) == 10);
		CHECK(floor == Catch::Approx(1.0f - 1.75f));
		// Only once.
		CHECK(feed(fc, 20, &floor, sample) == -1);
	}

	SECTION("not worn, untracked or looking down never calibrates")
	{
		u_floor_calibration_init(&fc, 1.75f);
		struct u_floor_calibration_sample off = sample;
		off.worn = false;
		CHECK(feed(fc, 30, &floor, off) == -1);
		struct u_floor_calibration_sample lost = sample;
		lost.tracked = false;
		CHECK(feed(fc, 30, &floor, lost) == -1);
		struct u_floor_calibration_sample down = sample;
		down.head = head_at(1.0f, -45.0f);
		CHECK(feed(fc, 30, &floor, down) == -1);
		// A slight tilt is still level enough.
		struct u_floor_calibration_sample tilted = sample;
		tilted.head = head_at(1.0f, 15.0f);
		CHECK(feed(fc, 30, &floor, tilted) == 10);
	}

	SECTION("movement restarts the steady window")
	{
		u_floor_calibration_init(&fc, 1.6f);
		int found = -1;
		for (int i = 0; i < 40 && found < 0; i++) {
			// Moves 2 cm at 0.5 s, then holds still.
			struct u_floor_calibration_sample s = sample;
			s.timestamp_ns = i * kStepNs;
			s.head.position.y = i < 5 ? 1.0f : 1.02f;
			if (u_floor_calibration_push(&fc, &s, &floor)) {
				found = i;
			}
		}
		CHECK(found == 15);
		CHECK(floor == Catch::Approx(1.02f - 1.6f));
	}

	SECTION("losing the head restarts the window")
	{
		u_floor_calibration_init(&fc, 1.75f);
		int found = -1;
		for (int i = 0; i < 40 && found < 0; i++) {
			struct u_floor_calibration_sample s = sample;
			s.timestamp_ns = i * kStepNs;
			s.worn = i != 7;
			if (u_floor_calibration_push(&fc, &s, &floor)) {
				found = i;
			}
		}
		CHECK(found == 18);
	}
}

namespace {

struct FakeHead
{
	struct xrt_device base;
	struct xrt_tracking_origin origin;
	struct xrt_input inputs[2];
	struct xrt_pose pose;
	bool worn = true;
};

xrt_result_t
fake_get_tracked_pose(struct xrt_device *xdev, enum xrt_input_name name, int64_t, struct xrt_space_relation *out)
{
	auto *head = reinterpret_cast<FakeHead *>(xdev);
	*out = XRT_SPACE_RELATION_ZERO;
	if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	out->pose = head->pose;
	out->relation_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	return XRT_SUCCESS;
}

xrt_result_t
fake_update_inputs(struct xrt_device *xdev)
{
	auto *head = reinterpret_cast<FakeHead *>(xdev);
	head->inputs[1].value.boolean = head->worn;
	return XRT_SUCCESS;
}

void
fake_destroy(struct xrt_device *)
{}

xrt_result_t
null_push(struct xrt_session_event_sink *, const union xrt_session_event *)
{
	return XRT_SUCCESS;
}

void
init_fake_head(FakeHead &head)
{
	head.origin.type = XRT_TRACKING_TYPE_OTHER;
	head.origin.initial_offset = XRT_POSE_IDENTITY;
	head.inputs[0].active = true;
	head.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;
	head.inputs[1].active = true;
	head.inputs[1].name = XRT_INPUT_GENERIC_HEAD_DETECT;
	head.base.device_type = XRT_DEVICE_TYPE_HMD;
	head.base.tracking_origin = &head.origin;
	head.base.inputs = head.inputs;
	head.base.input_count = 2;
	head.base.supported.orientation_tracking = true;
	head.base.supported.position_tracking = true;
	head.base.supported.presence = true;
	head.base.get_tracked_pose = fake_get_tracked_pose;
	head.base.update_inputs = fake_update_inputs;
	head.base.destroy = fake_destroy;
	head.pose = head_at(0.4f);
}

} // namespace

TEST_CASE("Floor calibration sets the managed STAGE from the head")
{
	FakeHead head = {};
	init_fake_head(head);
	struct xrt_device *xdevs[] = {&head.base};

	struct xrt_session_event_sink sink = {null_push};
	struct b_space_overseer *uso = b_space_overseer_create(&sink);
	struct xrt_pose local_offset = XRT_POSE_IDENTITY;
	b_space_overseer_legacy_setup(uso, xdevs, 1, &head.base, &local_offset, false, true);
	auto *xso = reinterpret_cast<struct xrt_space_overseer *>(uso);

	struct u_floor_calibration fc;
	u_floor_calibration_init(&fc, 1.75f);

	SECTION("applied once the head is worn and steady")
	{
		head.worn = false;
		for (int i = 0; i < 15; i++) {
			CHECK_FALSE(u_floor_calibration_poll(&fc, xso, &head.base, i * kStepNs));
		}
		head.worn = true;
		bool finished = false;
		for (int i = 15; i < 40 && !finished; i++) {
			finished = u_floor_calibration_poll(&fc, xso, &head.base, i * kStepNs);
		}
		REQUIRE(finished);

		struct xrt_pose stage;
		REQUIRE(xrt_space_overseer_get_reference_space_offset(xso, XRT_SPACE_REFERENCE_TYPE_STAGE, &stage) ==
		        XRT_SUCCESS);
		CHECK(stage.position.y == Catch::Approx(0.4f - 1.75f));
	}

	SECTION("aligned: centred under the head and facing its way")
	{
		fc.align = true;
		// Off-centre, facing -X: 90 degrees about +Y turns -Z into -X.
		struct xrt_vec3 up = {0, 1, 0};
		math_quat_from_angle_vector((float)M_PI / 2.0f, &up, &head.pose.orientation);
		head.pose.position = {0.3f, 0.4f, -0.2f};
		bool finished = false;
		for (int i = 0; i < 40 && !finished; i++) {
			finished = u_floor_calibration_poll(&fc, xso, &head.base, i * kStepNs);
		}
		REQUIRE(finished);

		struct xrt_pose stage;
		REQUIRE(xrt_space_overseer_get_reference_space_offset(xso, XRT_SPACE_REFERENCE_TYPE_STAGE, &stage) ==
		        XRT_SUCCESS);
		// The head in STAGE: at the origin, at eye height, facing -Z.
		struct xrt_pose inverse, in_stage;
		math_pose_invert(&stage, &inverse);
		math_pose_transform(&inverse, &head.pose, &in_stage);
		CHECK(in_stage.position.x == Catch::Approx(0.0f).margin(1e-5));
		CHECK(in_stage.position.y == Catch::Approx(1.75f));
		CHECK(in_stage.position.z == Catch::Approx(0.0f).margin(1e-5));
		struct xrt_vec3 forward = {0, 0, -1}, facing;
		math_quat_rotate_vec3(&in_stage.orientation, &forward, &facing);
		CHECK(facing.x == Catch::Approx(0.0f).margin(1e-5));
		CHECK(facing.z == Catch::Approx(-1.0f));
	}

	SECTION("an explicit calibration is not overridden")
	{
		struct xrt_pose manual = XRT_POSE_IDENTITY;
		manual.position.y = -1.1f;
		REQUIRE(xrt_space_overseer_set_reference_space_offset(xso, XRT_SPACE_REFERENCE_TYPE_STAGE, &manual) ==
		        XRT_SUCCESS);
		bool finished = false;
		for (int i = 0; i < 40 && !finished; i++) {
			finished = u_floor_calibration_poll(&fc, xso, &head.base, i * kStepNs);
		}
		REQUIRE(finished);

		struct xrt_pose stage;
		REQUIRE(xrt_space_overseer_get_reference_space_offset(xso, XRT_SPACE_REFERENCE_TYPE_STAGE, &stage) ==
		        XRT_SUCCESS);
		CHECK(stage.position.y == Catch::Approx(-1.1f));
	}

	xrt_space_overseer_destroy(&xso);
}

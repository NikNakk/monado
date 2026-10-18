// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Qwerty devices driven through the desktop-window input bridge.
 *
 * Runs with SIMULATED_DESKTOP=1 (set by CMake), so the qwerty system registers
 * for desktop input when it is created.
 */

#include "util/u_desktop_input.h"
#include "xrt/xrt_device.h"

#include "qwerty_device.h"
#include "qwerty_interface.h"

#include "catch_amalgamated.hpp"

using Catch::Approx;

namespace {

// USB HID keyboard usages.
constexpr uint16_t key_w = 26;
constexpr uint16_t key_lctrl = 224;

struct Devices
{
	xrt_device *head = nullptr;
	xrt_device *left = nullptr;
	xrt_device *right = nullptr;

	Devices()
	{
		REQUIRE(qwerty_create_devices(U_LOGGING_WARN, &head, &left, &right) == XRT_SUCCESS);
	}
	~Devices()
	{
		xrt_device_destroy(&right);
		xrt_device_destroy(&left);
		xrt_device_destroy(&head);
	}
};

xrt_pose
pose_of(xrt_device *xd, xrt_input_name name)
{
	xrt_space_relation relation{};
	REQUIRE(xrt_device_get_tracked_pose(xd, name, 0, &relation) == XRT_SUCCESS);
	return relation.pose;
}

xrt_pose
head_pose(xrt_device *head)
{
	return pose_of(head, XRT_INPUT_GENERIC_HEAD_POSE);
}

void
push(u_desktop_input_type type, uint16_t key = 0, float x = 0, float y = 0)
{
	u_desktop_input_event event{};
	event.type = type;
	event.key = key;
	event.x = x;
	event.y = y;
	REQUIRE(u_desktop_input_push(&event));
}

void
push_button(u_desktop_input_type type, u_desktop_input_button button)
{
	u_desktop_input_event event{};
	event.type = type;
	event.button = button;
	REQUIRE(u_desktop_input_push(&event));
}

} // namespace

TEST_CASE("Qwerty registers for desktop input while it exists", "[qwerty_desktop]")
{
	{
		Devices devices;
		CHECK(u_desktop_input_active());
	}
	CHECK_FALSE(u_desktop_input_active());
}

TEST_CASE("Desktop keys and trackpad move the qwerty head", "[qwerty_desktop]")
{
	Devices devices;
	xrt_pose start = head_pose(devices.head);

	SECTION("A trackpad move is applied once, relative to the heading")
	{
		push(U_DESKTOP_INPUT_MOVE, 0, 0.25f, 0.5f);
		xrt_pose moved = head_pose(devices.head);
		CHECK(moved.position.x == Approx(start.position.x + 0.25f));
		CHECK(moved.position.z == Approx(start.position.z - 0.5f));
		xrt_pose again = head_pose(devices.head);
		CHECK(again.position.z == Approx(moved.position.z));
	}

	SECTION("W moves forward while held, and focus loss stops it")
	{
		push(U_DESKTOP_INPUT_KEY_DOWN, key_w);
		float z1 = head_pose(devices.head).position.z;
		float z2 = head_pose(devices.head).position.z;
		CHECK(z1 < start.position.z);
		CHECK(z2 < z1);
		push(U_DESKTOP_INPUT_FOCUS_LOST);
		float z3 = head_pose(devices.head).position.z;
		CHECK(head_pose(devices.head).position.z == Approx(z3));
	}

	SECTION("Pointer right turns right")
	{
		push(U_DESKTOP_INPUT_LOOK, 0, 100.0f, 0.0f);
		xrt_quat q = head_pose(devices.head).orientation;
		// Yaw about +Y: a right turn is a negative rotation, so q.y < 0.
		CHECK(q.y < 0.0f);
	}
}

TEST_CASE("Desktop modifiers and clicks reach the controllers", "[qwerty_desktop]")
{
	Devices devices;
	xrt_pose head_start = head_pose(devices.head);
	xrt_pose left_start = pose_of(devices.left, XRT_INPUT_WMR_GRIP_POSE);

	// Hold left control: the move goes to the left controller, not the head.
	push(U_DESKTOP_INPUT_KEY_DOWN, key_lctrl);
	push(U_DESKTOP_INPUT_MOVE, 0, 0.0f, 0.1f);
	xrt_pose left_moved = pose_of(devices.left, XRT_INPUT_WMR_GRIP_POSE);
	CHECK(left_moved.position.z == Approx(left_start.position.z - 0.1f));
	CHECK(head_pose(devices.head).position.z == Approx(head_start.position.z));

	// A click with control held is the left controller's trigger.
	push_button(U_DESKTOP_INPUT_BUTTON_DOWN, U_DESKTOP_INPUT_BUTTON_LEFT);
	CHECK(qwerty_controller(devices.left)->trigger_clicked);
	CHECK_FALSE(qwerty_controller(devices.right)->trigger_clicked);
	push(U_DESKTOP_INPUT_KEY_UP, key_lctrl);

	// Without a modifier, a secondary click squeezes the right controller.
	push_button(U_DESKTOP_INPUT_BUTTON_DOWN, U_DESKTOP_INPUT_BUTTON_RIGHT);
	CHECK(qwerty_controller(devices.right)->squeeze_clicked);

	// Focus loss releases every button.
	push(U_DESKTOP_INPUT_FOCUS_LOST);
	CHECK_FALSE(qwerty_controller(devices.left)->trigger_clicked);
	CHECK_FALSE(qwerty_controller(devices.right)->squeeze_clicked);
}

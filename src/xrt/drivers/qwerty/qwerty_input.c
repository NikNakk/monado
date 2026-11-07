// Copyright 2021, Mateo de Mayo.
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Keyboard and pointer actions on qwerty devices, independent of the window system.
 * @author Mateo de Mayo <mateodemayo@gmail.com>
 * @ingroup drv_qwerty
 */

#include "qwerty_device.h"
#include "util/u_desktop_input.h"

// Amount of look_speed units a pointer delta of 1px in screen space will rotate the device
#define SENSITIVITY 0.1f

// USB HID keyboard usages (page 0x07), the same values as SDL scancodes.
enum
{
	KEY_A = 4,
	KEY_B = 5,
	KEY_C = 6,
	KEY_D = 7,
	KEY_E = 8,
	KEY_F = 9,
	KEY_G = 10,
	KEY_H = 11,
	KEY_I = 12,
	KEY_J = 13,
	KEY_K = 14,
	KEY_L = 15,
	KEY_M = 16,
	KEY_N = 17,
	KEY_Q = 20,
	KEY_R = 21,
	KEY_S = 22,
	KEY_T = 23,
	KEY_V = 25,
	KEY_W = 26,
	KEY_RIGHT = 79,
	KEY_LEFT = 80,
	KEY_DOWN = 81,
	KEY_UP = 82,
	KEY_KP_MINUS = 86,
	KEY_KP_PLUS = 87,
	KEY_LCTRL = 224,
	KEY_LSHIFT = 225,
	KEY_LALT = 226,
};

static void
release_controller_buttons(struct qwerty_controller *qc)
{
	qwerty_release_trigger(qc);
	qwerty_release_menu(qc);
	qwerty_release_squeeze(qc);
	qwerty_release_system(qc);
	qwerty_release_thumbstick_left(qc);
	qwerty_release_thumbstick_right(qc);
	qwerty_release_thumbstick_up(qc);
	qwerty_release_thumbstick_down(qc);
	qwerty_release_thumbstick_click(qc);
	qwerty_release_trackpad_left(qc);
	qwerty_release_trackpad_right(qc);
	qwerty_release_trackpad_up(qc);
	qwerty_release_trackpad_down(qc);
	qwerty_release_trackpad_click(qc);
}

void
qwerty_process_input(struct qwerty_system *qsys,
                     struct qwerty_device *default_qdev,
                     struct qwerty_controller *default_qctrl,
                     const struct u_desktop_input_event *event)
{
	if (!qsys->process_keys) {
		return;
	}

	// Initialize different views of the same pointers.

	struct qwerty_controller *qleft = qsys->lctrl;
	struct qwerty_device *qd_left = &qleft->base;

	struct qwerty_controller *qright = qsys->rctrl;
	struct qwerty_device *qd_right = &qright->base;

	bool using_qhmd = qsys->hmd != NULL;
	struct qwerty_hmd *qhmd = using_qhmd ? qsys->hmd : NULL;
	struct qwerty_device *qd_hmd = using_qhmd ? &qhmd->base : NULL;

	if (event->type == U_DESKTOP_INPUT_FOCUS_LOST) {
		qsys->ctrl_pressed = false;
		qsys->alt_pressed = false;
		if (using_qhmd)
			qwerty_release_all(qd_hmd);
		qwerty_release_all(qd_right);
		qwerty_release_all(qd_left);
		release_controller_buttons(qright);
		release_controller_buttons(qleft);
		return;
	}

	bool key_down = event->type == U_DESKTOP_INPUT_KEY_DOWN;
	bool key_up = event->type == U_DESKTOP_INPUT_KEY_UP;
	uint16_t key = event->key;
	bool button_down = event->type == U_DESKTOP_INPUT_BUTTON_DOWN;
	bool button_up = event->type == U_DESKTOP_INPUT_BUTTON_UP;

	// clang-format off
	// CTRL/ALT keys logic
	bool alt_down = key_down && key == KEY_LALT;
	bool alt_up = key_up && key == KEY_LALT;
	bool ctrl_down = key_down && key == KEY_LCTRL;
	bool ctrl_up = key_up && key == KEY_LCTRL;
	if (alt_down) qsys->alt_pressed = true;
	if (alt_up) qsys->alt_pressed = false;
	if (ctrl_down) qsys->ctrl_pressed = true;
	if (ctrl_up) qsys->ctrl_pressed = false;

	bool change_focus = alt_down || alt_up || ctrl_down || ctrl_up;
	if (change_focus) {
		if (using_qhmd) qwerty_release_all(qd_hmd);
		qwerty_release_all(qd_right);
		qwerty_release_all(qd_left);
	}

	// Determine focused device
	struct qwerty_device *qdev;
	if (qsys->ctrl_pressed) qdev = qd_left;
	else if (qsys->alt_pressed) qdev = qd_right;
	else qdev = default_qdev;

	// Determine focused controller for qwerty_controller specific methods
	struct qwerty_controller *qctrl = qdev != qd_hmd ? qwerty_controller(&qdev->base) : default_qctrl;

	// Update gui tracked variables
	qsys->hmd_focused = qdev == qd_hmd;
	qsys->lctrl_focused = qdev == qd_left;
	qsys->rctrl_focused = qdev == qd_right;

	// WASDQE Movement
	if (key_down && key == KEY_A) qwerty_press_left(qdev);
	if (key_up && key == KEY_A) qwerty_release_left(qdev);
	if (key_down && key == KEY_D) qwerty_press_right(qdev);
	if (key_up && key == KEY_D) qwerty_release_right(qdev);
	if (key_down && key == KEY_W) qwerty_press_forward(qdev);
	if (key_up && key == KEY_W) qwerty_release_forward(qdev);
	if (key_down && key == KEY_S) qwerty_press_backward(qdev);
	if (key_up && key == KEY_S) qwerty_release_backward(qdev);
	if (key_down && key == KEY_E) qwerty_press_up(qdev);
	if (key_up && key == KEY_E) qwerty_release_up(qdev);
	if (key_down && key == KEY_Q) qwerty_press_down(qdev);
	if (key_up && key == KEY_Q) qwerty_release_down(qdev);

	// Arrow keys rotation
	if (key_down && key == KEY_LEFT) qwerty_press_look_left(qdev);
	if (key_up && key == KEY_LEFT) qwerty_release_look_left(qdev);
	if (key_down && key == KEY_RIGHT) qwerty_press_look_right(qdev);
	if (key_up && key == KEY_RIGHT) qwerty_release_look_right(qdev);
	if (key_down && key == KEY_UP) qwerty_press_look_up(qdev);
	if (key_up && key == KEY_UP) qwerty_release_look_up(qdev);
	if (key_down && key == KEY_DOWN) qwerty_press_look_down(qdev);
	if (key_up && key == KEY_DOWN) qwerty_release_look_down(qdev);

	// Movement speed
	if (event->type == U_DESKTOP_INPUT_WHEEL) qwerty_change_movement_speed(qdev, event->y);
	if (key_down && key == KEY_KP_PLUS) qwerty_change_movement_speed(qdev, 1);
	if (key_down && key == KEY_KP_MINUS) qwerty_change_movement_speed(qdev, -1);

	// Sprinting
	if (key_down && key == KEY_LSHIFT) qwerty_press_sprint(qdev);
	if (key_up && key == KEY_LSHIFT) qwerty_release_sprint(qdev);

	// Pointer rotation and trackpad movement
	if (event->type == U_DESKTOP_INPUT_LOOK) qwerty_add_look_delta(qdev, -event->x * SENSITIVITY, -event->y * SENSITIVITY);
	if (event->type == U_DESKTOP_INPUT_MOVE) qwerty_add_move_delta(qdev, event->x, event->y);

	// Trigger, squeeze and menu clicks only for controllers.
	// SDL holds its right button to look, so it never sends it; a desktop window uses it for squeeze.
	bool squeeze_button = event->button == U_DESKTOP_INPUT_BUTTON_MIDDLE || event->button == U_DESKTOP_INPUT_BUTTON_RIGHT;
	if (button_down && event->button == U_DESKTOP_INPUT_BUTTON_LEFT) qwerty_press_trigger(qctrl);
	if (button_up && event->button == U_DESKTOP_INPUT_BUTTON_LEFT) qwerty_release_trigger(qctrl);
	if (button_down && squeeze_button) qwerty_press_squeeze(qctrl);
	if (button_up && squeeze_button) qwerty_release_squeeze(qctrl);

	if (key_down && key == KEY_N) qwerty_press_menu(qctrl);
	if (key_up && key == KEY_N) qwerty_release_menu(qctrl);
	if (key_down && key == KEY_B) qwerty_press_system(qctrl);
	if (key_up && key == KEY_B) qwerty_release_system(qctrl);

	// Thumbstick
	if (key_down && key == KEY_F) qwerty_press_thumbstick_left(qctrl);
	if (key_up && key == KEY_F) qwerty_release_thumbstick_left(qctrl);
	if (key_down && key == KEY_H) qwerty_press_thumbstick_right(qctrl);
	if (key_up && key == KEY_H) qwerty_release_thumbstick_right(qctrl);
	if (key_down && key == KEY_T) qwerty_press_thumbstick_up(qctrl);
	if (key_up && key == KEY_T) qwerty_release_thumbstick_up(qctrl);
	if (key_down && key == KEY_G) qwerty_press_thumbstick_down(qctrl);
	if (key_up && key == KEY_G) qwerty_release_thumbstick_down(qctrl);
	if (key_down && key == KEY_V) qwerty_press_thumbstick_click(qctrl);
	if (key_up && key == KEY_V) qwerty_release_thumbstick_click(qctrl);

	// Trackpad
	if (key_down && key == KEY_J) qwerty_press_trackpad_left(qctrl);
	if (key_up && key == KEY_J) qwerty_release_trackpad_left(qctrl);
	if (key_down && key == KEY_L) qwerty_press_trackpad_right(qctrl);
	if (key_up && key == KEY_L) qwerty_release_trackpad_right(qctrl);
	if (key_down && key == KEY_I) qwerty_press_trackpad_up(qctrl);
	if (key_up && key == KEY_I) qwerty_release_trackpad_up(qctrl);
	if (key_down && key == KEY_K) qwerty_press_trackpad_down(qctrl);
	if (key_up && key == KEY_K) qwerty_release_trackpad_down(qctrl);
	if (key_down && key == KEY_M) qwerty_press_trackpad_click(qctrl);
	if (key_up && key == KEY_M) qwerty_release_trackpad_click(qctrl);

	// clang-format on

	// Controllers follow/unfollow HMD
	if (key_down && key == KEY_C && !event->repeat) {
		if (qdev != qd_hmd) {
			qwerty_follow_hmd(qctrl, !qctrl->follow_hmd);
		} else { // If no controller is focused, set both to the same state
			bool both_not_following = !qleft->follow_hmd && !qright->follow_hmd;
			qwerty_follow_hmd(qleft, both_not_following);
			qwerty_follow_hmd(qright, both_not_following);
		}
	}

	// Reset controller poses
	if (key_down && key == KEY_R && !event->repeat) {
		if (qdev != qd_hmd) {
			qwerty_reset_controller_pose(qctrl);
		} else { // If no controller is focused, reset both
			qwerty_reset_controller_pose(qleft);
			qwerty_reset_controller_pose(qright);
		}
	}
}

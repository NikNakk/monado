// Copyright 2021, Mateo de Mayo.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Connection between user-generated SDL events and qwerty devices.
 * @author Mateo de Mayo <mateodemayo@gmail.com>
 * @ingroup drv_qwerty
 */

#include "qwerty_device.h"
#include "util/u_device.h"
#include "util/u_desktop_input.h"
#include "xrt/xrt_device.h"
#include <SDL2/SDL.h>
#include <SDL_scancode.h>
#include <assert.h>
#include <string.h>

static struct qwerty_system *
find_qwerty_system(struct xrt_device **xdevs, size_t xdev_count)
{
	struct xrt_device *xdev = NULL;
	for (size_t i = 0; i < xdev_count; i++) {
		if (xdevs[i] == NULL) {
			continue;
		}
		// We check against tracker name instead of device name because the tracking overrides
		// cause the multi device to have the same names even though they are not qwerty devices.
		const char *tracker_name = xdevs[i]->tracking_origin->name;
		if (strcmp(tracker_name, QWERTY_HMD_TRACKER_STR) == 0 ||
		    strcmp(tracker_name, QWERTY_LEFT_TRACKER_STR) == 0 ||
		    strcmp(tracker_name, QWERTY_RIGHT_TRACKER_STR) == 0) {
			xdev = xdevs[i];
			break;
		}
	}

	assert(xdev != NULL && "There is no device in xdevs with the name of a qwerty device");
	struct qwerty_device *qdev = qwerty_device(xdev);
	struct qwerty_system *qsys = qdev->sys;
	assert(qsys != NULL && "The qwerty_system of a qwerty_device was null");
	return qsys;
}

// Determines the default qwerty device based on which devices are in use
static struct qwerty_device *
default_qwerty_device(struct xrt_device **xdevs, size_t xdev_count, struct qwerty_system *qsys)
{
	int head;
	int eyes;
	int face;
	int left;
	int right;
	int gamepad;
	head = eyes = face = left = right = gamepad = XRT_DEVICE_ROLE_UNASSIGNED;
	u_device_assign_xdev_roles(xdevs, xdev_count, &head, &eyes, &face, &left, &right, &gamepad);

	struct xrt_device *xd_hmd = qsys->hmd ? &qsys->hmd->base.base : NULL;
	struct xrt_device *xd_left = &qsys->lctrl->base.base;
	struct xrt_device *xd_right = &qsys->rctrl->base.base;

	struct qwerty_device *default_qdev = NULL;
	if (xdevs[head] == xd_hmd) {
		default_qdev = qwerty_device(xd_hmd);
	} else if (xdevs[right] == xd_right) {
		default_qdev = qwerty_device(xd_right);
	} else if (xdevs[left] == xd_left) {
		default_qdev = qwerty_device(xd_left);
	} else { // Even here, xd_right is allocated and so we can modify it
		default_qdev = qwerty_device(xd_right);
	}

	return default_qdev;
}

// Determines the default qwerty controller based on which devices are in use
static struct qwerty_controller *
default_qwerty_controller(struct xrt_device **xdevs, size_t xdev_count, struct qwerty_system *qsys)
{
	int head;
	int eyes;
	int face;
	int left;
	int right;
	int gamepad;
	head = eyes = face = left = right = gamepad = XRT_DEVICE_ROLE_UNASSIGNED;
	u_device_assign_xdev_roles(xdevs, xdev_count, &head, &eyes, &face, &left, &right, &gamepad);

	struct xrt_device *xd_left = &qsys->lctrl->base.base;
	struct xrt_device *xd_right = &qsys->rctrl->base.base;

	struct qwerty_controller *default_qctrl = NULL;
	if (xdevs[right] == xd_right) {
		default_qctrl = qwerty_controller(xd_right);
	} else if (xdevs[left] == xd_left) {
		default_qctrl = qwerty_controller(xd_left);
	} else { // Even here, xd_right is allocated and so we can modify it
		default_qctrl = qwerty_controller(xd_right);
	}

	return default_qctrl;
}

// The desktop input keys are USB HID usages, which is what SDL scancodes are.
static_assert(SDL_SCANCODE_A == 4 && SDL_SCANCODE_LALT == 226, "SDL scancodes are not USB HID usages");

void
qwerty_process_event(struct xrt_device **xdevs, size_t xdev_count, SDL_Event event)
{
	static struct qwerty_system *qsys = NULL;

	// Default focused device: the one focused when CTRL and ALT are not pressed
	static struct qwerty_device *default_qdev;
	// Default focused controller: the one used for qwerty_controller specific methods
	static struct qwerty_controller *default_qctrl;

	// We can cache the devices as they don't get destroyed during runtime
	static bool cached = false;
	if (!cached) {
		qsys = find_qwerty_system(xdevs, xdev_count);
		default_qdev = default_qwerty_device(xdevs, xdev_count, qsys);
		default_qctrl = default_qwerty_controller(xdevs, xdev_count, qsys);
		cached = true;
	}

	if (!qsys->process_keys) {
		return;
	}

	struct u_desktop_input_event input = {0};
	switch (event.type) {
	case SDL_KEYDOWN:
	case SDL_KEYUP:
		input.type = event.type == SDL_KEYDOWN ? U_DESKTOP_INPUT_KEY_DOWN : U_DESKTOP_INPUT_KEY_UP;
		input.key = (uint16_t)event.key.keysym.scancode;
		input.repeat = event.key.repeat != 0;
		break;
	case SDL_MOUSEWHEEL:
		input.type = U_DESKTOP_INPUT_WHEEL;
		input.y = (float)event.wheel.y;
		break;
	case SDL_MOUSEMOTION:
		// Mouse rotation while the right button is held
		if ((event.motion.state & SDL_BUTTON_RMASK) == 0) {
			return;
		}
		SDL_SetRelativeMouseMode(true);
		input.type = U_DESKTOP_INPUT_LOOK;
		input.x = (float)event.motion.xrel;
		input.y = (float)event.motion.yrel;
		break;
	case SDL_MOUSEBUTTONDOWN:
	case SDL_MOUSEBUTTONUP:
		if (event.button.button == SDL_BUTTON_RIGHT) {
			// The right button only looks; it is not a controller button here.
			if (event.type == SDL_MOUSEBUTTONUP) {
				SDL_SetRelativeMouseMode(false);
			}
			return;
		}
		if (event.button.button != SDL_BUTTON_LEFT && event.button.button != SDL_BUTTON_MIDDLE) {
			return;
		}
		input.type =
		    event.type == SDL_MOUSEBUTTONDOWN ? U_DESKTOP_INPUT_BUTTON_DOWN : U_DESKTOP_INPUT_BUTTON_UP;
		input.button = event.button.button == SDL_BUTTON_LEFT ? U_DESKTOP_INPUT_BUTTON_LEFT
		                                                      : U_DESKTOP_INPUT_BUTTON_MIDDLE;
		break;
	default: return;
	}

	qwerty_process_input(qsys, default_qdev, default_qctrl, &input);
}

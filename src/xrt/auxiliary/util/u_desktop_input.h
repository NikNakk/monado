// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Keyboard and pointer input from a desktop window, for emulated devices.
 * @ingroup aux_util
 *
 * The window that shows the compositor's output (the macOS presenter's
 * SIMULATED_DESKTOP window today) pushes events here, and a driver that
 * emulates devices from them (qwerty) registers to receive them. Neither side
 * links against the other.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum u_desktop_input_type
{
	U_DESKTOP_INPUT_KEY_DOWN,
	U_DESKTOP_INPUT_KEY_UP,
	//! Turn by a pointer movement, in points: x right, y down.
	U_DESKTOP_INPUT_LOOK,
	//! Move across the floor relative to the heading, in metres: x right, y forward.
	U_DESKTOP_INPUT_MOVE,
	//! Mouse wheel steps, y up.
	U_DESKTOP_INPUT_WHEEL,
	U_DESKTOP_INPUT_BUTTON_DOWN,
	U_DESKTOP_INPUT_BUTTON_UP,
	//! The window lost keyboard focus: treat every key and button as released.
	U_DESKTOP_INPUT_FOCUS_LOST,
};

enum u_desktop_input_button
{
	U_DESKTOP_INPUT_BUTTON_LEFT,
	U_DESKTOP_INPUT_BUTTON_MIDDLE,
	U_DESKTOP_INPUT_BUTTON_RIGHT,
};

struct u_desktop_input_event
{
	enum u_desktop_input_type type;
	//! Key events: USB HID keyboard usage (page 0x07), the values SDL uses for scancodes.
	uint16_t key;
	//! Key events: auto-repeat rather than a new press.
	bool repeat;
	//! Button events.
	enum u_desktop_input_button button;
	//! Look, move and wheel events.
	float x, y;
};

typedef void (*u_desktop_input_handler_t)(void *ctx, const struct u_desktop_input_event *event);

//! Receive the window's events. One handler at a time.
void
u_desktop_input_register(u_desktop_input_handler_t handler, void *ctx);

//! Stop receiving events; waits for a call in progress.
void
u_desktop_input_unregister(void *ctx);

//! True while a handler is registered.
bool
u_desktop_input_active(void);

//! Deliver an event to the handler, if any. Returns true if one took it.
bool
u_desktop_input_push(const struct u_desktop_input_event *event);

#ifdef __cplusplus
}
#endif

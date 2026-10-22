// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Keyboard and pointer input from a desktop window, for emulated devices.
 * @ingroup aux_util
 */

#include "util/u_desktop_input.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static u_desktop_input_handler_t g_handler = NULL;
static void *g_ctx = NULL;

/*
 * Scripted input for unattended runs.
 *
 * QWERTY_SCRIPT is a comma-separated list of events, each "<seconds><+|-><name>":
 * seconds after the first handler registers, + to press and - to release, and a
 * name of lctrl, lshift, lalt, left, middle, right or a USB HID key usage. For
 * example "40+lctrl,40.1+left,40.6-left,40.7-lctrl" clicks the left
 * controller's trigger at 40 seconds. QWERTY_SCRIPT_REPEAT, in seconds, repeats
 * the whole list with that period.
 */

struct script_event
{
	double time;
	struct u_desktop_input_event event;
};

static bool
parse_script_event(const char *token, struct script_event *out)
{
	char *end = NULL;
	double time = strtod(token, &end);
	if (end == token || (*end != '+' && *end != '-')) {
		return false;
	}
	bool down = *end == '+';
	const char *name = end + 1;

	struct u_desktop_input_event event = {0};
	enum u_desktop_input_button button;
	bool is_button = true;
	if (strcmp(name, "left") == 0) {
		button = U_DESKTOP_INPUT_BUTTON_LEFT;
	} else if (strcmp(name, "middle") == 0) {
		button = U_DESKTOP_INPUT_BUTTON_MIDDLE;
	} else if (strcmp(name, "right") == 0) {
		button = U_DESKTOP_INPUT_BUTTON_RIGHT;
	} else {
		is_button = false;
	}

	if (is_button) {
		event.type = down ? U_DESKTOP_INPUT_BUTTON_DOWN : U_DESKTOP_INPUT_BUTTON_UP;
		event.button = button;
	} else {
		long key;
		if (strcmp(name, "lctrl") == 0) {
			key = 224;
		} else if (strcmp(name, "lshift") == 0) {
			key = 225;
		} else if (strcmp(name, "lalt") == 0) {
			key = 226;
		} else {
			char *key_end = NULL;
			key = strtol(name, &key_end, 0);
			if (key_end == name || *key_end != '\0' || key <= 0 || key > 0xFFFF) {
				return false;
			}
		}
		event.type = down ? U_DESKTOP_INPUT_KEY_DOWN : U_DESKTOP_INPUT_KEY_UP;
		event.key = (uint16_t)key;
	}

	out->time = time;
	out->event = event;
	return true;
}

static void *
script_thread(void *data)
{
	char *script = data;
	struct script_event events[64];
	size_t count = 0;
	char *save = NULL;
	for (char *token = strtok_r(script, ",", &save); token != NULL && count < 64;
	     token = strtok_r(NULL, ",", &save)) {
		while (*token == ' ') {
			token++;
		}
		if (!parse_script_event(token, &events[count])) {
			U_LOG_W("QWERTY_SCRIPT: ignoring '%s'", token);
			continue;
		}
		count++;
	}
	free(script);
	if (count == 0) {
		return NULL;
	}

	const char *repeat_str = getenv("QWERTY_SCRIPT_REPEAT");
	double repeat = repeat_str != NULL ? strtod(repeat_str, NULL) : 0.0;
	U_LOG_I("QWERTY_SCRIPT: %zu events, repeat every %.1f s", count, repeat);

	int64_t start = os_monotonic_get_ns();
	for (int round = 0;; round++) {
		double offset = repeat > 0.0 ? repeat * round : 0.0;
		for (size_t i = 0; i < count; i++) {
			int64_t due = start + (int64_t)((events[i].time + offset) * 1e9);
			int64_t now = os_monotonic_get_ns();
			if (due > now) {
				os_nanosleep(due - now);
			}
			if (!u_desktop_input_push(&events[i].event)) {
				U_LOG_W("QWERTY_SCRIPT: no handler for event %zu", i);
			}
		}
		if (repeat <= 0.0) {
			break;
		}
	}
	return NULL;
}

static void
start_script(void)
{
	static bool started = false;
	if (started) {
		return;
	}
	started = true;

	const char *script = getenv("QWERTY_SCRIPT");
	if (script == NULL || script[0] == '\0') {
		return;
	}
	char *copy = strdup(script);
	if (copy == NULL) {
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, script_thread, copy) != 0) {
		free(copy);
		return;
	}
	pthread_detach(thread);
}

void
u_desktop_input_register(u_desktop_input_handler_t handler, void *ctx)
{
	pthread_mutex_lock(&g_mutex);
	g_handler = handler;
	g_ctx = ctx;
	start_script();
	pthread_mutex_unlock(&g_mutex);
}

void
u_desktop_input_unregister(void *ctx)
{
	pthread_mutex_lock(&g_mutex);
	if (g_ctx == ctx) {
		g_handler = NULL;
		g_ctx = NULL;
	}
	pthread_mutex_unlock(&g_mutex);
}

bool
u_desktop_input_active(void)
{
	pthread_mutex_lock(&g_mutex);
	bool active = g_handler != NULL;
	pthread_mutex_unlock(&g_mutex);
	return active;
}

bool
u_desktop_input_push(const struct u_desktop_input_event *event)
{
	// The handler runs with the lock held, so it cannot be unregistered mid-call.
	pthread_mutex_lock(&g_mutex);
	bool handled = g_handler != NULL;
	if (handled) {
		g_handler(g_ctx, event);
	}
	pthread_mutex_unlock(&g_mutex);
	return handled;
}

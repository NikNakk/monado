// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/* Bounded live input and 3DoF diagnostic for PS VR2 Sense controllers. */

#include "cli_common.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"

#include "os/os_time.h"
#include "util/u_time.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>


static struct xrt_input *
find_input(struct xrt_device *xdev, enum xrt_input_name name)
{
	for (size_t i = 0; i < xdev->input_count; i++) {
		if (xdev->inputs[i].name == name) {
			return &xdev->inputs[i];
		}
	}
	return NULL;
}

static void
destroy_system(struct xrt_instance **xi,
               struct xrt_system **xsys,
               struct xrt_system_devices **xsysd,
               struct xrt_space_overseer **xso)
{
	xrt_space_overseer_destroy(xso);
	xrt_system_devices_destroy(xsysd);
	xrt_system_destroy(xsys);
	xrt_instance_destroy(xi);
}

static bool
parse_duration(int argc, const char **argv, uint32_t *out_duration_s)
{
	*out_duration_s = 15;
	if (argc == 2) {
		return true;
	}
	if (argc != 3) {
		return false;
	}

	errno = 0;
	char *end = NULL;
	long value = strtol(argv[2], &end, 10);
	if (errno != 0 || end == argv[2] || *end != '\0' || value < 1 || value > 120) {
		return false;
	}
	*out_duration_s = (uint32_t)value;
	return true;
}

struct controller_result
{
	bool saw_orientation;
	bool saw_motion;
	bool saw_stick;
	bool saw_trigger;
	bool saw_squeeze;
	bool saw_button;
	bool saw_battery;
	bool have_first_orientation;
	struct xrt_quat first_orientation;
};

static void
sample_controller(struct xrt_device *xdev, struct controller_result *result, int64_t now_ns, bool print)
{
	xrt_device_update_inputs(xdev);

	struct xrt_input *stick = find_input(xdev, XRT_INPUT_PSSENSE_THUMBSTICK);
	struct xrt_input *trigger = find_input(xdev, XRT_INPUT_PSSENSE_TRIGGER_VALUE);
	struct xrt_input *squeeze = find_input(xdev, XRT_INPUT_PSSENSE_SQUEEZE_PROXIMITY_FLOAT);
	struct xrt_input *ps = find_input(xdev, XRT_INPUT_PSSENSE_PS_CLICK);
	struct xrt_input *face = find_input(xdev, xdev->device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER
	                                             ? XRT_INPUT_PSSENSE_SQUARE_CLICK
	                                             : XRT_INPUT_PSSENSE_CROSS_CLICK);

	float stick_x = stick != NULL ? stick->value.vec2.x : 0.0f;
	float stick_y = stick != NULL ? stick->value.vec2.y : 0.0f;
	float trigger_value = trigger != NULL ? trigger->value.vec1.x : 0.0f;
	float squeeze_value = squeeze != NULL ? squeeze->value.vec1.x : 0.0f;
	bool ps_pressed = ps != NULL && ps->value.boolean;
	bool face_pressed = face != NULL && face->value.boolean;
	result->saw_stick |= fabsf(stick_x) > 0.15f || fabsf(stick_y) > 0.15f;
	result->saw_trigger |= trigger_value > 0.10f;
	result->saw_squeeze |= squeeze_value > 0.10f;
	result->saw_button |= ps_pressed || face_pressed;

	struct xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
	xrt_result_t xret = xrt_device_get_tracked_pose(xdev, XRT_INPUT_PSSENSE_AIM_POSE, now_ns, &relation);
	if (xret == XRT_SUCCESS &&
	    (relation.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0) {
		result->saw_orientation = true;
		if (!result->have_first_orientation) {
			result->first_orientation = relation.pose.orientation;
			result->have_first_orientation = true;
		} else {
			struct xrt_quat *a = &result->first_orientation;
			struct xrt_quat *b = &relation.pose.orientation;
			float dot = fabsf(a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w);
			result->saw_motion |= dot < 0.9999f;
		}
	}

	bool battery_present = false;
	bool charging = false;
	float charge = 0.0f;
	if (xrt_device_get_battery_status(xdev, &battery_present, &charging, &charge) == XRT_SUCCESS) {
		result->saw_battery |= battery_present;
	}

	if (print) {
		printf("%s stick=%+.2f,%+.2f trigger=%.2f squeeze=%.2f ps=%d face=%d battery=%s%.0f%% "
		       "flags=0x%02x quat=%+.3f,%+.3f,%+.3f,%+.3f\n",
		       xdev->device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER ? "L" : "R", stick_x, stick_y,
		       trigger_value, squeeze_value, ps_pressed, face_pressed, battery_present ? "" : "n/a ",
		       battery_present ? charge * 100.0f : 0.0f, (unsigned)relation.relation_flags,
		       relation.pose.orientation.x, relation.pose.orientation.y, relation.pose.orientation.z,
		       relation.pose.orientation.w);
	}
}

int
cli_cmd_pssense_test(int argc, const char **argv)
{
	uint32_t duration_s = 0;
	if (!parse_duration(argc, argv, &duration_s)) {
		fprintf(stderr, "Usage: %s %s [duration-seconds: 1-120]\n", argv[0], argv[1]);
		return EXIT_FAILURE;
	}

	struct xrt_instance *xi = NULL;
	struct xrt_system *xsys = NULL;
	struct xrt_system_devices *xsysd = NULL;
	struct xrt_space_overseer *xso = NULL;
	xrt_result_t xret = xrt_instance_create(NULL, &xi);
	if (xret != XRT_SUCCESS ||
	    (xret = xrt_instance_create_system(xi, &xsys, &xsysd, &xso, NULL)) != XRT_SUCCESS) {
		fprintf(stderr, "Failed to create Monado system: %d\n", xret);
		destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	struct xrt_device *controllers[2] = {NULL, NULL};
	for (size_t i = 0; i < xsysd->static_xdev_count; i++) {
		struct xrt_device *xdev = xsysd->static_xdevs[i];
		if (xdev->name != XRT_DEVICE_PSSENSE) {
			continue;
		}
		if (xdev->device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER) {
			controllers[0] = xdev;
		} else if (xdev->device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER) {
			controllers[1] = xdev;
		}
	}
	if (controllers[0] == NULL || controllers[1] == NULL) {
		fprintf(stderr, "Both left and right PS Sense controllers are required.\n");
		destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	printf("Move both controllers, sweep both sticks/triggers/grips, and press Square/Cross or PS during %u seconds.\n",
	       duration_s);
	struct controller_result results[2] = {0};
	int64_t start_ns = os_monotonic_get_ns();
	int64_t end_ns = start_ns + (int64_t)duration_s * U_TIME_1S_IN_NS;
	int64_t next_print_ns = start_ns;
	while (os_monotonic_get_ns() < end_ns) {
		int64_t now_ns = os_monotonic_get_ns();
		bool print = now_ns >= next_print_ns;
		for (size_t i = 0; i < 2; i++) {
			sample_controller(controllers[i], &results[i], now_ns, print);
		}
		if (print) {
			fflush(stdout);
			next_print_ns += U_TIME_1S_IN_NS / 5;
		}
		os_nanosleep(U_TIME_1S_IN_NS / 60);
	}

	bool pass = true;
	for (size_t i = 0; i < 2; i++) {
		printf("%s summary: orientation=%s motion=%s stick=%s trigger=%s squeeze=%s button=%s battery=%s\n",
		       i == 0 ? "L" : "R",
		       results[i].saw_orientation ? "yes" : "no", results[i].saw_motion ? "yes" : "no",
		       results[i].saw_stick ? "yes" : "no", results[i].saw_trigger ? "yes" : "no",
		       results[i].saw_squeeze ? "yes" : "no", results[i].saw_button ? "yes" : "no",
		       results[i].saw_battery ? "yes" : "no");
		pass &= results[i].saw_orientation && results[i].saw_motion && results[i].saw_stick &&
		        results[i].saw_trigger && results[i].saw_squeeze && results[i].saw_button &&
		        results[i].saw_battery;
	}
	destroy_system(&xi, &xsys, &xsysd, &xso);
	printf("%s: live Sense input and 3DoF probe.\n", pass ? "PASS" : "FAIL");
	return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}

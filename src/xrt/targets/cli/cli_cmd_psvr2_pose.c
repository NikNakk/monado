// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Hardware-backed PS VR2 discovery and pose probe.
 */

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
	*out_duration_s = 10;
	if (argc == 2) {
		return true;
	}
	if (argc != 3) {
		return false;
	}

	errno = 0;
	char *end = NULL;
	long duration_s = strtol(argv[2], &end, 10);
	if (errno != 0 || end == argv[2] || *end != '\0' || duration_s < 1 || duration_s > 120) {
		return false;
	}

	*out_duration_s = (uint32_t)duration_s;
	return true;
}

static bool
pose_changed(const struct xrt_pose *first, const struct xrt_pose *current)
{
	double dx = (double)current->position.x - first->position.x;
	double dy = (double)current->position.y - first->position.y;
	double dz = (double)current->position.z - first->position.z;
	double distance_squared = dx * dx + dy * dy + dz * dz;

	double orientation_dot =
	    fabs((double)current->orientation.x * first->orientation.x +
	         (double)current->orientation.y * first->orientation.y +
	         (double)current->orientation.z * first->orientation.z +
	         (double)current->orientation.w * first->orientation.w);
	if (orientation_dot > 1.0) {
		orientation_dot = 1.0;
	}

	// One millimetre of translation or roughly half a degree of rotation.
	return distance_squared > 0.000001 || (1.0 - orientation_dot) > 0.00001;
}

int
cli_cmd_psvr2_pose(int argc, const char **argv)
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
	if (xret != XRT_SUCCESS) {
		fprintf(stderr, "Failed to create Monado instance: %d\n", xret);
		return EXIT_FAILURE;
	}

	xret = xrt_instance_create_system(xi, &xsys, &xsysd, &xso, NULL);
	if (xret != XRT_SUCCESS) {
		fprintf(stderr, "Failed to create system devices: %d\n", xret);
		destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	struct xrt_device *head = xsysd != NULL ? xsysd->static_roles.head : NULL;
	if (head == NULL) {
		fprintf(stderr, "No head device was discovered.\n");
		destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}
	if (head->name != XRT_DEVICE_PSVR2) {
		fprintf(stderr, "Selected head device is not a PS VR2: %s\n", head->str);
		destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	printf("PS VR2 discovered as '%s'.\n", head->str);
	printf("Move the headset during the %u-second probe.\n", duration_s);
	printf("  time       flags       position (m)                 orientation (x y z w)\n");

	const enum xrt_space_relation_flags required_flags = (enum xrt_space_relation_flags)(
	    XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	    XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	const int64_t start_ns = os_monotonic_get_ns();
	const int64_t end_ns = start_ns + (int64_t)duration_s * U_TIME_1S_IN_NS;
	const int64_t sample_interval_ns = U_TIME_1S_IN_NS / 60;
	const int64_t print_interval_ns = U_TIME_1S_IN_NS / 10;
	int64_t next_print_ns = start_ns;
	uint32_t tracked_sample_count = 0;
	bool have_first_pose = false;
	bool saw_motion = false;
	struct xrt_pose first_pose = XRT_POSE_IDENTITY;

	while (os_monotonic_get_ns() < end_ns) {
		int64_t now_ns = os_monotonic_get_ns();
		struct xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
		xret = xrt_device_get_tracked_pose(head, XRT_INPUT_GENERIC_HEAD_POSE, now_ns, &relation);
		if (xret != XRT_SUCCESS) {
			fprintf(stderr, "Head pose query failed: %d\n", xret);
			destroy_system(&xi, &xsys, &xsysd, &xso);
			return EXIT_FAILURE;
		}

		bool fully_tracked = (relation.relation_flags & required_flags) == required_flags;
		if (fully_tracked) {
			tracked_sample_count++;
			if (!have_first_pose) {
				first_pose = relation.pose;
				have_first_pose = true;
			} else if (pose_changed(&first_pose, &relation.pose)) {
				saw_motion = true;
			}
		}

		if (now_ns >= next_print_ns) {
			double elapsed_s = (double)(now_ns - start_ns) / U_TIME_1S_IN_NS;
			printf("%7.2f s   0x%02x   %+8.4f %+8.4f %+8.4f   %+8.4f %+8.4f %+8.4f %+8.4f\n",
			       elapsed_s, (unsigned)relation.relation_flags, relation.pose.position.x,
			       relation.pose.position.y, relation.pose.position.z, relation.pose.orientation.x,
			       relation.pose.orientation.y, relation.pose.orientation.z, relation.pose.orientation.w);
			fflush(stdout);
			next_print_ns += print_interval_ns;
		}

		os_nanosleep(sample_interval_ns);
	}

	destroy_system(&xi, &xsys, &xsysd, &xso);

	if (tracked_sample_count == 0) {
		fprintf(stderr, "FAIL: no pose had valid and tracked position and orientation.\n");
		return EXIT_FAILURE;
	}
	if (!saw_motion) {
		fprintf(stderr, "FAIL: tracked poses were received, but no headset motion exceeded the probe threshold.\n");
		return EXIT_FAILURE;
	}

	printf("PASS: received %u fully tracked pose samples and observed headset motion.\n", tracked_sample_count);
	return EXIT_SUCCESS;
}

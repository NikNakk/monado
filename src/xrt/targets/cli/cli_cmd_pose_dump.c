// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Continuously dump PSVR2/HMD prediction sweeps as CSV.
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"

#include "os/os_time.h"

#include "cli_common.h"

#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>


static volatile sig_atomic_t keep_running = 1;


static void
handle_signal(int sig)
{
	(void)sig;
	keep_running = 0;
}


static void
print_relation(const struct xrt_space_relation *r, xrt_result_t result)
{
	printf(
	    "%.9f,%.9f,%.9f,"
	    "%.9f,%.9f,%.9f,%.9f,"
	    "%.9f,%.9f,%.9f,"
	    "%.9f,%.9f,%.9f,"
	    "%u,%d",
	    r->pose.position.x,
	    r->pose.position.y,
	    r->pose.position.z,
	    r->pose.orientation.x,
	    r->pose.orientation.y,
	    r->pose.orientation.z,
	    r->pose.orientation.w,
	    r->linear_velocity.x,
	    r->linear_velocity.y,
	    r->linear_velocity.z,
	    r->angular_velocity.x,
	    r->angular_velocity.y,
	    r->angular_velocity.z,
	    (unsigned)r->relation_flags,
	    (int)result);
}


static void
print_relation_header(const char *prefix)
{
	printf(
	    "%s_px,%s_py,%s_pz,"
	    "%s_qx,%s_qy,%s_qz,%s_qw,"
	    "%s_vx,%s_vy,%s_vz,"
	    "%s_wx,%s_wy,%s_wz,"
	    "%s_flags,%s_result",
	    prefix,
	    prefix,
	    prefix,
	    prefix,
	    prefix,
	    prefix,
	    prefix,
	    prefix,
	    prefix,
	    prefix,
	    prefix,
	    prefix,
	    prefix,
	    prefix,
	    prefix);
}


int
cli_cmd_pose_dump(int argc, const char **argv)
{
	double frequency_hz = 200.0;

	/*
	 * New syntax:
	 *
	 *   monado-cli pose-dump [frequency_hz]
	 *
	 * Prediction horizons are fixed at:
	 *   0, 5, 10, 15, 20 ms
	 */
	if (argc >= 3) {
		frequency_hz = strtod(argv[2], NULL);
	}

	if (frequency_hz <= 0.0) {
		fprintf(stderr, "frequency_hz must be > 0\n");
		return 1;
	}

	const int64_t interval_ns =
	    (int64_t)(1000000000.0 / frequency_hz);

	static const int64_t prediction_ns[] = {
	    0,
	    5000000,
	    10000000,
	    15000000,
	    20000000,
	};

	static const char *prediction_names[] = {
	    "p0",
	    "p5",
	    "p10",
	    "p15",
	    "p20",
	};

	const size_t prediction_count =
	    sizeof(prediction_ns) / sizeof(prediction_ns[0]);

	fprintf(stderr,
	        "pose-dump: simultaneous prediction sweep "
	        "0/5/10/15/20 ms at %.3f Hz\n",
	        frequency_hz);

	struct xrt_instance *xi = NULL;
	struct xrt_system *xsys = NULL;
	struct xrt_system_devices *xsysd = NULL;
	struct xrt_space_overseer *xso = NULL;

	xrt_result_t xret = xrt_instance_create(NULL, &xi);

	if (xret != XRT_SUCCESS || xi == NULL) {
		fprintf(stderr,
		        "Failed to create xrt_instance: %d\n",
		        (int)xret);
		return 1;
	}

	xret = xrt_instance_create_system(
	    xi,
	    &xsys,
	    &xsysd,
	    &xso,
	    NULL);

	if (xret != XRT_SUCCESS || xsysd == NULL) {
		fprintf(stderr,
		        "Failed to create xrt system: %d\n",
		        (int)xret);

		xrt_instance_destroy(&xi);
		return 1;
	}

	struct xrt_device *hmd = xsysd->static_roles.head;

	if (hmd == NULL) {
		for (uint32_t i = 0; i < XRT_SYSTEM_MAX_DEVICES; i++) {
			struct xrt_device *xdev = xsysd->static_xdevs[i];

			if (xdev != NULL && xdev->hmd != NULL) {
				hmd = xdev;
				break;
			}
		}
	}

	if (hmd == NULL) {
		fprintf(stderr, "No HMD found\n");

		xrt_space_overseer_destroy(&xso);
		xrt_system_devices_destroy(&xsysd);
		xrt_system_destroy(&xsys);
		xrt_instance_destroy(&xi);

		return 1;
	}

	fprintf(stderr, "Using HMD: %s\n", hmd->str);

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);

	/*
	 * One row = one common query time plus five requested prediction
	 * horizons.
	 */
	printf("sample,query_time_ns");

	for (size_t i = 0; i < prediction_count; i++) {
		printf(",%s_requested_time_ns,", prediction_names[i]);
		print_relation_header(prediction_names[i]);
	}

	printf("\n");
	fflush(stdout);

	uint64_t sample = 0;
	int64_t next_ns = os_monotonic_get_ns();

	while (keep_running) {
		const int64_t query_ns = os_monotonic_get_ns();

		struct xrt_space_relation relations[5] = {0};
		xrt_result_t results[5] = {0};

		/*
		 * Deliberately make all five queries from the same base
		 * timestamp.
		 */
		for (size_t i = 0; i < prediction_count; i++) {
			const int64_t requested_ns =
			    query_ns + prediction_ns[i];

			results[i] = xrt_device_get_tracked_pose(
			    hmd,
			    XRT_INPUT_GENERIC_HEAD_POSE,
			    requested_ns,
			    &relations[i]);
		}

		printf("%" PRIu64 ",%" PRIi64, sample++, query_ns);

		for (size_t i = 0; i < prediction_count; i++) {
			const int64_t requested_ns =
			    query_ns + prediction_ns[i];

			printf(",%" PRIi64 ",", requested_ns);
			print_relation(&relations[i], results[i]);
		}

		printf("\n");

		if ((sample & 0x3f) == 0) {
			fflush(stdout);
		}

		next_ns += interval_ns;

		const int64_t now_ns = os_monotonic_get_ns();

		if (next_ns > now_ns) {
			os_nanosleep(next_ns - now_ns);
		} else {
			next_ns = now_ns;
		}
	}

	fflush(stdout);
	fprintf(stderr, "Stopping pose dump\n");

	xrt_space_overseer_destroy(&xso);
	xrt_system_devices_destroy(&xsysd);
	xrt_system_destroy(&xsys);
	xrt_instance_destroy(&xi);

	return 0;
}

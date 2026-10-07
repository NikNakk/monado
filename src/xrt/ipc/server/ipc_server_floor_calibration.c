// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Optional automatic floor calibration while the service runs.
 * @ingroup ipc_server
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_system.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_floor_calibration.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"

#include "server/ipc_server.h"

#include <math.h>


DEBUG_GET_ONCE_FLOAT_OPTION(floor_eye_height, "XRT_FLOOR_EYE_HEIGHT_M", 0.0f)

#define POLL_INTERVAL_NS (100 * U_TIME_1MS_IN_NS)

struct ipc_server_floor_calibration
{
	struct os_thread_helper oth;
	struct u_floor_calibration fc;
	struct xrt_space_overseer *xso;
	struct xrt_device *head;
};

static void *
run_floor_calibration(void *ptr)
{
	struct ipc_server_floor_calibration *fcal = ptr;
	os_thread_name_self("Floor calibration");

	U_LOG_I("Floor calibration: waiting for a worn, tracked, level and steady head (eye height %.2f m).",
	        fcal->fc.eye_height_m);

	os_thread_helper_lock(&fcal->oth);
	while (os_thread_helper_is_running_locked(&fcal->oth)) {
		os_thread_helper_unlock(&fcal->oth);

		if (u_floor_calibration_poll(&fcal->fc, fcal->xso, fcal->head, os_monotonic_get_ns())) {
			return NULL;
		}
		os_nanosleep(POLL_INTERVAL_NS);

		os_thread_helper_lock(&fcal->oth);
	}
	os_thread_helper_unlock(&fcal->oth);

	return NULL;
}

void
ipc_server_floor_calibration_start(struct ipc_server *s)
{
	const float eye_height = debug_get_float_option_floor_eye_height();
	if (eye_height == 0.0f || s->floor_calibration != NULL || s->xso == NULL || s->xsysd == NULL) {
		return;
	}
	if (!isfinite(eye_height) || eye_height < 0.5f || eye_height > 2.5f) {
		U_LOG_W("Floor calibration: XRT_FLOOR_EYE_HEIGHT_M must be between 0.5 and 2.5 metres; ignored.");
		return;
	}

	struct xrt_device *head = s->xsysd->static_roles.head;
	if (head == NULL || !head->supported.position_tracking) {
		U_LOG_W("Floor calibration: no position-tracked head device; automatic calibration disabled.");
		return;
	}

	struct ipc_server_floor_calibration *fcal = U_TYPED_CALLOC(struct ipc_server_floor_calibration);
	u_floor_calibration_init(&fcal->fc, eye_height);
	fcal->xso = s->xso;
	fcal->head = head;

	if (os_thread_helper_init(&fcal->oth) != 0 ||
	    os_thread_helper_start(&fcal->oth, run_floor_calibration, fcal) != 0) {
		U_LOG_E("Floor calibration: failed to start its thread.");
		os_thread_helper_destroy(&fcal->oth);
		free(fcal);
		return;
	}

	s->floor_calibration = fcal;
}

void
ipc_server_floor_calibration_stop(struct ipc_server *s)
{
	if (s->floor_calibration == NULL) {
		return;
	}

	// Joins the thread whether it is still polling or already finished.
	os_thread_helper_destroy(&s->floor_calibration->oth);
	free(s->floor_calibration);
	s->floor_calibration = NULL;
}

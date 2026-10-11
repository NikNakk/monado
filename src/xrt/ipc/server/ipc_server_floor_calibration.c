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
#include "math/m_api.h"
#include "util/u_debug.h"
#include "util/u_floor_calibration.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"

#include "server/ipc_server.h"

#include <math.h>


DEBUG_GET_ONCE_FLOAT_OPTION(floor_eye_height, "XRT_FLOOR_EYE_HEIGHT_M", 0.0f)
// Also centre the STAGE under the head and face it the head's way.
DEBUG_GET_ONCE_BOOL_OPTION(floor_align, "XRT_FLOOR_ALIGN", true)
// Opt-in until the controller gesture has been validated on hardware.
DEBUG_GET_ONCE_BOOL_OPTION(stage_recenter, "XRT_PSSENSE_STAGE_RECENTER", false)
DEBUG_GET_ONCE_NUM_OPTION(stage_recenter_hold_ms, "XRT_PSSENSE_STAGE_RECENTER_HOLD_MS", 1000)

#define POLL_INTERVAL_NS (100 * U_TIME_1MS_IN_NS)

struct ipc_server_floor_calibration
{
	struct os_thread_helper oth;
	struct u_floor_calibration fc;
	struct ipc_server *server;
	struct xrt_device *right;
	struct u_stage_recenter_hold hold;
	int64_t hold_ns;
	bool calibrate;
	struct xrt_space_overseer *xso;
	struct xrt_device *head;
};

static void *
run_floor_calibration(void *ptr)
{
	struct ipc_server_floor_calibration *fcal = ptr;
	os_thread_name_self("Floor calibration");

	if (fcal->calibrate) {
		U_LOG_I("Floor calibration: waiting for a worn, tracked, level and steady head (eye height %.2f m).",
		        fcal->fc.eye_height_m);
	}

	os_thread_helper_lock(&fcal->oth);
	while (os_thread_helper_is_running_locked(&fcal->oth)) {
		os_thread_helper_unlock(&fcal->oth);

		int64_t now_ns = os_monotonic_get_ns();
		os_mutex_lock(&fcal->server->input_lock);
		if (fcal->calibrate) {
			fcal->calibrate = !u_floor_calibration_poll(&fcal->fc, fcal->xso, fcal->head, now_ns);
		}
		bool pressed = false;
		if (fcal->right != NULL && xrt_device_update_inputs(fcal->right) == XRT_SUCCESS) {
			now_ns = os_monotonic_get_ns();
			for (uint32_t i = 0; i < fcal->right->input_count; i++) {
				const struct xrt_input *input = &fcal->right->inputs[i];
				if (input->name == XRT_INPUT_PSSENSE_OPTIONS_CLICK) {
					pressed = input->active && input->value.boolean &&
					          input->timestamp <= now_ns + 100 * U_TIME_1MS_IN_NS &&
					          now_ns - input->timestamp < U_TIME_1S_IN_NS;
				}
			}
		}
		os_mutex_unlock(&fcal->server->input_lock);
		if (u_stage_recenter_hold_update(&fcal->hold, pressed, now_ns, fcal->hold_ns)) {
			xrt_result_t ret = u_stage_recenter(fcal->xso, fcal->head, now_ns);
			if (ret == XRT_SUCCESS) {
				U_LOG_I("STAGE recentered from right Sense Options hold (floor height preserved).");
			} else {
				U_LOG_W("STAGE recenter refused (%d); release Options before retrying.", ret);
			}
		}
		if (!fcal->calibrate && fcal->right == NULL) {
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
	float eye_height = debug_get_float_option_floor_eye_height();
	bool recenter = debug_get_bool_option_stage_recenter();
	if ((eye_height == 0.0f && !recenter) || s->floor_calibration != NULL || s->xso == NULL || s->xsysd == NULL) {
		return;
	}
	if (eye_height != 0.0f && (!isfinite(eye_height) || eye_height < 0.5f || eye_height > 2.5f)) {
		U_LOG_W("Floor calibration: XRT_FLOOR_EYE_HEIGHT_M must be between 0.5 and 2.5 metres; ignored.");
		eye_height = 0.0f;
		if (!recenter) {
			return;
		}
	}

	struct xrt_device *head = s->xsysd->static_roles.head;
	if (head == NULL || !head->supported.position_tracking) {
		U_LOG_W("Floor calibration: no position-tracked head device; automatic calibration disabled.");
		return;
	}

	struct ipc_server_floor_calibration *fcal = U_TYPED_CALLOC(struct ipc_server_floor_calibration);
	u_floor_calibration_init(&fcal->fc, eye_height);
	fcal->fc.align = debug_get_bool_option_floor_align();
	fcal->server = s;
	fcal->calibrate = eye_height != 0.0f;
	fcal->hold_ns = (int64_t)CLAMP(debug_get_num_option_stage_recenter_hold_ms(), 100, 10000) * U_TIME_1MS_IN_NS;
	if (recenter) {
		struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
		if (xrt_system_devices_get_roles(s->xsysd, &roles) == XRT_SUCCESS && roles.right >= 0 &&
		    (uint32_t)roles.right < s->xsysd->static_xdev_count) {
			fcal->right = s->xsysd->static_xdevs[roles.right];
		}
	}
	fcal->xso = s->xso;
	fcal->head = head;

	if (os_thread_helper_init(&fcal->oth) != 0) {
		U_LOG_E("Floor calibration: failed to initialise its thread helper.");
		free(fcal);
		return;
	}
	if (os_thread_helper_start(&fcal->oth, run_floor_calibration, fcal) != 0) {
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

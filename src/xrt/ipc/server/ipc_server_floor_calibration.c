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
#include <string.h>


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
	//! Right-hand controller and its Options input, refreshed every poll.
	struct xrt_device *right;
	int32_t options_index;
	bool right_logged;
	struct u_stage_recenter_hold hold;
	int64_t hold_ns;
	bool calibrate;
	bool recenter;
	struct xrt_space_overseer *xso;
	struct xrt_device *head;
};

/*!
 * Look up the right-hand controller each poll, so a role assigned after
 * start-up is picked up, and say once whenever the gesture becomes available
 * or unavailable.
 */
static void
update_right_controller(struct ipc_server_floor_calibration *fcal)
{
	struct xrt_system_devices *xsysd = fcal->server->xsysd;
	struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
	struct xrt_device *right = NULL;
	if (xrt_system_devices_get_roles(xsysd, &roles) == XRT_SUCCESS && roles.right >= 0 &&
	    (uint32_t)roles.right < xsysd->static_xdev_count) {
		right = xsysd->static_xdevs[roles.right];
	}
	if (right == fcal->right && fcal->right_logged) {
		return;
	}

	fcal->right = right;
	fcal->right_logged = true;
	fcal->options_index = -1;
	for (uint32_t i = 0; right != NULL && i < right->input_count; i++) {
		if (right->inputs[i].name == XRT_INPUT_PSSENSE_OPTIONS_CLICK) {
			fcal->options_index = (int32_t)i;
		}
	}
	memset(&fcal->hold, 0, sizeof(fcal->hold));

	if (right == NULL) {
		U_LOG_W("STAGE recenter: no right-hand controller assigned; the gesture is unavailable until one is.");
	} else if (fcal->options_index < 0) {
		U_LOG_W("STAGE recenter: right-hand controller '%s' has no Sense Options button; gesture unavailable.",
		        right->str);
	} else {
		U_LOG_I("STAGE recenter: hold Options on '%s' for %lld ms to recentre%s.", right->str,
		        (long long)(fcal->hold_ns / U_TIME_1MS_IN_NS),
		        fcal->fc.eye_height_m > 0.0f ? " and re-measure the floor" : "");
	}
}

/*!
 * Whether the right controller's Options button is held. The input lock is
 * held only while the inputs are updated and read. Sets @p out_now_ns to the
 * time of that read.
 */
static bool
read_options_pressed(struct ipc_server_floor_calibration *fcal, int64_t *out_now_ns)
{
	if (fcal->right == NULL || fcal->options_index < 0) {
		return false;
	}

	bool pressed = false;
	os_mutex_lock(&fcal->server->input_lock);
	if (xrt_device_update_inputs(fcal->right) == XRT_SUCCESS) {
		int64_t now_ns = os_monotonic_get_ns();
		const struct xrt_input *input = &fcal->right->inputs[fcal->options_index];
		pressed = input->active && input->value.boolean && input->timestamp <= now_ns + 100 * U_TIME_1MS_IN_NS &&
		          now_ns - input->timestamp < U_TIME_1S_IN_NS;
		*out_now_ns = now_ns;
	}
	os_mutex_unlock(&fcal->server->input_lock);
	return pressed;
}

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
		if (fcal->calibrate) {
			fcal->calibrate = !u_floor_calibration_poll(&fcal->fc, fcal->xso, fcal->head, now_ns);
		}
		bool pressed = false;
		if (fcal->recenter) {
			update_right_controller(fcal);
			pressed = read_options_pressed(fcal, &now_ns);
		}
		if (u_stage_recenter_hold_update(&fcal->hold, pressed, now_ns, fcal->hold_ns)) {
			// With an eye height configured, the hold also re-measures the
			// floor, so a calibration taken while seated can be redone standing.
			float eye_height = fcal->fc.eye_height_m;
			xrt_result_t ret = u_stage_recenter(fcal->xso, fcal->head, now_ns, eye_height);
			if (ret == XRT_SUCCESS && eye_height > 0.0f) {
				U_LOG_I("STAGE recentered from right Sense Options hold, floor %.2f m below the head.",
				        eye_height);
				// The STAGE now holds a standing measurement; do not replace it.
				fcal->calibrate = false;
			} else if (ret == XRT_SUCCESS) {
				U_LOG_I("STAGE recentered from right Sense Options hold (floor height preserved).");
			} else {
				U_LOG_W("STAGE recenter refused (%d); release Options before retrying.", ret);
			}
		}
		if (!fcal->calibrate && !fcal->recenter) {
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
	fcal->fc.input_lock = &s->input_lock;
	fcal->server = s;
	fcal->calibrate = eye_height != 0.0f;
	fcal->recenter = recenter;
	fcal->options_index = -1;
	fcal->hold_ns = (int64_t)CLAMP(debug_get_num_option_stage_recenter_hold_ms(), 100, 10000) * U_TIME_1MS_IN_NS;
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

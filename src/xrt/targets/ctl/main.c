// Copyright 2020-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Small cli application to control IPC service.
 * @author Pete Black <pblack@collabora.com>
 * @ingroup ipc
 */

#include "util/u_file.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "os/os_time.h"
#include "math/m_api.h"

#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"

#include "ipc_client_generated.h"
#include "xrt/xrt_results.h"

#include <getopt.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>


#define P(...) fprintf(stdout, __VA_ARGS__)
#define PE(...) fprintf(stderr, __VA_ARGS__)

typedef enum op_mode
{
	MODE_GET,
	MODE_SET_PRIMARY,
	MODE_SET_FOCUSED,
	MODE_TOGGLE_IO,
	MODE_RECENTER,
	MODE_GET_BRIGHTNESS,
	MODE_SET_BRIGHTNESS,
	MODE_CALIBRATE_FLOOR,
} op_mode_t;


struct full_device_info
{
	struct ipc_device_list list;
	struct ipc_device_info info[XRT_SYSTEM_MAX_DEVICES];
};

/*
 *
 * Helper functions.
 *
 */

/*!
 * Get the device list from a given connection.
 *
 * @param ipc_c The IPC connection.
 * @param[out] out_full The full device info struct to fill out.
 * @return XRT_SUCCESS on success, or an error code.
 */
static xrt_result_t
get_device_list(struct ipc_connection *ipc_c, struct full_device_info *out_full)
{
	xrt_result_t xret = XRT_SUCCESS;
	xret = ipc_call_system_devices_get_list(ipc_c, &out_full->list);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	for (uint32_t i = 0; i < out_full->list.device_count; i++) {
		uint32_t device_id = out_full->list.devices[i].id;

		xret = ipc_call_device_get_info_no_arrays(ipc_c, device_id, &out_full->info[i]);
		if (xret != XRT_SUCCESS) {
			PE("Failed to get device info for device %u.\n", device_id);
			return xret;
		}
	}

	return XRT_SUCCESS;
}

int
get_mode(struct ipc_connection *ipc_c)
{
	struct ipc_client_list clients;

	xrt_result_t r;

	r = ipc_call_system_get_clients(ipc_c, &clients);
	if (r != XRT_SUCCESS) {
		PE("Failed to get client list.\n");
		exit(1);
	}

	P("Clients:\n");
	for (uint32_t i = 0; i < clients.id_count; i++) {
		uint32_t id = clients.ids[i];

		struct ipc_app_state cs;
		r = ipc_call_system_get_client_info(ipc_c, id, &cs);
		if (r != XRT_SUCCESS) {
			PE("Failed to get client info for client %d.\n", id);
			return 1;
		}

		P("\tid: %d"
		  "\tact: %d"
		  "\tdisp: %d"
		  "\tfoc: %d"
		  "\tposes: %d"
		  "\thands: %d"
		  "\tinputs: %d"
		  "\toutputs: %d"
		  "\tovly: %d"
		  "\tz: %d"
		  "\tpid: %d"
		  "\t%s\n",
		  clients.ids[i],                    //
		  cs.session_active,                 //
		  cs.session_visible,                //
		  cs.session_focused,                //
		  !cs.io_blocks.block_poses,         //
		  !cs.io_blocks.block_hand_tracking, //
		  !cs.io_blocks.block_inputs,        //
		  !cs.io_blocks.block_outputs,       //
		  cs.session_overlay,                //
		  cs.z_order,                        //
		  (int)cs.pid,                       //
		  cs.info.application_name);
	}

	struct full_device_info full_device_info;
	r = get_device_list(ipc_c, &full_device_info);
	if (r != XRT_SUCCESS) {
		PE("Failed to get device list.\n");
		return 1;
	}

	P("\nDevices:\n");
	for (uint32_t i = 0; i < full_device_info.list.device_count; i++) {
		P("\tid: %d"
		  "\tname: %d"
		  "\t\"%s\"\n",
		  full_device_info.list.devices[i].id, //
		  full_device_info.info[i].name,       //
		  full_device_info.info[i].str);       //
	}

	return 0;
}

int
set_primary(struct ipc_connection *ipc_c, int client_id)
{
	xrt_result_t r;

	r = ipc_call_system_set_primary_client(ipc_c, client_id);
	if (r != XRT_SUCCESS) {
		PE("Failed to set active client to %d.\n", client_id);
		return 1;
	}

	return 0;
}

int
set_focused(struct ipc_connection *ipc_c, int client_id)
{
	xrt_result_t r;

	r = ipc_call_system_set_focused_client(ipc_c, client_id);
	if (r != XRT_SUCCESS) {
		PE("Failed to set focused client to %d.\n", client_id);
		return 1;
	}

	return 0;
}

int
toggle_io(struct ipc_connection *ipc_c, int client_id)
{
	xrt_result_t r;

	r = ipc_call_system_toggle_io_client(ipc_c, client_id);
	if (r != XRT_SUCCESS) {
		PE("Failed to toggle io for client %d.\n", client_id);
		return 1;
	}

	return 0;
}

int
recenter_local_spaces(struct ipc_connection *ipc_c)
{
	xrt_result_t r;

	r = ipc_call_space_recenter_local_spaces(ipc_c);
	if (r != XRT_SUCCESS) {
		PE("Failed to recenter local spaces.\n");
		return 1;
	}

	return 0;
}

int
get_first_device_id(struct ipc_connection *ipc_c)
{
	struct full_device_info full_device_info;
	xrt_result_t xret;

	// Get device list using the new API
	xret = get_device_list(ipc_c, &full_device_info);
	if (xret != XRT_SUCCESS) {
		PE("Failed to get device list.\n");
		return -1;
	}

	// Look for the first HMD device
	for (uint32_t i = 0; i < full_device_info.list.device_count; i++) {
		if (full_device_info.list.devices[i].device_type == XRT_DEVICE_TYPE_HMD) {
			uint32_t device_id = full_device_info.list.devices[i].id;

			// Print to stderr to enable scripting
			PE("Picked device %u: %s\n", device_id, full_device_info.info[i].str);

			return device_id;
		}
	}

	return -1;
}

int
get_brightness(struct ipc_connection *ipc_c, int device_id)
{
	device_id = device_id >= 0 ? device_id : get_first_device_id(ipc_c);
	if (device_id < 0) {
		PE("Couldn't find a HMD device!\n");
		return 1;
	}

	float out_brightness;
	xrt_result_t r = ipc_call_device_get_brightness(ipc_c, device_id, &out_brightness);
	if (r != XRT_SUCCESS) {
		PE("Failed to get brightness for device %d\n", device_id);
		return 1;
	}

	P("%d\n", (int)(out_brightness * 100));

	return 0;
}

int
set_brightness(struct ipc_connection *ipc_c, int device_id, const char *value)
{
	if (value == NULL) {
		return 1;
	}

	const int length = strlen(value);
	if (length == 0) {
		return 1;
	}

	bool relative = (value[0] == '-' || value[0] == '+');

	char *end = NULL;
	float target_brightness = strtof(value, &end);

	if ((length > (end - value)) && *end == '%') {
		target_brightness /= 100.f;
	}

	device_id = device_id >= 0 ? device_id : get_first_device_id(ipc_c);
	if (device_id < 0) {
		PE("Couldn't find a HMD device!\n");
		return 1;
	}

	xrt_result_t r = ipc_call_device_set_brightness(ipc_c, device_id, target_brightness, relative);
	if (r != XRT_SUCCESS) {
		PE("Failed to set brightness for device %d\n", device_id);
		return 1;
	}

	float out_brightness;
	r = ipc_call_device_get_brightness(ipc_c, device_id, &out_brightness);
	if (r != XRT_SUCCESS) {
		PE("Failed to get brightness for device %d\n", device_id);
		return 1;
	}

	if (relative || out_brightness != target_brightness) {
		P("Set brightness to %d%%\n", (int)(out_brightness * 100));
	}

	return 0;
}

/*
 *
 * Floor calibration of the managed STAGE space.
 *
 */

#define FLOOR_SAMPLE_COUNT 30
#define FLOOR_SAMPLE_INTERVAL_NS (16 * U_TIME_1MS_IN_NS)
#define FLOOR_MAX_SPREAD_M 0.01f
// Grip pose height of a controller lying on the floor; override per controller.
#define FLOOR_DEFAULT_DEVICE_HEIGHT_M 0.03f

// Grip poses tried, in order, to find a controller's pose input.
static const enum xrt_input_name grip_pose_names[] = {
    XRT_INPUT_PSSENSE_GRIP_POSE,    XRT_INPUT_INDEX_GRIP_POSE,       XRT_INPUT_TOUCH_GRIP_POSE,
    XRT_INPUT_TOUCH_PLUS_GRIP_POSE, XRT_INPUT_TOUCH_PRO_GRIP_POSE,   XRT_INPUT_VIVE_GRIP_POSE,
    XRT_INPUT_WMR_GRIP_POSE,        XRT_INPUT_G2_CONTROLLER_GRIP_POSE, XRT_INPUT_PSMV_GRIP_POSE,
    XRT_INPUT_SIMPLE_GRIP_POSE,     XRT_INPUT_GENERIC_GRIP_POSE,     XRT_INPUT_GENERIC_TRACKER_POSE,
};

/*!
 * Height of one tracked pose of a device in the root space.
 */
static bool
sample_height_in_root(struct ipc_connection *ipc_c,
                      uint32_t root_id,
                      uint32_t device_id,
                      enum xrt_input_name name,
                      float *out_height)
{
	struct xrt_pose identity = XRT_POSE_IDENTITY;
	int64_t now = os_monotonic_get_ns();
	struct xrt_space_relation origin, pose;

	// Where the device's poses are reported, then the pose within that.
	if (ipc_call_space_locate_device(ipc_c, root_id, &identity, now, device_id, &origin) != XRT_SUCCESS ||
	    ipc_call_device_get_tracked_pose(ipc_c, device_id, name, now, &pose) != XRT_SUCCESS) {
		return false;
	}

	const enum xrt_space_relation_flags valid =
	    XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT;
	if ((origin.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) == 0 ||
	    (pose.relation_flags & valid) != valid) {
		return false;
	}

	struct xrt_pose in_root;
	math_pose_transform(&origin.pose, &pose.pose, &in_root);
	*out_height = in_root.position.y;
	return true;
}

/*!
 * Average height of a device that is held still, in the root space.
 */
static int
measure_height_in_root(
    struct ipc_connection *ipc_c, uint32_t root_id, uint32_t device_id, enum xrt_input_name name, float *out_height)
{
	float min = 0, max = 0, sum = 0;
	for (int i = 0; i < FLOOR_SAMPLE_COUNT; i++) {
		float height;
		if (!sample_height_in_root(ipc_c, root_id, device_id, name, &height)) {
			PE("Device %u is not tracked; keep it in view of the tracking and try again.\n", device_id);
			return 1;
		}
		min = i == 0 ? height : fminf(min, height);
		max = i == 0 ? height : fmaxf(max, height);
		sum += height;
		os_nanosleep(FLOOR_SAMPLE_INTERVAL_NS);
	}

	if (max - min > FLOOR_MAX_SPREAD_M) {
		PE("Device %u moved by %.1f cm while measuring; hold it still and try again.\n", device_id,
		   (max - min) * 100.f);
		return 1;
	}

	*out_height = sum / FLOOR_SAMPLE_COUNT;
	return 0;
}

static int
get_device_for_floor(struct ipc_connection *ipc_c, const char *which, int *out_device_id)
{
	if (strcmp(which, "left") == 0 || strcmp(which, "right") == 0) {
		struct xrt_system_roles roles;
		if (ipc_call_system_devices_get_roles(ipc_c, &roles) != XRT_SUCCESS) {
			PE("Failed to get device roles.\n");
			return 1;
		}
		*out_device_id = strcmp(which, "left") == 0 ? roles.left : roles.right;
		if (*out_device_id < 0) {
			PE("No %s controller is connected.\n", which);
			return 1;
		}
		return 0;
	}

	char *end = NULL;
	long id = strtol(which, &end, 10);
	if (end == which || *end != '\0' || id < 0) {
		PE("Use left, right or a device id from monado-ctl for the floor device.\n");
		return 1;
	}
	*out_device_id = (int)id;
	return 0;
}

/*!
 * Set the managed STAGE floor so that either the head is @p eye_height above
 * it, or the controller pose is @p device_height above it.
 */
int
calibrate_floor(struct ipc_connection *ipc_c, float eye_height, const char *device, float device_height)
{
	uint32_t root_id, view_id, local_id, local_floor_id, stage_id, unbounded_id;
	xrt_result_t xret = ipc_call_space_create_semantic_ids( //
	    ipc_c, &root_id, &view_id, &local_id, &local_floor_id, &stage_id, &unbounded_id);
	if (xret != XRT_SUCCESS || root_id == UINT32_MAX) {
		PE("Failed to get the root space.\n");
		return 1;
	}

	float measured;
	float height_above_floor;
	if (device == NULL) {
		int head_id = get_first_device_id(ipc_c);
		if (head_id < 0) {
			PE("Couldn't find a HMD device!\n");
			return 1;
		}
		P("Measuring head height; stand upright and keep still.\n");
		if (measure_height_in_root(ipc_c, root_id, head_id, XRT_INPUT_GENERIC_HEAD_POSE, &measured) != 0) {
			return 1;
		}
		height_above_floor = eye_height;
	} else {
		int device_id;
		if (get_device_for_floor(ipc_c, device, &device_id) != 0) {
			return 1;
		}
		enum xrt_input_name name = 0;
		float ignored;
		for (size_t i = 0; i < ARRAY_SIZE(grip_pose_names) && name == 0; i++) {
			if (sample_height_in_root(ipc_c, root_id, device_id, grip_pose_names[i], &ignored)) {
				name = grip_pose_names[i];
			}
		}
		if (name == 0) {
			PE("Device %d has no tracked grip pose; keep it in view of the tracking and try again.\n",
			   device_id);
			return 1;
		}
		P("Measuring controller height; leave it resting on the floor.\n");
		if (measure_height_in_root(ipc_c, root_id, device_id, name, &measured) != 0) {
			return 1;
		}
		height_above_floor = device_height;
	}

	struct xrt_pose stage;
	xret = ipc_call_space_get_reference_space_offset(ipc_c, XRT_SPACE_REFERENCE_TYPE_STAGE, &stage);
	if (xret != XRT_SUCCESS) {
		PE("Failed to read the STAGE offset (%d).\n", xret);
		return 1;
	}

	// Only the floor height changes; the stage keeps its position and heading.
	const float previous = stage.position.y;
	stage.position.y = measured - height_above_floor;
	xret = ipc_call_space_set_reference_space_offset(ipc_c, XRT_SPACE_REFERENCE_TYPE_STAGE, &stage);
	if (xret != XRT_SUCCESS) {
		PE("Failed to set the STAGE offset (%d). The driver provides its own STAGE space, which this "
		   "calibration cannot move.\n",
		   xret);
		return 1;
	}

	P("Floor moved by %+.3f m; the measured pose is %.3f m above the floor.\n", stage.position.y - previous,
	  height_above_floor);
	P("This lasts until the service exits or its tracking origin is reset.\n");
	return 0;
}

enum LongOptions
{
	OPTION_DEVICE = 1,
	OPTION_GET_BRIGHTNESS,
	OPTION_SET_BRIGHTNESS,
	OPTION_FLOOR_EYE_HEIGHT,
	OPTION_FLOOR_DEVICE,
	OPTION_FLOOR_DEVICE_HEIGHT,
};

int
main(int argc, char *argv[])
{
	op_mode_t op_mode = MODE_GET;

	// parse arguments
	int c;
	int s_val = 0;
	int device_val = -1;
	char *brightness;
	float floor_eye_height = 0;
	const char *floor_device = NULL;
	float floor_device_height = FLOOR_DEFAULT_DEVICE_HEIGHT_M;

	static struct option long_options[] = {
	    {"device", required_argument, NULL, OPTION_DEVICE},
	    {"get-brightness", no_argument, NULL, OPTION_GET_BRIGHTNESS},
	    {"set-brightness", required_argument, NULL, OPTION_SET_BRIGHTNESS},
	    {"floor-eye-height", required_argument, NULL, OPTION_FLOOR_EYE_HEIGHT},
	    {"floor-device", required_argument, NULL, OPTION_FLOOR_DEVICE},
	    {"floor-device-height", required_argument, NULL, OPTION_FLOOR_DEVICE_HEIGHT},
	    {NULL, 0, NULL, 0},
	};

	int option_index = 0;
	opterr = 0;
	while ((c = getopt_long(argc, argv, "p:f:i:c", long_options, &option_index)) != -1) {
		switch (c) {
		case 'p':
			s_val = atoi(optarg);
			op_mode = MODE_SET_PRIMARY;
			break;
		case 'f':
			s_val = atoi(optarg);
			op_mode = MODE_SET_FOCUSED;
			break;
		case 'i':
			s_val = atoi(optarg);
			op_mode = MODE_TOGGLE_IO;
			break;
		case 'c': op_mode = MODE_RECENTER; break;
		case OPTION_DEVICE: {
			device_val = atoi(optarg);
			break;
		}
		case OPTION_GET_BRIGHTNESS: {
			op_mode = MODE_GET_BRIGHTNESS;
			break;
		}
		case OPTION_SET_BRIGHTNESS: {
			brightness = optarg;
			op_mode = MODE_SET_BRIGHTNESS;
			break;
		}
		case OPTION_FLOOR_EYE_HEIGHT: {
			floor_eye_height = strtof(optarg, NULL);
			if (!(floor_eye_height >= 0.5f && floor_eye_height <= 2.5f)) {
				PE("--floor-eye-height takes an eye height in metres between 0.5 and 2.5.\n");
				exit(1);
			}
			op_mode = MODE_CALIBRATE_FLOOR;
			break;
		}
		case OPTION_FLOOR_DEVICE: {
			floor_device = optarg;
			op_mode = MODE_CALIBRATE_FLOOR;
			break;
		}
		case OPTION_FLOOR_DEVICE_HEIGHT: {
			floor_device_height = strtof(optarg, NULL);
			if (!(floor_device_height >= 0.f && floor_device_height <= 0.5f)) {
				PE("--floor-device-height takes a height in metres between 0 and 0.5.\n");
				exit(1);
			}
			break;
		}
		case '?':
			if (optopt == 's') {
				PE("Option -s requires an id to set.\n");
			} else if (isprint(optopt)) {
				PE("Option `-%c' unknown. Usage:\n", optopt);
				PE("    -c: Recenter local spaces\n");
				PE("    -f <id>: Set focused client\n");
				PE("    -p <id>: Set primary client\n");
				PE("    -i <id>: Toggle whether client receives input\n");
				PE("    --device <id>: Set device for subsequent command, otherwise defaults to the "
				   "primary device\n");
				PE("    --get-brightness: Get current display brightness in percent\n");
				PE("    --set-brightness <[+-]brightness[%%]>: Set display brightness\n");
				PE("    --floor-eye-height <m>: Set the STAGE floor this far below the head; stand "
				   "upright\n");
				PE("    --floor-device <left|right|id>: Set the STAGE floor from a controller resting "
				   "on the floor\n");
				PE("    --floor-device-height <m>: Grip pose height of that resting controller "
				   "(default %.2f)\n",
				   FLOOR_DEFAULT_DEVICE_HEIGHT_M);
			} else {
				PE("Option `\\x%x' unknown.\n", optopt);
			}
			exit(1);
		default: exit(0);
		}
	}

	// Connection struct on the stack, super simple.
	struct ipc_connection ipc_c = {0};

	struct xrt_instance_info info = {
	    .app_info.application_name = "monado-ctl",
	};

	xrt_result_t xret = ipc_client_connection_init(&ipc_c, U_LOGGING_INFO, &info);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("ipc_client_connection_init: %u", xret);
		return -1;
	}

	bool is_system_available = false;
	xret = ipc_call_instance_is_system_available(&ipc_c, &is_system_available);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("ipc_call_instance_is_system_available: %u", xret);
		return -1;
	}
	if (!is_system_available) {
		PE("System isn't available, devices won't be available!");
	}

	switch (op_mode) {
	case MODE_GET: exit(get_mode(&ipc_c)); break;
	case MODE_SET_PRIMARY: exit(set_primary(&ipc_c, s_val)); break;
	case MODE_SET_FOCUSED: exit(set_focused(&ipc_c, s_val)); break;
	case MODE_TOGGLE_IO: exit(toggle_io(&ipc_c, s_val)); break;
	case MODE_RECENTER: exit(recenter_local_spaces(&ipc_c)); break;
	case MODE_GET_BRIGHTNESS: exit(get_brightness(&ipc_c, device_val)); break;
	case MODE_SET_BRIGHTNESS: exit(set_brightness(&ipc_c, device_val, brightness)); break;
	case MODE_CALIBRATE_FLOOR:
		if ((floor_eye_height > 0) == (floor_device != NULL)) {
			PE("Use either --floor-eye-height or --floor-device.\n");
			exit(1);
		}
		exit(calibrate_floor(&ipc_c, floor_eye_height, floor_device, floor_device_height));
		break;
	default: P("Unrecognised operation mode.\n"); exit(1);
	}

	return 0;
}

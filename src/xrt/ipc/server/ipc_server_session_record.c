// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Records a session's head and controller state, and playtest marks.
 *
 * With XRT_SESSION_RECORD_DIR set, a thread samples the head pose and views and
 * every pose and input of the left and right controllers at up to 60 Hz (once per
 * compositor frame) into
 * session.jsonl, in the space overseer's root space. Inputs are written only
 * when they change. Holding both thumbsticks clicked for 0.8 s adds a mark: a
 * line in the recording and the log, and the next composited frame as
 * mark_<n>.ppm. Creating <dir>/mark-request also adds a mark (the file is
 * removed). The recording can be replayed into the remote driver.
 * @ingroup ipc_server
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_pretty_print.h"
#include "util/u_session_mark.h"
#include "util/u_time.h"

#include "server/ipc_server.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

DEBUG_GET_ONCE_OPTION(session_record_dir, "XRT_SESSION_RECORD_DIR", NULL)

#define RECORD_INTERVAL_NS (U_TIME_1S_IN_NS / 60)
#define MARK_HOLD_NS (800 * U_TIME_1MS_IN_NS)
#define MAX_POSES 8
#define MAX_INPUTS 64

struct record_hand
{
	const char *key;
	struct xrt_device *xdev;
	uint32_t pose_count;
	enum xrt_input_name pose_names[MAX_POSES];
	struct xrt_space *pose_spaces[MAX_POSES];
	bool have_last;
	union xrt_input_value last[MAX_INPUTS];
	bool last_active[MAX_INPUTS];
	int thumbstick_click;
};

struct ipc_server_session_record
{
	struct os_thread_helper oth;
	struct ipc_server *server;
	FILE *file;
	char dir[512];
	int64_t start_ns;
	struct xrt_space *head_space;
	struct record_hand hands[2];
	int64_t both_since_ns;
	bool mark_armed;
	uint32_t mark_count;
};

static void
write_pose(FILE *f, const struct xrt_space_relation *rel)
{
	const struct xrt_pose *p = &rel->pose;
	bool valid = (rel->relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0 &&
	             (rel->relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0;
	fprintf(f, "{\"p\":[%.5f,%.5f,%.5f],\"q\":[%.6f,%.6f,%.6f,%.6f],\"v\":%d}", p->position.x, p->position.y,
	        p->position.z, p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w, valid ? 1 : 0);
}

static void
bind_hand(struct ipc_server_session_record *rec, struct record_hand *hand, struct xrt_device *xdev)
{
	if (hand->xdev == xdev) {
		return;
	}
	for (uint32_t i = 0; i < hand->pose_count; i++) {
		xrt_space_reference(&hand->pose_spaces[i], NULL);
	}
	hand->pose_count = 0;
	hand->have_last = false;
	hand->thumbstick_click = -1;
	hand->xdev = xdev;
	if (xdev == NULL) {
		return;
	}
	struct xrt_space_overseer *xso = rec->server->xso;
	for (uint32_t i = 0; i < xdev->input_count && i < MAX_INPUTS; i++) {
		enum xrt_input_name name = xdev->inputs[i].name;
		if (XRT_GET_INPUT_TYPE(name) == XRT_INPUT_TYPE_POSE && hand->pose_count < MAX_POSES) {
			struct xrt_space *space = NULL;
			if (xrt_space_overseer_create_pose_space(xso, xdev, name, &space) == XRT_SUCCESS) {
				hand->pose_names[hand->pose_count] = name;
				hand->pose_spaces[hand->pose_count++] = space;
			}
		}
		if (strstr(u_str_xrt_input_name(name), "THUMBSTICK_CLICK") != NULL) {
			hand->thumbstick_click = (int)i;
		}
	}
	U_LOG_I("Session record: %s hand is '%s' (%u poses, %u inputs)", hand->key, xdev->str, hand->pose_count,
	        xdev->input_count);
}

static bool
write_hand(struct ipc_server_session_record *rec, struct record_hand *hand, int64_t now_ns)
{
	FILE *f = rec->file;
	struct xrt_device *xdev = hand->xdev;
	bool stick = false;

	fprintf(f, ",\"%s\":{\"dev\":\"%s\",\"poses\":{", hand->key, xdev->str);
	for (uint32_t i = 0; i < hand->pose_count; i++) {
		struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
		xrt_space_overseer_locate_space(rec->server->xso, rec->server->xso->semantic.root, &(struct xrt_pose)XRT_POSE_IDENTITY,
		                                now_ns, hand->pose_spaces[i], &(struct xrt_pose)XRT_POSE_IDENTITY, &rel);
		fprintf(f, "%s\"%s\":", i ? "," : "", u_str_xrt_input_name(hand->pose_names[i]));
		write_pose(f, &rel);
	}
	fprintf(f, "}");

	os_mutex_lock(&rec->server->input_lock);
	bool updated = xrt_device_update_inputs(xdev) == XRT_SUCCESS;
	bool first = true;
	for (uint32_t i = 0; updated && i < xdev->input_count && i < MAX_INPUTS; i++) {
		const struct xrt_input *in = &xdev->inputs[i];
		enum xrt_input_type type = XRT_GET_INPUT_TYPE(in->name);
		if (type == XRT_INPUT_TYPE_POSE || type == XRT_INPUT_TYPE_HAND_TRACKING ||
		    type == XRT_INPUT_TYPE_FACE_TRACKING || type == XRT_INPUT_TYPE_BODY_TRACKING) {
			continue;
		}
		if ((int)i == hand->thumbstick_click) {
			stick = in->active && in->value.boolean;
		}
		bool changed = !hand->have_last || hand->last_active[i] != in->active ||
		               memcmp(&hand->last[i], &in->value, sizeof(in->value)) != 0;
		if (!changed) {
			continue;
		}
		hand->last[i] = in->value;
		hand->last_active[i] = in->active;
		fprintf(f, "%s\"%s\":", first ? ",\"in\":{" : ",", u_str_xrt_input_name(in->name));
		first = false;
		switch (type) {
		case XRT_INPUT_TYPE_BOOLEAN: fprintf(f, "%d", in->value.boolean ? 1 : 0); break;
		case XRT_INPUT_TYPE_VEC1_ZERO_TO_ONE:
		case XRT_INPUT_TYPE_VEC1_MINUS_ONE_TO_ONE: fprintf(f, "%.4f", in->value.vec1.x); break;
		case XRT_INPUT_TYPE_VEC2_MINUS_ONE_TO_ONE:
			fprintf(f, "[%.4f,%.4f]", in->value.vec2.x, in->value.vec2.y);
			break;
		default: fprintf(f, "null"); break;
		}
	}
	hand->have_last = hand->have_last || updated;
	os_mutex_unlock(&rec->server->input_lock);
	if (!first) {
		fprintf(f, "}");
	}
	fprintf(f, "}");
	return stick;
}

static void
write_sample(struct ipc_server_session_record *rec)
{
	struct ipc_server *s = rec->server;
	struct xrt_system_devices *xsysd = s->xsysd;
	struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
	struct xrt_device *left = NULL, *right = NULL;
	if (xrt_system_devices_get_roles(xsysd, &roles) == XRT_SUCCESS) {
		if (roles.left >= 0 && (uint32_t)roles.left < xsysd->static_xdev_count) {
			left = xsysd->static_xdevs[roles.left];
		}
		if (roles.right >= 0 && (uint32_t)roles.right < xsysd->static_xdev_count) {
			right = xsysd->static_xdevs[roles.right];
		}
	}
	bind_hand(rec, &rec->hands[0], left);
	bind_hand(rec, &rec->hands[1], right);

	int64_t now_ns = os_monotonic_get_ns();
	FILE *f = rec->file;
	fprintf(f, "{\"t\":%.4f", time_ns_to_s(now_ns - rec->start_ns));
	if (s->xso->semantic.stage != NULL) {
		struct xrt_space_relation stage = XRT_SPACE_RELATION_ZERO;
		xrt_space_overseer_locate_space(s->xso, s->xso->semantic.root, &(struct xrt_pose)XRT_POSE_IDENTITY,
		                                now_ns, s->xso->semantic.stage, &(struct xrt_pose)XRT_POSE_IDENTITY, &stage);
		fprintf(f, ",\"stage\":");
		write_pose(f, &stage);
	}

	struct xrt_device *head = xsysd->static_roles.head;
	if (head != NULL && rec->head_space != NULL) {
		struct xrt_space_relation rel = XRT_SPACE_RELATION_ZERO;
		xrt_space_overseer_locate_space(s->xso, s->xso->semantic.root, &(struct xrt_pose)XRT_POSE_IDENTITY, now_ns,
		                                rec->head_space, &(struct xrt_pose)XRT_POSE_IDENTITY, &rel);
		fprintf(f, ",\"head\":");
		write_pose(f, &rel);

		struct xrt_vec3 eye_relation = {0.063f, 0.0f, 0.0f};
		struct xrt_space_relation head_rel = XRT_SPACE_RELATION_ZERO;
		struct xrt_fov fovs[2] = {0};
		struct xrt_pose poses[2] = {0};
		if (xrt_device_get_view_poses(head, &eye_relation, now_ns, XRT_VIEW_TYPE_STEREO, 2, &head_rel, fovs,
		                              poses) == XRT_SUCCESS) {
			fprintf(f, ",\"views\":[");
			for (int i = 0; i < 2; i++) {
				fprintf(f,
				        "%s{\"fov\":[%.5f,%.5f,%.5f,%.5f],\"p\":[%.5f,%.5f,%.5f],\"q\":[%.6f,%.6f,%.6f,%.6f]}",
				        i ? "," : "", fovs[i].angle_left, fovs[i].angle_right, fovs[i].angle_up,
				        fovs[i].angle_down, poses[i].position.x, poses[i].position.y, poses[i].position.z,
				        poses[i].orientation.x, poses[i].orientation.y, poses[i].orientation.z,
				        poses[i].orientation.w);
			}
			fprintf(f, "]");
		}
	}

	bool sticks[2] = {false, false};
	for (int i = 0; i < 2; i++) {
		if (rec->hands[i].xdev != NULL) {
			sticks[i] = write_hand(rec, &rec->hands[i], now_ns);
		}
	}

	const char *how = NULL;
	bool both = sticks[0] && sticks[1];
	if (!both) {
		rec->both_since_ns = 0;
		rec->mark_armed = true;
	} else if (rec->both_since_ns == 0) {
		rec->both_since_ns = now_ns;
	} else if (rec->mark_armed && now_ns - rec->both_since_ns >= MARK_HOLD_NS) {
		rec->mark_armed = false;
		how = "both thumbsticks held";
	}
	char request[600];
	snprintf(request, sizeof(request), "%s/mark-request", rec->dir);
	if (how == NULL && remove(request) == 0) {
		how = "mark-request file";
	}
	if (how != NULL) {
		uint32_t n = ++rec->mark_count;
		char path[1024];
		snprintf(path, sizeof(path), "%s/mark_%u.ppm", rec->dir, n);
		u_session_mark_request_capture(path);
		fprintf(f, ",\"mark\":%u", n);
		U_LOG_W("SESSION_MARK %u at %.1f s (%s); frame '%s'", n, time_ns_to_s(now_ns - rec->start_ns), how, path);
	}

	fprintf(f, "}\n");
	fflush(f);
}

static void *
run_session_record(void *ptr)
{
	struct ipc_server_session_record *rec = ptr;
	os_thread_name_self("Session record");
#ifdef __APPLE__
	// Without this, macOS coalesces the 16 ms sleeps of a background service thread to ~130 ms.
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

	// One sample per compositor frame, at most every RECORD_INTERVAL_NS. Timed
	// sleeps of a background macOS service stretch to ~130 ms; the frame tick does not.
	int64_t last_ns = 0;
	os_thread_helper_lock(&rec->oth);
	while (os_thread_helper_is_running_locked(&rec->oth)) {
		os_thread_helper_unlock(&rec->oth);
		u_session_mark_wait_frame(RECORD_INTERVAL_NS * 3);
		int64_t now_ns = os_monotonic_get_ns();
		if (now_ns - last_ns >= RECORD_INTERVAL_NS * 9 / 10) {
			write_sample(rec);
			last_ns = now_ns;
		}
		os_thread_helper_lock(&rec->oth);
	}
	os_thread_helper_unlock(&rec->oth);
	return NULL;
}

void
ipc_server_session_record_start(struct ipc_server *s)
{
	const char *dir = debug_get_option_session_record_dir();
	if (dir == NULL || dir[0] == '\0' || s->session_record != NULL || s->xso == NULL || s->xsysd == NULL) {
		return;
	}
	struct ipc_server_session_record *rec = U_TYPED_CALLOC(struct ipc_server_session_record);
	rec->server = s;
	snprintf(rec->dir, sizeof(rec->dir), "%s", dir);
	rec->hands[0].key = "left";
	rec->hands[1].key = "right";
	rec->hands[0].thumbstick_click = rec->hands[1].thumbstick_click = -1;
	rec->mark_armed = true;

	char path[600];
	snprintf(path, sizeof(path), "%s/session.jsonl", dir);
	rec->file = fopen(path, "w");
	if (rec->file == NULL) {
		U_LOG_E("Session record: cannot open '%s'", path);
		free(rec);
		return;
	}

	struct xrt_device *head = s->xsysd->static_roles.head;
	if (head != NULL) {
		xrt_space_overseer_create_pose_space(s->xso, head, XRT_INPUT_GENERIC_HEAD_POSE, &rec->head_space);
	}

	rec->start_ns = os_monotonic_get_ns();
	struct timespec wall;
	clock_gettime(CLOCK_REALTIME, &wall);
	struct tm tm_local;
	localtime_r(&wall.tv_sec, &tm_local);
	char stamp[64];
	strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", &tm_local);
	fprintf(rec->file, "{\"format\":\"monado-session-record\",\"version\":2,\"pose_space\":\"root\",\"start_wall\":\"%s.%03ld\",\"head\":\"%s\"}\n",
	        stamp, wall.tv_nsec / 1000000, head != NULL ? head->str : "");

	if (os_thread_helper_init(&rec->oth) != 0 || os_thread_helper_start(&rec->oth, run_session_record, rec) != 0) {
		U_LOG_E("Session record: failed to start its thread.");
		fclose(rec->file);
		xrt_space_reference(&rec->head_space, NULL);
		free(rec);
		return;
	}
	s->session_record = rec;
	U_LOG_W("Session record: writing '%s' at 60 Hz; hold both thumbsticks 0.8 s to mark a moment.", path);
}

void
ipc_server_session_record_stop(struct ipc_server *s)
{
	struct ipc_server_session_record *rec = s->session_record;
	if (rec == NULL) {
		return;
	}
	os_thread_helper_destroy(&rec->oth);
	for (int h = 0; h < 2; h++) {
		for (uint32_t i = 0; i < rec->hands[h].pose_count; i++) {
			xrt_space_reference(&rec->hands[h].pose_spaces[i], NULL);
		}
	}
	xrt_space_reference(&rec->head_space, NULL);
	fclose(rec->file);
	free(rec);
	s->session_record = NULL;
}

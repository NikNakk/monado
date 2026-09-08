// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Record synchronized PS VR2 mode-4 camera sets for offline calibration.
 */

#include "cli_common.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_frame.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "psvr2/psvr2_interface.h"
#include "util/u_time.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>


#define CALIBRATION_CAMERA_COUNT 4
#define CALIBRATION_PENDING_SLOTS 64
#define CALIBRATION_MAX_QUEUE_DEPTH 32

struct calibration_recorder;

struct calibration_camera_sink
{
	struct xrt_frame_sink base;
	struct calibration_recorder *recorder;
	uint32_t camera_index;
};

struct calibration_pending_set
{
	bool used;
	uint64_t sequence;
	struct xrt_frame *frames[CALIBRATION_CAMERA_COUNT];
	bool pose_sampled;
	bool pose_valid;
	struct xrt_space_relation head_relation;
};

struct calibration_write_job
{
	struct calibration_write_job *next;
	uint64_t set_index;
	uint64_t sequence;
	int64_t exposure_monotonic_ns;
	int64_t exposure_vts_ns;
	bool pose_valid;
	struct xrt_space_relation head_relation;
	struct xrt_frame *frames[CALIBRATION_CAMERA_COUNT];
};

struct calibration_recorder
{
	struct xrt_device *head;
	char output_dir[1024];
	char frames_dir[1024];
	uint32_t sequence_stride;

	struct os_mutex mutex;
	struct os_cond cond;
	struct os_thread writer_thread;
	bool writer_started;
	bool stopping;

	struct calibration_pending_set pending[CALIBRATION_PENDING_SLOTS];
	struct calibration_write_job *queue_head;
	struct calibration_write_job *queue_tail;
	uint32_t queue_depth;

	uint64_t completed_sets;
	uint64_t queued_sets;
	uint64_t written_sets;
	uint64_t dropped_incomplete_sets;
	uint64_t dropped_writer_sets;
	uint64_t pose_failures;

	FILE *manifest;
};

static void
calibration_destroy_system(struct xrt_instance **xi,
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
calibration_mkdir(const char *path)
{
	if (mkdir(path, 0775) == 0 || errno == EEXIST) {
		return true;
	}
	fprintf(stderr, "Could not create calibration directory '%s': %s\n", path, strerror(errno));
	return false;
}

static bool
calibration_write_metadata(const struct calibration_recorder *recorder, const struct xrt_device *head)
{
	char path[1200];
	snprintf(path, sizeof(path), "%s/dataset.json", recorder->output_dir);
	FILE *file = fopen(path, "w");
	if (file == NULL) {
		fprintf(stderr, "Could not create '%s'.\n", path);
		return false;
	}

	fprintf(file,
	        "{\n"
	        "  \"schema_version\": 1,\n"
	        "  \"purpose\": \"psvr2_four_camera_charuco_calibration\",\n"
	        "  \"headset_serial\": \"%s\",\n"
	        "  \"camera_mode\": 4,\n"
	        "  \"camera_count\": 4,\n"
	        "  \"image_format\": \"L8_PGM\",\n"
	        "  \"image_width\": 512,\n"
	        "  \"image_height\": 508,\n"
	        "  \"manifest\": \"manifest.csv\",\n"
	        "  \"time_domains\": {\n"
	        "    \"exposure_monotonic_ns\": \"Monado monotonic timestamp carried by xrt_frame.timestamp\",\n"
	        "    \"exposure_vts_ns\": \"raw PSVR2 VTS timestamp carried by xrt_frame.source_timestamp\"\n"
	        "  },\n"
	        "  \"charuco_target\": {\n"
	        "    \"squares_x\": 7,\n"
	        "    \"squares_y\": 5,\n"
	        "    \"square_length_mm_nominal\": 40.0,\n"
	        "    \"marker_length_mm_nominal\": 30.0,\n"
	        "    \"dictionary\": \"DICT_4X4_50\",\n"
	        "    \"actual_square_length_mm\": null,\n"
	        "    \"note\": \"Measure the printed square size and supply the actual value to the offline calibration tool.\"\n"
	        "  }\n"
	        "}\n",
	        head->serial);

	bool ok = fclose(file) == 0;
	if (!ok) {
		fprintf(stderr, "Failed to finish writing '%s'.\n", path);
	}
	return ok;
}

static bool
calibration_write_pgm(const struct calibration_recorder *recorder,
                      uint64_t set_index,
                      uint32_t camera_index,
                      struct xrt_frame *frame,
                      char relative_path[128])
{
	snprintf(relative_path, 128, "frames/set-%06" PRIu64 "-camera%u.pgm", set_index, camera_index);
	char path[1200];
	snprintf(path, sizeof(path), "%s/%s", recorder->output_dir, relative_path);

	FILE *file = fopen(path, "wb");
	if (file == NULL) {
		fprintf(stderr, "Could not create calibration image '%s'.\n", path);
		return false;
	}

	fprintf(file, "P5\n%u %u\n255\n", frame->width, frame->height);
	bool ok = true;
	for (uint32_t y = 0; y < frame->height; y++) {
		if (fwrite(frame->data + y * frame->stride, 1, frame->width, file) != frame->width) {
			ok = false;
			break;
		}
	}
	ok = fclose(file) == 0 && ok;
	return ok;
}

static void
calibration_release_job(struct calibration_write_job *job)
{
	if (job == NULL) {
		return;
	}
	for (uint32_t i = 0; i < CALIBRATION_CAMERA_COUNT; i++) {
		xrt_frame_reference(&job->frames[i], NULL);
	}
	free(job);
}

static void *
calibration_writer_thread(void *ptr)
{
	struct calibration_recorder *recorder = ptr;

	for (;;) {
		os_mutex_lock(&recorder->mutex);
		while (recorder->queue_head == NULL && !recorder->stopping) {
			os_cond_wait(&recorder->cond, &recorder->mutex);
		}
		if (recorder->queue_head == NULL && recorder->stopping) {
			os_mutex_unlock(&recorder->mutex);
			break;
		}

		struct calibration_write_job *job = recorder->queue_head;
		recorder->queue_head = job->next;
		if (recorder->queue_head == NULL) {
			recorder->queue_tail = NULL;
		}
		recorder->queue_depth--;
		os_mutex_unlock(&recorder->mutex);

		char relative_paths[CALIBRATION_CAMERA_COUNT][128] = {{0}};
		bool images_ok = true;
		for (uint32_t i = 0; i < CALIBRATION_CAMERA_COUNT; i++) {
			images_ok = calibration_write_pgm(recorder, job->set_index, i, job->frames[i], relative_paths[i]) && images_ok;
		}

		if (images_ok) {
			const struct xrt_pose *pose = &job->head_relation.pose;
			fprintf(recorder->manifest,
			        "%" PRIu64 ",%" PRIu64 ",%" PRIi64 ",%" PRIi64 ",%u,%u,"
			        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%s,%s,%s,%s\n",
			        job->set_index, job->sequence, job->exposure_monotonic_ns, job->exposure_vts_ns,
			        job->pose_valid ? 1U : 0U, (unsigned int)job->head_relation.relation_flags,
			        pose->position.x, pose->position.y, pose->position.z, pose->orientation.x, pose->orientation.y,
			        pose->orientation.z, pose->orientation.w, relative_paths[0], relative_paths[1], relative_paths[2],
			        relative_paths[3]);
			fflush(recorder->manifest);
			os_mutex_lock(&recorder->mutex);
			recorder->written_sets++;
			os_mutex_unlock(&recorder->mutex);
		} else {
			fprintf(stderr, "Failed to write one or more images for calibration set %" PRIu64 ".\n", job->set_index);
		}

		calibration_release_job(job);
	}

	return NULL;
}

static void
calibration_clear_pending(struct calibration_pending_set *pending)
{
	for (uint32_t i = 0; i < CALIBRATION_CAMERA_COUNT; i++) {
		xrt_frame_reference(&pending->frames[i], NULL);
	}
	memset(pending, 0, sizeof(*pending));
}

static struct calibration_pending_set *
calibration_find_pending_locked(struct calibration_recorder *recorder, uint64_t sequence)
{
	struct calibration_pending_set *empty = NULL;
	struct calibration_pending_set *oldest = NULL;
	for (uint32_t i = 0; i < CALIBRATION_PENDING_SLOTS; i++) {
		struct calibration_pending_set *pending = &recorder->pending[i];
		if (pending->used && pending->sequence == sequence) {
			return pending;
		}
		if (!pending->used && empty == NULL) {
			empty = pending;
		}
		if (pending->used && (oldest == NULL || pending->sequence < oldest->sequence)) {
			oldest = pending;
		}
	}

	if (empty != NULL) {
		return empty;
	}

	if (oldest != NULL) {
		recorder->dropped_incomplete_sets++;
		calibration_clear_pending(oldest);
	}
	return oldest;
}

static bool
calibration_pending_complete(const struct calibration_pending_set *pending)
{
	for (uint32_t i = 0; i < CALIBRATION_CAMERA_COUNT; i++) {
		if (pending->frames[i] == NULL) {
			return false;
		}
	}
	return true;
}

static void
calibration_camera_push(struct xrt_frame_sink *xfs, struct xrt_frame *frame)
{
	struct calibration_camera_sink *sink = container_of(xfs, struct calibration_camera_sink, base);
	struct calibration_recorder *recorder = sink->recorder;
	const uint32_t camera_index = sink->camera_index;

	if (frame->source_id != camera_index || frame->width != 512 || frame->height != 508 ||
	    recorder->sequence_stride == 0 || frame->source_sequence % recorder->sequence_stride != 0) {
		return;
	}

	bool pose_sampled = false;
	bool pose_valid = false;
	struct xrt_space_relation head_relation = XRT_SPACE_RELATION_ZERO;
	if (camera_index == 0 && recorder->head->get_tracked_pose != NULL) {
		pose_sampled = true;
		xrt_result_t xret = recorder->head->get_tracked_pose(recorder->head, XRT_INPUT_GENERIC_HEAD_POSE,
		                                                   frame->timestamp, &head_relation);
		pose_valid = xret == XRT_SUCCESS &&
		             (head_relation.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0 &&
		             (head_relation.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0;
	}

	os_mutex_lock(&recorder->mutex);
	struct calibration_pending_set *pending = calibration_find_pending_locked(recorder, frame->source_sequence);
	if (pending == NULL) {
		os_mutex_unlock(&recorder->mutex);
		return;
	}
	if (!pending->used) {
		pending->used = true;
		pending->sequence = frame->source_sequence;
	}

	if (pending->frames[camera_index] == NULL) {
		xrt_frame_reference(&pending->frames[camera_index], frame);
	}
	if (pose_sampled) {
		pending->pose_sampled = true;
		pending->pose_valid = pose_valid;
		pending->head_relation = head_relation;
		if (!pose_valid) {
			recorder->pose_failures++;
		}
	}

	if (!calibration_pending_complete(pending)) {
		os_mutex_unlock(&recorder->mutex);
		return;
	}

	recorder->completed_sets++;
	if (recorder->queue_depth >= CALIBRATION_MAX_QUEUE_DEPTH) {
		recorder->dropped_writer_sets++;
		calibration_clear_pending(pending);
		os_mutex_unlock(&recorder->mutex);
		return;
	}

	struct calibration_write_job *job = calloc(1, sizeof(*job));
	if (job == NULL) {
		recorder->dropped_writer_sets++;
		calibration_clear_pending(pending);
		os_mutex_unlock(&recorder->mutex);
		return;
	}

	job->set_index = recorder->queued_sets++;
	job->sequence = pending->sequence;
	job->exposure_monotonic_ns = pending->frames[0]->timestamp;
	job->exposure_vts_ns = pending->frames[0]->source_timestamp;
	job->pose_valid = pending->pose_sampled && pending->pose_valid;
	job->head_relation = pending->head_relation;
	for (uint32_t i = 0; i < CALIBRATION_CAMERA_COUNT; i++) {
		job->frames[i] = pending->frames[i];
		pending->frames[i] = NULL;
	}
	memset(pending, 0, sizeof(*pending));

	if (recorder->queue_tail != NULL) {
		recorder->queue_tail->next = job;
	} else {
		recorder->queue_head = job;
	}
	recorder->queue_tail = job;
	recorder->queue_depth++;
	os_cond_signal(&recorder->cond);
	os_mutex_unlock(&recorder->mutex);
}

static bool
calibration_recorder_init(struct calibration_recorder *recorder,
                          struct xrt_device *head,
                          const char *output_dir,
                          uint32_t sequence_stride)
{
	memset(recorder, 0, sizeof(*recorder));
	recorder->head = head;
	recorder->sequence_stride = sequence_stride;
	snprintf(recorder->output_dir, sizeof(recorder->output_dir), "%s", output_dir);
	snprintf(recorder->frames_dir, sizeof(recorder->frames_dir), "%s/frames", output_dir);

	if (!calibration_mkdir(recorder->output_dir) || !calibration_mkdir(recorder->frames_dir)) {
		return false;
	}

	char manifest_path[1200];
	snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.csv", recorder->output_dir);
	recorder->manifest = fopen(manifest_path, "w");
	if (recorder->manifest == NULL) {
		fprintf(stderr, "Could not create calibration manifest '%s'.\n", manifest_path);
		return false;
	}
	fprintf(recorder->manifest,
	        "set_index,sequence_id,exposure_monotonic_ns,exposure_vts_ns,pose_valid,relation_flags,"
	        "head_px,head_py,head_pz,head_qx,head_qy,head_qz,head_qw,camera0_file,camera1_file,camera2_file,"
	        "camera3_file\n");
	fflush(recorder->manifest);

	if (!calibration_write_metadata(recorder, head)) {
		fclose(recorder->manifest);
		recorder->manifest = NULL;
		return false;
	}

	if (os_mutex_init(&recorder->mutex) != 0 || os_cond_init(&recorder->cond) != 0 ||
	    os_thread_init(&recorder->writer_thread) != 0) {
		fprintf(stderr, "Failed to initialize calibration writer synchronization.\n");
		if (recorder->manifest != NULL) {
			fclose(recorder->manifest);
			recorder->manifest = NULL;
		}
		return false;
	}
	if (os_thread_start(&recorder->writer_thread, calibration_writer_thread, recorder) != 0) {
		fprintf(stderr, "Failed to start calibration writer thread.\n");
		os_cond_destroy(&recorder->cond);
		os_mutex_destroy(&recorder->mutex);
		fclose(recorder->manifest);
		recorder->manifest = NULL;
		return false;
	}
	recorder->writer_started = true;
	return true;
}

static void
calibration_recorder_finish(struct calibration_recorder *recorder)
{
	if (recorder->writer_started) {
		os_mutex_lock(&recorder->mutex);
		recorder->stopping = true;
		os_cond_broadcast(&recorder->cond);
		os_mutex_unlock(&recorder->mutex);
		os_thread_join(&recorder->writer_thread);
		recorder->writer_started = false;
	}

	for (uint32_t i = 0; i < CALIBRATION_PENDING_SLOTS; i++) {
		if (recorder->pending[i].used) {
			calibration_clear_pending(&recorder->pending[i]);
		}
	}
	while (recorder->queue_head != NULL) {
		struct calibration_write_job *job = recorder->queue_head;
		recorder->queue_head = job->next;
		calibration_release_job(job);
	}

	if (recorder->manifest != NULL) {
		fclose(recorder->manifest);
		recorder->manifest = NULL;
	}
	os_cond_destroy(&recorder->cond);
	os_mutex_destroy(&recorder->mutex);
	os_thread_destroy(&recorder->writer_thread);
}

static bool
parse_long_arg(const char *text, long minimum, long maximum, long *out_value)
{
	errno = 0;
	char *end = NULL;
	long value = strtol(text, &end, 10);
	if (errno != 0 || end == text || *end != '\0' || value < minimum || value > maximum) {
		return false;
	}
	*out_value = value;
	return true;
}

int
cli_cmd_psvr2_calibration_record(int argc, const char **argv)
{
	if (argc < 3 || argc > 5) {
		fprintf(stderr,
		        "Usage: %s %s <output-dir> [duration-seconds: 1-600] [sequence-stride: 1-120]\n",
		        argv[0], argv[1]);
		return EXIT_FAILURE;
	}

	long duration_s = 30;
	long stride = 6;
	if (argc >= 4 && !parse_long_arg(argv[3], 1, 600, &duration_s)) {
		fprintf(stderr, "Invalid duration '%s'.\n", argv[3]);
		return EXIT_FAILURE;
	}
	if (argc >= 5 && !parse_long_arg(argv[4], 1, 120, &stride)) {
		fprintf(stderr, "Invalid sequence stride '%s'.\n", argv[4]);
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
		calibration_destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	struct xrt_device *head = xsysd != NULL ? xsysd->static_roles.head : NULL;
	struct psvr2_camera_diagnostics diag = {0};
	if (head == NULL || !psvr2_get_camera_diagnostics(head, &diag)) {
		fprintf(stderr, "No PS VR2 HMD was discovered.\n");
		calibration_destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}
	if (!diag.enabled || diag.configured_mode != 4) {
		fprintf(stderr,
		        "Calibration recording requires mode 4. Set PSVR2_CAMERA_STREAMS=1 PSVR2_CAMERA_MODE=4.\n");
		calibration_destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	struct calibration_recorder recorder;
	if (!calibration_recorder_init(&recorder, head, argv[2], (uint32_t)stride)) {
		calibration_destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	struct calibration_camera_sink sinks[CALIBRATION_CAMERA_COUNT] = {0};
	struct xrt_frame_sink *frame_sinks[CALIBRATION_CAMERA_COUNT] = {0};
	for (uint32_t i = 0; i < CALIBRATION_CAMERA_COUNT; i++) {
		sinks[i].base.push_frame = calibration_camera_push;
		sinks[i].recorder = &recorder;
		sinks[i].camera_index = i;
		frame_sinks[i] = &sinks[i].base;
	}
	if (!psvr2_set_camera_frame_sinks(head, frame_sinks)) {
		fprintf(stderr, "Failed to attach PS VR2 calibration frame sinks.\n");
		calibration_recorder_finish(&recorder);
		calibration_destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	uint64_t first_packet_count = diag.frame_count;
	fprintf(stderr,
	        "Recording synchronized PS VR2 mode-4 calibration sets for %ld seconds to '%s' (sequence stride %ld).\n",
	        duration_s, argv[2], stride);

	int64_t end_ns = os_monotonic_get_ns() + duration_s * U_TIME_1S_IN_NS;
	int64_t next_progress_ns = os_monotonic_get_ns() + U_TIME_1S_IN_NS;
	while (os_monotonic_get_ns() < end_ns) {
		os_nanosleep(10 * U_TIME_1MS_IN_NS);
		int64_t now_ns = os_monotonic_get_ns();
		if (now_ns >= next_progress_ns) {
			os_mutex_lock(&recorder.mutex);
			uint64_t completed = recorder.completed_sets;
			uint64_t written = recorder.written_sets;
			uint32_t queued = recorder.queue_depth;
			os_mutex_unlock(&recorder.mutex);
			fprintf(stderr, "  completed=%" PRIu64 " written=%" PRIu64 " writer_queue=%u\n", completed, written,
			        queued);
			next_progress_ns += U_TIME_1S_IN_NS;
		}
	}

	struct xrt_frame_sink *null_sinks[CALIBRATION_CAMERA_COUNT] = {0};
	(void)psvr2_set_camera_frame_sinks(head, null_sinks);
	(void)psvr2_get_camera_diagnostics(head, &diag);
	calibration_recorder_finish(&recorder);

	fprintf(stderr,
	        "Calibration recording complete: packets=%" PRIu64 ", complete_sets=%" PRIu64 ", written=%" PRIu64
	        ", incomplete_drops=%" PRIu64 ", writer_drops=%" PRIu64 ", pose_failures=%" PRIu64 ".\n",
	        diag.frame_count - first_packet_count, recorder.completed_sets, recorder.written_sets,
	        recorder.dropped_incomplete_sets, recorder.dropped_writer_sets, recorder.pose_failures);
	fprintf(stderr, "Dataset: %s/dataset.json\nManifest: %s/manifest.csv\n", argv[2], argv[2]);

	calibration_destroy_system(&xi, &xsys, &xsysd, &xso);
	return recorder.written_sets > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

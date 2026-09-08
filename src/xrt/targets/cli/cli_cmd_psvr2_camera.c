// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Dump raw PS VR2 camera packet metadata without assigning exposure timestamps.
 */

#include "cli_common.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"

#include "constellation/t_rift_blobwatch.h"
#include "os/os_time.h"
#include "psvr2/psvr2_interface.h"
#include "tracking/t_constellation.h"
#include "util/u_debug.h"
#include "util/u_sink.h"
#include "util/u_time.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>


DEBUG_GET_ONCE_BOOL_OPTION(psvr2_camera_blobs, "PSVR2_CAMERA_BLOBS", false)
DEBUG_GET_ONCE_NUM_OPTION(psvr2_blob_pixel_threshold, "PSVR2_BLOB_PIXEL_THRESHOLD", 0x50)
DEBUG_GET_ONCE_NUM_OPTION(psvr2_blob_required_threshold, "PSVR2_BLOB_REQUIRED_THRESHOLD", 0xb4)
DEBUG_GET_ONCE_NUM_OPTION(psvr2_blob_max_width, "PSVR2_BLOB_MAX_WIDTH", 50)


static long
clamp_long(long value, long minimum, long maximum)
{
	return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static void
camera_destroy_system(struct xrt_instance **xi,
                      struct xrt_system **xsys,
                      struct xrt_system_devices **xsysd,
                      struct xrt_space_overseer **xso)
{
	xrt_space_overseer_destroy(xso);
	xrt_system_devices_destroy(xsysd);
	xrt_system_destroy(xsys);
	xrt_instance_destroy(xi);
}

struct camera_snapshot_sink
{
	struct xrt_frame_sink base;
	atomic_bool ready;
	uint8_t *data;
	uint32_t width;
	uint32_t height;
	uint64_t first_sequence;
	bool have_first_sequence;
	uint64_t source_sequence;
};

struct camera_blob_sink
{
	struct t_blob_sink base;
	atomic_uint_fast64_t observation_count;
	atomic_uint_fast64_t observations_with_blobs;
	atomic_uint_fast64_t total_blobs;
	atomic_uint_fast32_t max_blobs;
	FILE *file;
};

static void
camera_blob_push(struct t_blob_sink *tbs, struct t_blob_observation *observation)
{
	struct camera_blob_sink *sink = container_of(tbs, struct camera_blob_sink, base);
	atomic_fetch_add_explicit(&sink->observation_count, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&sink->total_blobs, observation->num_blobs, memory_order_relaxed);
	if (observation->num_blobs > 0) {
		atomic_fetch_add_explicit(&sink->observations_with_blobs, 1, memory_order_relaxed);
	}

	uint_fast32_t old_max = atomic_load_explicit(&sink->max_blobs, memory_order_relaxed);
	while (old_max < observation->num_blobs &&
	       !atomic_compare_exchange_weak_explicit(&sink->max_blobs, &old_max, observation->num_blobs,
	                                              memory_order_relaxed, memory_order_relaxed)) {}

	if (sink->file == NULL) {
		return;
	}

	if (observation->num_blobs == 0) {
		fprintf(sink->file, "%" PRIi64 ",%" PRIu64 ",0,,,,,,,,\n", observation->timestamp_ns,
		        observation->id);
		return;
	}

	for (uint32_t i = 0; i < observation->num_blobs; i++) {
		const struct t_blob *blob = &observation->blobs[i];
		fprintf(sink->file, "%" PRIi64 ",%" PRIu64 ",%u,%u,%.3f,%.3f,%d,%d,%d,%d,%.6f\n",
		        observation->timestamp_ns, observation->id, observation->num_blobs, blob->blob_id,
		        blob->center.x, blob->center.y, blob->bounding_box.offset.w, blob->bounding_box.offset.h,
		        blob->bounding_box.extent.w, blob->bounding_box.extent.h, blob->brightness);
	}
}

static void
camera_blob_files_open(const char *prefix, struct camera_blob_sink sinks[4])
{
	if (prefix == NULL) {
		return;
	}

	for (size_t i = 0; i < 4; i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s-camera%zu-blobs.csv", prefix, i);
		sinks[i].file = fopen(path, "w");
		if (sinks[i].file == NULL) {
			fprintf(stderr, "Could not open blob trace '%s'.\n", path);
			continue;
		}
		fprintf(sinks[i].file,
		        "timestamp_ns,observation_id,blob_count,blob_id,center_x,center_y,bbox_x,bbox_y,bbox_width,"
		        "bbox_height,brightness\n");
	}
}

static void
camera_blob_files_close(struct camera_blob_sink sinks[4])
{
	for (size_t i = 0; i < 4; i++) {
		if (sinks[i].file != NULL) {
			fclose(sinks[i].file);
			sinks[i].file = NULL;
		}
	}
}

static void
camera_snapshot_push(struct xrt_frame_sink *xfs, struct xrt_frame *frame)
{
	struct camera_snapshot_sink *sink = container_of(xfs, struct camera_snapshot_sink, base);
	if (atomic_load_explicit(&sink->ready, memory_order_acquire)) {
		return;
	}
	if (!sink->have_first_sequence) {
		sink->first_sequence = frame->source_sequence;
		sink->have_first_sequence = true;
		return;
	}
	/* Let controller LED synchronization settle before taking the diagnostic snapshot. */
	if (frame->source_sequence - sink->first_sequence < 60) {
		return;
	}

	uint8_t *data = malloc(frame->width * frame->height);
	if (data == NULL) {
		return;
	}
	for (uint32_t y = 0; y < frame->height; y++) {
		memcpy(data + y * frame->width, frame->data + y * frame->stride, frame->width);
	}
	sink->data = data;
	sink->width = frame->width;
	sink->height = frame->height;
	sink->source_sequence = frame->source_sequence;
	atomic_store_explicit(&sink->ready, true, memory_order_release);
}

static bool
write_camera_snapshot(const char *prefix, size_t index, const struct camera_snapshot_sink *sink)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s-camera%zu.pgm", prefix, index);
	FILE *file = fopen(path, "wb");
	if (file == NULL) {
		fprintf(stderr, "Could not open camera snapshot '%s'.\n", path);
		return false;
	}
	fprintf(file, "P5\n%u %u\n255\n", sink->width, sink->height);
	size_t size = sink->width * sink->height;
	bool success = fwrite(sink->data, 1, size, file) == size;
	success = fclose(file) == 0 && success;
	fprintf(stderr, "%s camera %zu sequence %" PRIu64 " to %s\n", success ? "Wrote" : "Failed to write",
	        index, sink->source_sequence, path);
	return success;
}

int
cli_cmd_psvr2_camera(int argc, const char **argv)
{
	long duration_s = 5;
	if (argc > 4) {
		fprintf(stderr, "Usage: %s %s [duration-seconds: 1-120] [snapshot-prefix]\n", argv[0], argv[1]);
		return EXIT_FAILURE;
	}
	if (argc >= 3) {
		errno = 0;
		char *end = NULL;
		duration_s = strtol(argv[2], &end, 10);
		if (errno != 0 || end == argv[2] || *end != '\0' || duration_s < 1 || duration_s > 120) {
			fprintf(stderr, "Usage: %s %s [duration-seconds: 1-120]\n", argv[0], argv[1]);
			return EXIT_FAILURE;
		}
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
		camera_destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	struct xrt_device *head = xsysd != NULL ? xsysd->static_roles.head : NULL;
	struct psvr2_camera_diagnostics diag = {0};
	if (head == NULL || !psvr2_get_camera_diagnostics(head, &diag)) {
		fprintf(stderr, "No PS VR2 HMD was discovered.\n");
		camera_destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}
	if (!diag.enabled) {
		fprintf(stderr,
		        "PS VR2 camera streaming is disabled. Set PSVR2_CAMERA_STREAMS=1 and optionally "
		        "PSVR2_CAMERA_MODE=<1-16>.\n");
		camera_destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	struct camera_snapshot_sink snapshot_sinks[4] = {0};
	struct camera_blob_sink blob_sinks[4] = {0};
	struct t_blobwatch *blobwatches[4] = {0};
	struct xrt_frame_context blob_xfctx = {0};
	struct xrt_frame_sink *frame_sinks[4] = {0};
	bool blob_diag = debug_get_bool_option_psvr2_camera_blobs();
	const char *snapshot_prefix = argc >= 4 ? argv[3] : NULL;
	if (blob_diag) {
		camera_blob_files_open(snapshot_prefix, blob_sinks);
	}
	for (size_t i = 0; i < 4; i++) {
		snapshot_sinks[i].base.push_frame = camera_snapshot_push;
		frame_sinks[i] = &snapshot_sinks[i].base;

		if (blob_diag) {
			blob_sinks[i].base.push_blobs = camera_blob_push;
			struct t_rift_blobwatch_params params = {
			    .pixel_threshold =
			        (uint8_t)clamp_long(debug_get_num_option_psvr2_blob_pixel_threshold(), 0, 255),
			    .blob_required_threshold =
			        (uint8_t)clamp_long(debug_get_num_option_psvr2_blob_required_threshold(), 0, 255),
			    .max_match_dist = 50.0f,
			    .max_blob_width =
			        (uint16_t)clamp_long(debug_get_num_option_psvr2_blob_max_width(), 1, UINT16_MAX),
			};
			struct xrt_frame_sink *blob_frame_sink = NULL;
			if (t_rift_blobwatch_create(&params, &blob_xfctx, &blob_sinks[i].base, &blob_frame_sink,
			                            &blobwatches[i]) != 0 ||
			    !u_sink_simple_queue_create(&blob_xfctx, blob_frame_sink, &blob_frame_sink)) {
				fprintf(stderr, "Failed to create blob detector pipeline for camera %zu.\n", i);
				xrt_frame_context_destroy_nodes(&blob_xfctx);
				camera_blob_files_close(blob_sinks);
				camera_destroy_system(&xi, &xsys, &xsysd, &xso);
				return EXIT_FAILURE;
			}
			u_sink_split_create(&blob_xfctx, frame_sinks[i], blob_frame_sink, &frame_sinks[i]);
		}
	}
	if (!psvr2_set_camera_frame_sinks(head, frame_sinks)) {
		fprintf(stderr, "Failed to attach PS VR2 camera snapshot sinks.\n");
		xrt_frame_context_destroy_nodes(&blob_xfctx);
		camera_blob_files_close(blob_sinks);
		camera_destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	fprintf(stderr, "Capturing PS VR2 camera mode 0x%x for %ld seconds.\n", diag.configured_mode, duration_s);
	printf("frame_count,arrival_ns,arrival_interval_ns,packet_size,vts_us,sequence_id,vts_monotonic_ns,arrival_age_ns,"
	       "camera_set,image_width,image_height,vi_signature,header_hex\n");

	uint64_t last_frame_count = 0;
	int64_t end_ns = os_monotonic_get_ns() + duration_s * U_TIME_1S_IN_NS;
	while (os_monotonic_get_ns() < end_ns) {
		if (!psvr2_get_camera_diagnostics(head, &diag)) {
			break;
		}
		if (diag.frame_count != 0 && diag.frame_count != last_frame_count) {
			int64_t arrival_age_ns =
			    diag.last_vts_monotonic_ns == 0 ? -1 : diag.last_arrival_ns - diag.last_vts_monotonic_ns;
			printf("%" PRIu64 ",%" PRIi64 ",%" PRIi64 ",%u,%u,%u,%" PRIi64 ",%" PRIi64 ",%u,%u,%u,%d,",
			       diag.frame_count, diag.last_arrival_ns, diag.last_interval_ns, diag.last_packet_size,
			       diag.last_vts_us, diag.last_sequence_id, diag.last_vts_monotonic_ns, arrival_age_ns, diag.last_camera_set,
			       diag.last_image_width, diag.last_image_height,
			       diag.last_header_size >= 2 && diag.last_header[0] == 'V' && diag.last_header[1] == 'I');
			for (uint32_t i = 0; i < diag.last_header_size; i++) {
				printf("%02x", diag.last_header[i]);
			}
			printf("\n");
			last_frame_count = diag.frame_count;
		}
		os_nanosleep(U_TIME_1MS_IN_NS);
	}

	fflush(stdout);
	fprintf(stderr, "Received %" PRIu64 " packets (%" PRIu64 " with VI signature).\n", diag.frame_count,
	        diag.vi_signature_count);
	for (size_t i = 0; i < PSVR2_CAMERA_DIAGNOSTIC_SIZE_SLOTS && diag.packet_sizes[i].size != 0; i++) {
		fprintf(stderr, "  %u bytes: %" PRIu64 " packets\n", diag.packet_sizes[i].size,
		        diag.packet_sizes[i].count);
	}
	if (argc >= 4) {
		for (size_t i = 0; i < 4; i++) {
			if (atomic_load_explicit(&snapshot_sinks[i].ready, memory_order_acquire)) {
				(void)write_camera_snapshot(argv[3], i, &snapshot_sinks[i]);
			} else {
				fprintf(stderr, "No image arrived for camera %zu.\n", i);
			}
		}
	}

	(void)psvr2_set_camera_frame_sinks(head, NULL);
	xrt_frame_context_destroy_nodes(&blob_xfctx);
	if (blob_diag) {
		for (size_t i = 0; i < 4; i++) {
			uint64_t observations = atomic_load_explicit(&blob_sinks[i].observation_count, memory_order_relaxed);
			uint64_t with_blobs =
			    atomic_load_explicit(&blob_sinks[i].observations_with_blobs, memory_order_relaxed);
			uint64_t total = atomic_load_explicit(&blob_sinks[i].total_blobs, memory_order_relaxed);
			uint32_t max_blobs = atomic_load_explicit(&blob_sinks[i].max_blobs, memory_order_relaxed);
			fprintf(stderr,
			        "Camera %zu blobs: %" PRIu64 " observations, %" PRIu64 " with blobs, %" PRIu64
			        " total, %u maximum.\n",
			        i, observations, with_blobs, total, max_blobs);
		}
	}
	camera_blob_files_close(blob_sinks);
	camera_destroy_system(&xi, &xsys, &xsysd, &xso);
	for (size_t i = 0; i < 4; i++) {
		free(snapshot_sinks[i].data);
	}
	return diag.frame_count > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

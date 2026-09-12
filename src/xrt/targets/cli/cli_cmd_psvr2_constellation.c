// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/* Opt-in live probe for the provisional PS VR2 mode-4 constellation calibration. */

#include "cli_common.h"

#include "xrt/xrt_config_drivers.h"

#ifdef XRT_BUILD_DRIVER_PSSENSE

#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_system.h"

#include "constellation/t_constellation_tracker.h"
#include "constellation/t_rift_blobwatch.h"
#include "os/os_time.h"
#include "pssense/pssense_interface.h"
#include "psvr2/psvr2_interface.h"
#include "util/u_debug.h"
#include "util/u_file.h"
#include "util/u_json.h"
#include "util/u_sink.h"
#include "util/u_time.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>


DEBUG_GET_ONCE_NUM_OPTION(psvr2_constellation_blob_pixel_threshold, "PSVR2_BLOB_PIXEL_THRESHOLD", 0x50)
DEBUG_GET_ONCE_NUM_OPTION(psvr2_constellation_blob_required_threshold, "PSVR2_BLOB_REQUIRED_THRESHOLD", 0xb4)
DEBUG_GET_ONCE_NUM_OPTION(psvr2_constellation_blob_max_width, "PSVR2_BLOB_MAX_WIDTH", 50)
DEBUG_GET_ONCE_NUM_OPTION(psvr2_constellation_capture_stride, "PSVR2_CONSTELLATION_CAPTURE_STRIDE", 1)


#define CONSTELLATION_CAMERA_COUNT 4

struct constellation_capture_sink
{
	struct xrt_frame_sink base;
	char output_dir[1024];
	FILE *manifest;
	uint32_t camera_index;
	uint32_t sequence_stride;
	uint64_t received_frames;
	uint64_t written_frames;
	uint64_t failed_frames;
};

static bool
constellation_mkdir(const char *path)
{
	if (mkdir(path, 0775) == 0 || errno == EEXIST) {
		return true;
	}
	fprintf(stderr, "Could not create constellation capture directory '%s': %s\n", path, strerror(errno));
	return false;
}

static void
constellation_capture_push(struct xrt_frame_sink *xfs, struct xrt_frame *frame)
{
	struct constellation_capture_sink *sink = container_of(xfs, struct constellation_capture_sink, base);
	sink->received_frames++;
	if (frame->source_id != sink->camera_index || frame->width != 512 || frame->height != 508 ||
	    frame->source_sequence % sink->sequence_stride != 0) {
		return;
	}

	char relative_path[160];
	snprintf(relative_path, sizeof(relative_path), "frames/camera%u-sequence-%010" PRIu64 ".pgm",
	         sink->camera_index, frame->source_sequence);
	char path[1200];
	snprintf(path, sizeof(path), "%s/%s", sink->output_dir, relative_path);
	FILE *file = fopen(path, "wb");
	if (file == NULL) {
		sink->failed_frames++;
		return;
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
	if (!ok) {
		sink->failed_frames++;
		return;
	}

	fprintf(sink->manifest, "%u,%" PRIu64 ",%" PRIi64 ",%" PRIi64 ",%u,%u,%zu,%s\n",
	        sink->camera_index, frame->source_sequence, frame->timestamp, frame->source_timestamp, frame->width,
	        frame->height, frame->stride, relative_path);
	fflush(sink->manifest);
	sink->written_frames++;
}

static bool
constellation_capture_init(struct constellation_capture_sink sinks[CONSTELLATION_CAMERA_COUNT],
                           const char *output_dir,
                           uint32_t sequence_stride,
                           const char *calibration_path,
                           const struct xrt_device *head,
                           uint8_t pixel_threshold,
                           uint8_t required_threshold,
                           uint16_t max_width)
{
	char frames_dir[1200];
	snprintf(frames_dir, sizeof(frames_dir), "%s/frames", output_dir);
	if (!constellation_mkdir(output_dir) || !constellation_mkdir(frames_dir)) {
		return false;
	}

	char metadata_path[1200];
	snprintf(metadata_path, sizeof(metadata_path), "%s/dataset.json", output_dir);
	FILE *metadata = fopen(metadata_path, "wx");
	if (metadata == NULL) {
		fprintf(stderr, "Refusing to replace existing capture metadata '%s'.\n", metadata_path);
		return false;
	}
	const char *tracker_dataset = getenv("CONSTELLATION_TRACKER_DATA_RECORDER_OUTPUT");
	fprintf(metadata,
	        "{\n"
	        "  \"schema_version\": 1,\n"
	        "  \"purpose\": \"psvr2_mode4_constellation_diagnostic\",\n"
	        "  \"headset_serial\": \"%s\",\n"
	        "  \"calibration\": \"%s\",\n"
	        "  \"camera_mode\": 4,\n"
	        "  \"camera_count\": 4,\n"
	        "  \"image_format\": \"L8_PGM\",\n"
	        "  \"image_width\": 512,\n"
	        "  \"image_height\": 508,\n"
	        "  \"sequence_stride\": %u,\n"
	        "  \"blob_pixel_threshold\": %u,\n"
	        "  \"blob_required_threshold\": %u,\n"
	        "  \"blob_max_width\": %u,\n"
	        "  \"tracker_dataset\": %s%s%s,\n"
	        "  \"manifests\": [\"camera0.csv\", \"camera1.csv\", \"camera2.csv\", \"camera3.csv\"]\n"
	        "}\n",
	        head->serial, calibration_path, sequence_stride, pixel_threshold, required_threshold, max_width,
	        tracker_dataset != NULL && tracker_dataset[0] != '\0' ? "\"" : "null",
	        tracker_dataset != NULL && tracker_dataset[0] != '\0' ? tracker_dataset : "",
	        tracker_dataset != NULL && tracker_dataset[0] != '\0' ? "\"" : "");
	if (fclose(metadata) != 0) {
		fprintf(stderr, "Failed to finish capture metadata '%s'.\n", metadata_path);
		return false;
	}

	for (uint32_t i = 0; i < CONSTELLATION_CAMERA_COUNT; i++) {
		struct constellation_capture_sink *sink = &sinks[i];
		sink->base.push_frame = constellation_capture_push;
		sink->camera_index = i;
		sink->sequence_stride = sequence_stride;
		snprintf(sink->output_dir, sizeof(sink->output_dir), "%s", output_dir);
		char manifest_path[1200];
		snprintf(manifest_path, sizeof(manifest_path), "%s/camera%u.csv", output_dir, i);
		sink->manifest = fopen(manifest_path, "wx");
		if (sink->manifest == NULL) {
			fprintf(stderr, "Refusing to replace existing capture manifest '%s'.\n", manifest_path);
			for (uint32_t j = 0; j < i; j++) {
				fclose(sinks[j].manifest);
				sinks[j].manifest = NULL;
			}
			return false;
		}
		fprintf(sink->manifest,
		        "camera,source_sequence,exposure_monotonic_ns,exposure_vts_ns,width,height,stride,file\n");
		fflush(sink->manifest);
	}
	return true;
}

static void
constellation_capture_finish(struct constellation_capture_sink sinks[CONSTELLATION_CAMERA_COUNT])
{
	for (uint32_t i = 0; i < CONSTELLATION_CAMERA_COUNT; i++) {
		if (sinks[i].manifest != NULL) {
			fclose(sinks[i].manifest);
			sinks[i].manifest = NULL;
		}
	}
}


static long
clamp_long(long value, long minimum, long maximum)
{
	return value < minimum ? minimum : (value > maximum ? maximum : value);
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
get_number(const cJSON *object, const char *name, double *out_value)
{
	return object != NULL && u_json_get_double(u_json_get(object, name), out_value);
}

static bool
load_camera(const cJSON *json, struct t_constellation_tracker_camera *out_camera)
{
	const cJSON *calibration = u_json_get(json, "calibration");
	const cJSON *resolution = u_json_get(calibration, "resolution");
	const cJSON *intrinsics = u_json_get(calibration, "intrinsics");
	const cJSON *distortion = u_json_get(calibration, "distortion");
	const cJSON *pose = u_json_get(json, "pose_in_tracking_origin_xrt");
	char model[64] = {0};
	int width = 0;
	int height = 0;
	double fx = 0.0, fy = 0.0, cx = 0.0, cy = 0.0;
	double k[4] = {0};

	bool good = u_json_get_string_into_array(u_json_get(calibration, "model"), model, sizeof(model));
	good = good && strcmp(model, "fisheye_equidistant4") == 0;
	good = good && u_json_get_int(u_json_get(resolution, "width"), &width);
	good = good && u_json_get_int(u_json_get(resolution, "height"), &height);
	good = good && width == 512 && height == 508;
	good = good && get_number(intrinsics, "fx", &fx) && get_number(intrinsics, "fy", &fy);
	good = good && get_number(intrinsics, "cx", &cx) && get_number(intrinsics, "cy", &cy);
	good = good && get_number(distortion, "k1", &k[0]) && get_number(distortion, "k2", &k[1]);
	good = good && get_number(distortion, "k3", &k[2]) && get_number(distortion, "k4", &k[3]);
	good = good && fx > 0.0 && fy > 0.0 && u_json_get_pose(pose, &out_camera->pose_in_origin);
	if (!good) {
		return false;
	}

	out_camera->calibration.image_size_pixels = (struct xrt_size){(uint32_t)width, (uint32_t)height};
	out_camera->calibration.intrinsics[0][0] = fx;
	out_camera->calibration.intrinsics[0][2] = cx;
	out_camera->calibration.intrinsics[1][1] = fy;
	out_camera->calibration.intrinsics[1][2] = cy;
	out_camera->calibration.intrinsics[2][2] = 1.0;
	out_camera->calibration.kb4 = (struct t_camera_calibration_kb4_params){k[0], k[1], k[2], k[3]};
	out_camera->calibration.distortion_model = T_DISTORTION_FISHEYE_KB4;
	out_camera->has_concrete_pose = true;
	return true;
}

static bool
load_calibration(const char *path, struct t_constellation_tracker_params *out_params)
{
	char *contents = u_file_read_content_from_path(path, NULL);
	if (contents == NULL) {
		fprintf(stderr, "Could not read calibration '%s'.\n", path);
		return false;
	}
	cJSON *root = cJSON_Parse(contents);
	free(contents);
	if (root == NULL) {
		fprintf(stderr, "Could not parse calibration '%s'.\n", path);
		return false;
	}

	char format[64] = {0};
	const cJSON *cameras = u_json_get(root, "cameras");
	bool good = u_json_get_string_into_array(u_json_get(root, "format"), format, sizeof(format));
	good = good && strcmp(format, "psvr2-mode4-constellation-calibration-v1") == 0;
	good = good && cJSON_IsArray(cameras) && cJSON_GetArraySize(cameras) == 4;
	bool seen[4] = {false};
	for (int index = 0; good && index < 4; index++) {
		const cJSON *camera = cJSON_GetArrayItem(cameras, index);
		int camera_index = -1;
		good = u_json_get_int(u_json_get(camera, "camera"), &camera_index);
		good = good && camera_index >= 0 && camera_index < 4 && !seen[camera_index];
		if (good) {
			seen[camera_index] = true;
			good = load_camera(camera, &out_params->mosaics[0].cameras[camera_index]);
		}
	}
	cJSON_Delete(root);
	if (!good) {
		fprintf(stderr, "Calibration is not a valid four-camera provisional mode-4 artifact.\n");
		return false;
	}
	out_params->num_mosaics = 1;
	out_params->mosaics[0].num_cameras = 4;
	return true;
}

static void
print_relation(const char *hand, struct xrt_device *controller, int64_t now_ns, bool *out_saw_position)
{
	struct xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
	struct pssense_constellation_diagnostics diagnostics = {0};
	xrt_device_get_tracked_pose(controller, XRT_INPUT_PSSENSE_AIM_POSE, now_ns, &relation);
	(void)pssense_get_constellation_diagnostics(controller, &diagnostics);
	bool positioned = (relation.relation_flags & XRT_SPACE_RELATION_POSITION_TRACKED_BIT) != 0;
	*out_saw_position |= positioned;
	int64_t pose_age_ns = diagnostics.last_fused_timestamp_ns > 0 ? now_ns - diagnostics.last_fused_timestamp_ns : -1;
	printf("%" PRIi64 ",%s,0x%x,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%" PRIi64 ",%" PRIu64
	       ",%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
	       now_ns, hand, (unsigned)relation.relation_flags, relation.pose.position.x, relation.pose.position.y,
	       relation.pose.position.z, relation.pose.orientation.x, relation.pose.orientation.y,
	       relation.pose.orientation.z, relation.pose.orientation.w, pose_age_ns, diagnostics.fused_pose_count,
	       diagnostics.last_fused_camera_count, diagnostics.candidate_count, diagnostics.disagreement_count,
	       diagnostics.jump_rejection_count);
}

int
cli_cmd_psvr2_constellation(int argc, const char **argv)
{
	if (argc < 3 || argc > 5) {
		fprintf(stderr,
		        "Usage: %s %s CALIBRATION.json [duration-seconds: 1-120] [capture-directory]\n",
		        argv[0], argv[1]);
		return EXIT_FAILURE;
	}
	long duration_s = 20;
	if (argc >= 4) {
		errno = 0;
		char *end = NULL;
		duration_s = strtol(argv[3], &end, 10);
		if (errno != 0 || end == argv[3] || *end != '\0' || duration_s < 1 || duration_s > 120) {
			fprintf(stderr, "Duration must be between 1 and 120 seconds.\n");
			return EXIT_FAILURE;
		}
	}

	struct t_constellation_tracker_params params = {0};
	if (!load_calibration(argv[2], &params)) {
		return EXIT_FAILURE;
	}
	fprintf(stderr, "WARNING: using a provisional calibration for an opt-in diagnostic only.\n");

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

	struct xrt_device *head = xsysd->static_roles.head;
	struct xrt_device *controllers[2] = {NULL, NULL};
	for (size_t i = 0; i < xsysd->static_xdev_count; i++) {
		struct xrt_device *xdev = xsysd->static_xdevs[i];
		if (xdev->name != XRT_DEVICE_PSSENSE) {
			continue;
		}
		if (xdev->device_type == XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER) controllers[0] = xdev;
		if (xdev->device_type == XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER) controllers[1] = xdev;
	}
	struct psvr2_camera_diagnostics diag = {0};
	if (head == NULL || !psvr2_get_camera_diagnostics(head, &diag) || !diag.enabled ||
	    diag.configured_mode != 4 || (controllers[0] == NULL && controllers[1] == NULL)) {
		fprintf(stderr, "Need a PS VR2 in camera mode 4 and at least one connected Sense controller.\n");
		destroy_system(&xi, &xsys, &xsysd, &xso);
		return EXIT_FAILURE;
	}

	struct xrt_frame_context tracking_xfctx = {0};
	struct t_constellation_tracker *tracker = NULL;
	struct t_blobwatch *blobwatches[4] = {0};
	struct xrt_frame_sink *frame_sinks[4] = {0};
	uint8_t blob_pixel_threshold = (uint8_t)clamp_long(
	    debug_get_num_option_psvr2_constellation_blob_pixel_threshold(), 0, UINT8_MAX);
	uint8_t blob_required_threshold = (uint8_t)clamp_long(
	    debug_get_num_option_psvr2_constellation_blob_required_threshold(), 0, UINT8_MAX);
	uint16_t blob_max_width = (uint16_t)clamp_long(
	    debug_get_num_option_psvr2_constellation_blob_max_width(), 1, UINT16_MAX);
	uint32_t capture_stride = (uint32_t)clamp_long(
	    debug_get_num_option_psvr2_constellation_capture_stride(), 1, UINT32_MAX);
	const char *capture_dir = argc == 5 ? argv[4] : NULL;
	struct constellation_capture_sink capture_sinks[CONSTELLATION_CAMERA_COUNT] = {0};
	bool capture_initialized = false;
	fprintf(stderr, "Blob detector: pixel threshold=%u, required threshold=%u, max width=%u.\n",
	        blob_pixel_threshold, blob_required_threshold, blob_max_width);
	if (capture_dir != NULL &&
	    !constellation_capture_init(capture_sinks, capture_dir, capture_stride, argv[2], head,
	                                blob_pixel_threshold, blob_required_threshold, blob_max_width)) {
		goto fail;
	}
	capture_initialized = capture_dir != NULL;
	if (capture_initialized) {
		fprintf(stderr, "Retaining mode-4 images in '%s' (sequence stride %u).\n", capture_dir, capture_stride);
	}
	if (t_constellation_tracker_create(&tracking_xfctx, &params, &tracker) != 0) {
		fprintf(stderr, "Failed to create constellation tracker.\n");
		goto fail;
	}
	for (size_t i = 0; i < 4; i++) {
		struct t_rift_blobwatch_params blob_params = {
		    .pixel_threshold = blob_pixel_threshold,
		    .blob_required_threshold = blob_required_threshold,
		    .max_match_dist = 50.0f,
		    .max_blob_width = blob_max_width,
		};
		if (t_rift_blobwatch_create(&blob_params, &tracking_xfctx, params.mosaics[0].cameras[i].blob_sink,
		                            &frame_sinks[i], &blobwatches[i]) != 0 ||
		    !u_sink_simple_queue_create(&tracking_xfctx, frame_sinks[i], &frame_sinks[i])) {
			fprintf(stderr, "Failed to create blob pipeline for camera %zu.\n", i);
			goto fail;
		}
		if (capture_initialized) {
			struct xrt_frame_sink *capture_sink = &capture_sinks[i].base;
			if (!u_sink_simple_queue_create(&tracking_xfctx, capture_sink, &capture_sink)) {
				fprintf(stderr, "Failed to create capture queue for camera %zu.\n", i);
				goto fail;
			}
			u_sink_split_create(&tracking_xfctx, frame_sinks[i], capture_sink, &frame_sinks[i]);
		}
	}
	if (!psvr2_set_camera_frame_sinks(head, frame_sinks)) {
		fprintf(stderr, "Failed to attach tracking camera sinks.\n");
		goto fail;
	}
	for (size_t i = 0; i < 2; i++) {
		if (controllers[i] != NULL && pssense_add_to_constellation_tracker(controllers[i], tracker) != 0) {
			fprintf(stderr, "Failed to attach %s controller.\n", i == 0 ? "left" : "right");
			goto fail;
		}
	}

	printf("timestamp_ns,hand,relation_flags,px,py,pz,qx,qy,qz,qw,pose_age_ns,fused_pose_count,"
	       "fused_camera_count,candidate_count,disagreement_count,jump_rejection_count\n");
	bool saw_position[2] = {false};
	int64_t end_ns = os_monotonic_get_ns() + duration_s * U_TIME_1S_IN_NS;
	int64_t next_print_ns = 0;
	while (os_monotonic_get_ns() < end_ns) {
		int64_t now_ns = os_monotonic_get_ns();
		if (now_ns >= next_print_ns) {
			if (controllers[0] != NULL) print_relation("left", controllers[0], now_ns, &saw_position[0]);
			if (controllers[1] != NULL) print_relation("right", controllers[1], now_ns, &saw_position[1]);
			fflush(stdout);
			next_print_ns = now_ns + U_TIME_1S_IN_NS / 10;
		}
		os_nanosleep(U_TIME_1MS_IN_NS);
	}
	(void)psvr2_set_camera_frame_sinks(head, NULL);
	struct pssense_constellation_diagnostics final_diagnostics[2] = {0};
	for (size_t i = 0; i < 2; i++) {
		if (controllers[i] != NULL) {
			(void)pssense_get_constellation_diagnostics(controllers[i], &final_diagnostics[i]);
		}
	}
	for (size_t i = 0; i < 2; i++) if (controllers[i] != NULL) pssense_remove_from_constellation_tracker(controllers[i]);
	xrt_frame_context_destroy_nodes(&tracking_xfctx);
	if (capture_initialized) {
		for (uint32_t i = 0; i < CONSTELLATION_CAMERA_COUNT; i++) {
			fprintf(stderr,
			        "Capture camera %u: processed=%" PRIu64 ", written=%" PRIu64 ", failed=%" PRIu64 ".\n",
			        i, capture_sinks[i].received_frames, capture_sinks[i].written_frames,
			        capture_sinks[i].failed_frames);
		}
		constellation_capture_finish(capture_sinks);
	}
	(void)psvr2_get_camera_diagnostics(head, &diag);
	destroy_system(&xi, &xsys, &xsysd, &xso);
	bool pass = diag.frame_count > 0;
	for (size_t i = 0; i < 2; i++) {
		if (controllers[i] != NULL) {
			pass &= saw_position[i] && final_diagnostics[i].fused_pose_count >= 2;
		}
	}
	fprintf(stderr, "%s: mode-4 frames=%" PRIu64 ", left-position=%s (%" PRIu64
	                " candidates [%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "], %" PRIu64
	                " fused, %" PRIu64 " disagree, %" PRIu64 " jumps), right-position=%s (%" PRIu64
	                " candidates [%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "], %" PRIu64
	                " fused, %" PRIu64 " disagree, %" PRIu64 " jumps).\n",
	        pass ? "PASS" : "INCOMPLETE", diag.frame_count, saw_position[0] ? "yes" : "no",
	        final_diagnostics[0].candidate_count, final_diagnostics[0].camera_candidate_count[0],
	        final_diagnostics[0].camera_candidate_count[1], final_diagnostics[0].camera_candidate_count[2],
	        final_diagnostics[0].camera_candidate_count[3],
	        final_diagnostics[0].fused_pose_count, final_diagnostics[0].disagreement_count,
	        final_diagnostics[0].jump_rejection_count, saw_position[1] ? "yes" : "no",
	        final_diagnostics[1].candidate_count, final_diagnostics[1].camera_candidate_count[0],
	        final_diagnostics[1].camera_candidate_count[1], final_diagnostics[1].camera_candidate_count[2],
	        final_diagnostics[1].camera_candidate_count[3],
	        final_diagnostics[1].fused_pose_count, final_diagnostics[1].disagreement_count,
	        final_diagnostics[1].jump_rejection_count);
	return pass ? EXIT_SUCCESS : 2;

fail:
	(void)psvr2_set_camera_frame_sinks(head, NULL);
	for (size_t i = 0; i < 2; i++) {
		if (controllers[i] != NULL) pssense_remove_from_constellation_tracker(controllers[i]);
	}
	xrt_frame_context_destroy_nodes(&tracking_xfctx);
	if (capture_initialized) {
		constellation_capture_finish(capture_sinks);
	}
	destroy_system(&xi, &xsys, &xsysd, &xso);
	return EXIT_FAILURE;
}

#else

#include <stdio.h>
#include <stdlib.h>

int
cli_cmd_psvr2_constellation(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	fprintf(stderr, "psvr2-constellation requires the PS Sense driver.\n");
	return EXIT_FAILURE;
}

#endif

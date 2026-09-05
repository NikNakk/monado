from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one match, found {count}: {old[:120]!r}")
    path.write_text(text.replace(old, new, 1))


psvr2 = Path("src/xrt/drivers/psvr2/psvr2.c")
macos = Path("src/xrt/compositor/main/comp_window_macos.m")

replace_once(
    psvr2,
    '''#include <stdio.h>\n#include <assert.h>\n#include <inttypes.h>\n#include <libusb.h>\n''',
    '''#include <assert.h>\n#include <inttypes.h>\n#include <libusb.h>\n#include <stdatomic.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n\n#ifdef XRT_OS_OSX\n#include <unistd.h>\n#endif\n''',
)

trace_helpers = r'''DEBUG_GET_ONCE_BOOL_OPTION(psvr2_timing_trace, "PSVR2_TIMING_TRACE", false)

#ifdef XRT_OS_OSX
struct psvr2_timing_trace_state
{
	bool attempted;
	FILE *imu;
	FILE *slam;
	FILE *pose;
	uint64_t imu_rows;
	uint64_t slam_rows;
	uint64_t pose_rows;
	atomic_uint_fast32_t last_dp_scanout;
};

static struct psvr2_timing_trace_state g_psvr2_timing_trace;

static FILE *
psvr2_timing_trace_open_file(const char *suffix, const char *header)
{
	const char *dir = getenv("PSVR2_TIMING_TRACE_DIR");
	if (dir == NULL || dir[0] == '\0') {
		dir = "/tmp";
	}

	char path[1024];
	size_t dir_len = strlen(dir);
	const char *separator = dir_len > 0 && dir[dir_len - 1] == '/' ? "" : "/";
	snprintf(path, sizeof(path), "%s%smonado_psvr2_%d_%s.csv", dir, separator, (int)getpid(), suffix);

	FILE *file = fopen(path, "w");
	if (file == NULL) {
		return NULL;
	}
	setvbuf(file, NULL, _IOFBF, 64 * 1024);
	fputs(header, file);
	fputc('\n', file);
	fflush(file);
	return file;
}

static void
psvr2_timing_trace_open(void)
{
	if (g_psvr2_timing_trace.attempted || !debug_get_bool_option_psvr2_timing_trace()) {
		return;
	}
	g_psvr2_timing_trace.attempted = true;

	g_psvr2_timing_trace.imu = psvr2_timing_trace_open_file(
	    "imu",
	    "host_estimated_sample_ns,vts_ns,imu_ns,vts_mapped_host_ns,imu_mapped_host_ns,vts_us,imu_ts_us,"
	    "dp_frame_cnt,dp_line_cnt,status,hw2mono_vts_ns,hw2mono_imu_ns,gyro_x,gyro_y,gyro_z,accel_x,accel_y,"
	    "accel_z");
	g_psvr2_timing_trace.slam = psvr2_timing_trace_open_file(
	    "slam",
	    "host_received_ns,slam_vts_ns,slam_mapped_host_ns,latest_imu_vts_ns,latest_imu_mapped_host_ns,"
	    "dp_frame_cnt,dp_line_cnt,raw_pos_x,raw_pos_y,raw_pos_z,raw_qw,raw_qx,raw_qy,raw_qz,corrected_pos_x,"
	    "corrected_pos_y,corrected_pos_z,corrected_qw,corrected_qx,corrected_qy,corrected_qz,linvel_x,linvel_y,"
	    "linvel_z,angvel_x,angvel_y,angvel_z,relation_flags");
	g_psvr2_timing_trace.pose = psvr2_timing_trace_open_file(
	    "pose",
	    "host_query_ns,requested_host_ns,requested_vts_ns,latest_slam_vts_ns,latest_imu_vts_ns,hw2mono_vts_ns,"
	    "dp_frame_cnt,dp_line_cnt,pos_x,pos_y,pos_z,qw,qx,qy,qz,linvel_x,linvel_y,linvel_z,angvel_x,angvel_y,"
	    "angvel_z,relation_flags");
}

static void
psvr2_timing_trace_close(void)
{
	FILE *files[] = {g_psvr2_timing_trace.imu, g_psvr2_timing_trace.slam, g_psvr2_timing_trace.pose};
	for (size_t i = 0; i < 3; i++) {
		if (files[i] != NULL) {
			fflush(files[i]);
			fclose(files[i]);
		}
	}
	g_psvr2_timing_trace.imu = NULL;
	g_psvr2_timing_trace.slam = NULL;
	g_psvr2_timing_trace.pose = NULL;
}

static void
psvr2_timing_trace_maybe_flush(FILE *file, uint64_t rows, uint64_t interval)
{
	if (file != NULL && rows % interval == 0) {
		fflush(file);
	}
}

static void
psvr2_timing_trace_store_scanout(uint16_t frame, uint16_t line)
{
	uint32_t packed = ((uint32_t)frame << 16) | (uint32_t)line;
	atomic_store_explicit(&g_psvr2_timing_trace.last_dp_scanout, packed, memory_order_release);
}

static void
psvr2_timing_trace_load_scanout(uint16_t *frame, uint16_t *line)
{
	uint32_t packed = (uint32_t)atomic_load_explicit(&g_psvr2_timing_trace.last_dp_scanout, memory_order_acquire);
	*frame = (uint16_t)(packed >> 16);
	*line = (uint16_t)(packed & 0xffffu);
}

static void
psvr2_timing_trace_imu(struct psvr2_hmd *hmd,
                       const struct imu_record *imu,
                       timepoint_ns estimated_sample_time,
                       timepoint_ns vts_ns,
                       timepoint_ns imu_ns)
{
	psvr2_timing_trace_store_scanout(imu->dp_frame_cnt, imu->dp_line_cnt);
	FILE *file = g_psvr2_timing_trace.imu;
	if (file == NULL) {
		return;
	}

	fprintf(file,
	        "%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%u,%u,%u,%u,%u,%" PRIi64
	        ",%" PRIi64 ",%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
	        (int64_t)estimated_sample_time, (int64_t)vts_ns, (int64_t)imu_ns, (int64_t)(vts_ns + hmd->hw2mono_vts),
	        (int64_t)(imu_ns + hmd->hw2mono_imu), imu->vts_us, imu->imu_ts_us, imu->dp_frame_cnt, imu->dp_line_cnt,
	        imu->status, (int64_t)hmd->hw2mono_vts, (int64_t)hmd->hw2mono_imu, hmd->last_gyro.x, hmd->last_gyro.y,
	        hmd->last_gyro.z, hmd->last_accel.x, hmd->last_accel.y, hmd->last_accel.z);
	g_psvr2_timing_trace.imu_rows++;
	psvr2_timing_trace_maybe_flush(file, g_psvr2_timing_trace.imu_rows, 2048);
}

static void
psvr2_timing_trace_slam(struct psvr2_hmd *hmd,
                        timepoint_ns received_ns,
                        timepoint_ns vts_ns,
                        const struct xrt_space_relation *relation)
{
	FILE *file = g_psvr2_timing_trace.slam;
	if (file == NULL) {
		return;
	}
	uint16_t dp_frame_cnt = 0;
	uint16_t dp_line_cnt = 0;
	psvr2_timing_trace_load_scanout(&dp_frame_cnt, &dp_line_cnt);

	fprintf(file,
	        "%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%u,%u,"
	        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
	        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u\n",
	        (int64_t)received_ns, (int64_t)vts_ns, (int64_t)(vts_ns + hmd->hw2mono_vts),
	        (int64_t)hmd->last_imu_vts_ns, (int64_t)(hmd->last_imu_vts_ns + hmd->hw2mono_vts), dp_frame_cnt,
	        dp_line_cnt, hmd->last_slam_pose.position.x, hmd->last_slam_pose.position.y, hmd->last_slam_pose.position.z,
	        hmd->last_slam_pose.orientation.w, hmd->last_slam_pose.orientation.x, hmd->last_slam_pose.orientation.y,
	        hmd->last_slam_pose.orientation.z, hmd->pose.position.x, hmd->pose.position.y, hmd->pose.position.z,
	        hmd->pose.orientation.w, hmd->pose.orientation.x, hmd->pose.orientation.y, hmd->pose.orientation.z,
	        relation->linear_velocity.x, relation->linear_velocity.y, relation->linear_velocity.z,
	        relation->angular_velocity.x, relation->angular_velocity.y, relation->angular_velocity.z,
	        (unsigned int)relation->relation_flags);
	g_psvr2_timing_trace.slam_rows++;
	psvr2_timing_trace_maybe_flush(file, g_psvr2_timing_trace.slam_rows, 256);
}

static void
psvr2_timing_trace_pose(timepoint_ns host_query_ns,
                        timepoint_ns requested_host_ns,
                        timepoint_ns requested_vts_ns,
                        timepoint_ns latest_slam_vts_ns,
                        timepoint_ns latest_imu_vts_ns,
                        time_duration_ns hw2mono_vts,
                        const struct xrt_space_relation *relation)
{
	FILE *file = g_psvr2_timing_trace.pose;
	if (file == NULL) {
		return;
	}
	uint16_t dp_frame_cnt = 0;
	uint16_t dp_line_cnt = 0;
	psvr2_timing_trace_load_scanout(&dp_frame_cnt, &dp_line_cnt);

	fprintf(file,
	        "%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%u,%u,"
	        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u\n",
	        (int64_t)host_query_ns, (int64_t)requested_host_ns, (int64_t)requested_vts_ns, (int64_t)latest_slam_vts_ns,
	        (int64_t)latest_imu_vts_ns, (int64_t)hw2mono_vts, dp_frame_cnt, dp_line_cnt, relation->pose.position.x,
	        relation->pose.position.y, relation->pose.position.z, relation->pose.orientation.w, relation->pose.orientation.x,
	        relation->pose.orientation.y, relation->pose.orientation.z, relation->linear_velocity.x, relation->linear_velocity.y,
	        relation->linear_velocity.z, relation->angular_velocity.x, relation->angular_velocity.y,
	        relation->angular_velocity.z, (unsigned int)relation->relation_flags);
	g_psvr2_timing_trace.pose_rows++;
	psvr2_timing_trace_maybe_flush(file, g_psvr2_timing_trace.pose_rows, 256);
}
#endif
'''

replace_once(
    psvr2,
    'DEBUG_GET_ONCE_BOOL_OPTION(psvr2_timing_log, "PSVR2_TIMING_LOG", false)\n',
    'DEBUG_GET_ONCE_BOOL_OPTION(psvr2_timing_log, "PSVR2_TIMING_LOG", false)\n' + trace_helpers,
)

replace_once(
    psvr2,
    '''\tpsvr2_usb_destroy(hmd);\n\n\t// @note We appear to be hitting a bug in libusb, so this is commented out\n''',
    '''\tpsvr2_usb_destroy(hmd);\n\n#ifdef XRT_OS_OSX\n\tpsvr2_timing_trace_close();\n#endif\n\n\t// @note We appear to be hitting a bug in libusb, so this is commented out\n''',
)

replace_once(
    psvr2,
    '''\tstruct psvr2_hmd *hmd = psvr2_hmd(xdev);\n\n\tswitch (name) {\n''',
    '''\tstruct psvr2_hmd *hmd = psvr2_hmd(xdev);\n\n#ifdef XRT_OS_OSX\n\ttimepoint_ns trace_host_query_ns = os_monotonic_get_ns();\n#endif\n\n\tswitch (name) {\n''',
)

replace_once(
    psvr2,
    '''\ttimepoint_ns prediction_ns_hw = at_timestamp_ns - hmd->hw2mono_vts;\n\n\tstruct xrt_relation_chain chain = {0};\n''',
    '''\ttimepoint_ns prediction_ns_hw = at_timestamp_ns - hmd->hw2mono_vts;\n\n#ifdef XRT_OS_OSX\n\ttimepoint_ns trace_latest_slam_vts_ns = hmd->last_slam_vts_ns;\n\ttimepoint_ns trace_latest_imu_vts_ns = hmd->last_imu_vts_ns;\n\ttime_duration_ns trace_hw2mono_vts = hmd->hw2mono_vts;\n#endif\n\n\tstruct xrt_relation_chain chain = {0};\n''',
)

replace_once(
    psvr2,
    '''\t// Resolve the final relation\n\tm_relation_chain_resolve(&chain, out_relation);\n\n\treturn XRT_SUCCESS;\n}\n''',
    '''\t// Resolve the final relation\n\tm_relation_chain_resolve(&chain, out_relation);\n\n#ifdef XRT_OS_OSX\n\tif (name == XRT_INPUT_GENERIC_HEAD_POSE) {\n\t\tpsvr2_timing_trace_pose(trace_host_query_ns, at_timestamp_ns, prediction_ns_hw, trace_latest_slam_vts_ns,\n\t\t                         trace_latest_imu_vts_ns, trace_hw2mono_vts, out_relation);\n\t}\n#endif\n\n\treturn XRT_SUCCESS;\n}\n''',
)

replace_once(
    psvr2,
    '''\tm_clock_offset_a2b(IMU_FREQ, now_vts, estimated_sample_time, &hmd->hw2mono_vts);\n\tm_clock_offset_a2b(IMU_FREQ, now_imu, estimated_sample_time, &hmd->hw2mono_imu);\n\n\tif (hmd->timestamp_samples < TIMESTAMP_SAMPLES) {\n''',
    '''\tm_clock_offset_a2b(IMU_FREQ, now_vts, estimated_sample_time, &hmd->hw2mono_vts);\n\tm_clock_offset_a2b(IMU_FREQ, now_imu, estimated_sample_time, &hmd->hw2mono_imu);\n\n#ifdef XRT_OS_OSX\n\tpsvr2_timing_trace_imu(hmd, &imu_data, estimated_sample_time, now_vts, now_imu);\n#endif\n\n\tif (hmd->timestamp_samples < TIMESTAMP_SAMPLES) {\n''',
)

replace_once(
    psvr2,
    'process_slam_record(struct psvr2_hmd *hmd, uint8_t *buf, int bytes_read)\n',
    'process_slam_record(struct psvr2_hmd *hmd, uint8_t *buf, int bytes_read, timepoint_ns received_ns)\n',
)

replace_once(
    psvr2,
    '''\tm_relation_history_push_with_motion_estimation(hmd->slam_relation_history, &relation, pose_sample.timestamp_ns);\n\tos_mutex_unlock(&hmd->data_lock);\n}\n''',
    '''\tm_relation_history_push_with_motion_estimation(hmd->slam_relation_history, &relation, pose_sample.timestamp_ns);\n\n#ifdef XRT_OS_OSX\n\tstruct xrt_space_relation traced_relation = relation;\n\ttimepoint_ns traced_relation_ts = 0;\n\tif (!m_relation_history_get_latest(hmd->slam_relation_history, &traced_relation_ts, &traced_relation)) {\n\t\ttraced_relation = relation;\n\t}\n\tpsvr2_timing_trace_slam(hmd, received_ns, vts_ns, &traced_relation);\n#endif\n\n\tos_mutex_unlock(&hmd->data_lock);\n}\n''',
)

replace_once(
    psvr2,
    '''\tstruct psvr2_hmd *hmd = xfer->user_data;\n\tif (xfer->actual_length == sizeof(struct slam_usb_record)) {\n\t\tprocess_slam_record(hmd, xfer->buffer, xfer->actual_length);\n\t}\n''',
    '''\tstruct psvr2_hmd *hmd = xfer->user_data;\n\ttimepoint_ns received_ns = os_monotonic_get_ns();\n\tif (xfer->actual_length == sizeof(struct slam_usb_record)) {\n\t\tprocess_slam_record(hmd, xfer->buffer, xfer->actual_length, received_ns);\n\t}\n''',
)

replace_once(
    psvr2,
    '''\thmd->log_level = debug_get_log_option_psvr2_log();\n\thmd->auxiliary_streams_enabled = debug_get_bool_option_psvr2_auxiliary_streams();\n\n\tsnprintf(hmd->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "PS VR2 Tracking");\n''',
    '''\thmd->log_level = debug_get_log_option_psvr2_log();\n\thmd->auxiliary_streams_enabled = debug_get_bool_option_psvr2_auxiliary_streams();\n\n#ifdef XRT_OS_OSX\n\tpsvr2_timing_trace_open();\n#endif\n\n\tsnprintf(hmd->base.tracking_origin->name, XRT_TRACKING_NAME_LEN, "PS VR2 Tracking");\n''',
)

replace_once(
    macos,
    '#include <stdatomic.h>\n',
    '#include <inttypes.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <stdatomic.h>\n#include <string.h>\n#include <unistd.h>\n',
)

replace_once(
    macos,
    'DEBUG_GET_ONCE_NUM_OPTION(display_rate_divisor, "XRT_MACOS_DISPLAY_RATE_DIVISOR", 1)\n',
    'DEBUG_GET_ONCE_NUM_OPTION(display_rate_divisor, "XRT_MACOS_DISPLAY_RATE_DIVISOR", 1)\nDEBUG_GET_ONCE_BOOL_OPTION(macos_psvr2_timing_trace, "PSVR2_TIMING_TRACE", false)\n',
)

replace_once(
    macos,
    '''\tatomic_uint_fast64_t latest_vblank_ns;\n\tuint64_t last_vblank_ns;\n''',
    '''\tatomic_uint_fast64_t latest_vblank_ns;\n\tatomic_uint_fast64_t latest_displaylink_now_ns;\n\tatomic_uint_fast64_t latest_displaylink_output_ns;\n\tatomic_uint_fast64_t latest_displaylink_callback_ns;\n\tuint64_t last_vblank_ns;\n\tuint64_t trace_frame_id;\n''',
)

replace_once(
    macos,
    '''\tuint32_t next_image;\n\tbool logged_layer_state;\n};\n\nstatic uint64_t\nhost_time_to_ns''',
    '''\tuint32_t next_image;\n\tbool logged_layer_state;\n\tFILE *trace_present;\n\tFILE *trace_vblank;\n\tuint64_t trace_present_rows;\n\tuint64_t trace_vblank_rows;\n};\n\nstatic FILE *\nmacos_timing_trace_open_file(const char *suffix, const char *header)\n{\n\tconst char *dir = getenv("PSVR2_TIMING_TRACE_DIR");\n\tif (dir == NULL || dir[0] == '\\0') {\n\t\tdir = "/tmp";\n\t}\n\tchar path[1024];\n\tsize_t dir_len = strlen(dir);\n\tconst char *separator = dir_len > 0 && dir[dir_len - 1] == '/' ? "" : "/";\n\tsnprintf(path, sizeof(path), "%s%smonado_psvr2_%d_%s.csv", dir, separator, (int)getpid(), suffix);\n\tFILE *file = fopen(path, "w");\n\tif (file == NULL) {\n\t\treturn NULL;\n\t}\n\tsetvbuf(file, NULL, _IOFBF, 64 * 1024);\n\tfputs(header, file);\n\tfputc('\\n', file);\n\tfflush(file);\n\treturn file;\n}\n\nstatic void\nmacos_timing_trace_open(struct comp_window_macos *cwm)\n{\n\tif (!debug_get_bool_option_macos_psvr2_timing_trace()) {\n\t\treturn;\n\t}\n\tcwm->trace_present = macos_timing_trace_open_file(\n\t    "present",\n\t    "frame_id,host_call_ns,desired_present_ns,present_slop_ns,image_index,timeline_value,after_vk_wait_ns,"\n\t    "after_drawable_ns,before_present_call_ns,after_present_call_ns,after_commit_ns,after_metal_wait_ns,"\n\t    "latest_displaylink_output_ns,drawable_presented_time_s,gpu_start_time_s,gpu_end_time_s");\n\tcwm->trace_vblank = macos_timing_trace_open_file(\n\t    "vblank",\n\t    "host_consumed_ns,displaylink_callback_ns,displaylink_now_ns,displaylink_output_ns,output_minus_now_ns,"\n\t    "callback_minus_output_ns,interval_from_previous_output_ns,display_period_ns");\n}\n\nstatic void\nmacos_timing_trace_close(struct comp_window_macos *cwm)\n{\n\tif (cwm->trace_present != NULL) {\n\t\tfflush(cwm->trace_present);\n\t\tfclose(cwm->trace_present);\n\t\tcwm->trace_present = NULL;\n\t}\n\tif (cwm->trace_vblank != NULL) {\n\t\tfflush(cwm->trace_vblank);\n\t\tfclose(cwm->trace_vblank);\n\t\tcwm->trace_vblank = NULL;\n\t}\n}\n\nstatic uint64_t\nhost_time_to_ns''',
)

replace_once(
    macos,
    '''\t(void)display_link;\n\t(void)in_now;\n\t(void)flags_in;\n\t(void)flags_out;\n\tstruct comp_window_macos *cwm = context;\n\tif ((in_output_time->flags & kCVTimeStampHostTimeValid) != 0) {\n\t\tuint64_t output_ns = host_time_to_ns(cwm, in_output_time->hostTime);\n\t\tatomic_store_explicit(&cwm->latest_vblank_ns, output_ns, memory_order_release);\n\t}\n\treturn kCVReturnSuccess;\n''',
    '''\t(void)display_link;\n\t(void)flags_in;\n\t(void)flags_out;\n\tstruct comp_window_macos *cwm = context;\n\tif ((in_output_time->flags & kCVTimeStampHostTimeValid) != 0) {\n\t\tuint64_t output_ns = host_time_to_ns(cwm, in_output_time->hostTime);\n\t\tatomic_store_explicit(&cwm->latest_vblank_ns, output_ns, memory_order_release);\n\t\tatomic_store_explicit(&cwm->latest_displaylink_output_ns, output_ns, memory_order_release);\n\t}\n\tif ((in_now->flags & kCVTimeStampHostTimeValid) != 0) {\n\t\tuint64_t now_ns = host_time_to_ns(cwm, in_now->hostTime);\n\t\tatomic_store_explicit(&cwm->latest_displaylink_now_ns, now_ns, memory_order_release);\n\t}\n\tatomic_store_explicit(&cwm->latest_displaylink_callback_ns, os_monotonic_get_ns(), memory_order_release);\n\treturn kCVReturnSuccess;\n''',
)

replace_once(
    macos,
    '''\tstruct comp_window_macos *cwm = (struct comp_window_macos *)ct;\n\tstruct vk_bundle *vk = get_vk(cwm);\n\t(void)timeline_semaphore_value;\n\t(void)present_slop_ns;\n\tassert(present_queue != NULL);\n''',
    '''\tstruct comp_window_macos *cwm = (struct comp_window_macos *)ct;\n\tstruct vk_bundle *vk = get_vk(cwm);\n\tuint64_t frame_id = ++cwm->trace_frame_id;\n\tuint64_t after_drawable_ns = 0;\n\tuint64_t before_present_call_ns = 0;\n\tuint64_t after_present_call_ns = 0;\n\tuint64_t after_commit_ns = 0;\n\tuint64_t after_metal_wait_ns = 0;\n\tdouble drawable_presented_time_s = 0.0;\n\tdouble gpu_start_time_s = 0.0;\n\tdouble gpu_end_time_s = 0.0;\n\tassert(present_queue != NULL);\n''',
)

replace_once(
    macos,
    '''\t\tid<CAMetalDrawable> drawable = [cwm->metal_layer nextDrawable];\n\t\tuint64_t after_drawable_ns = os_monotonic_get_ns();\n''',
    '''\t\tid<CAMetalDrawable> drawable = [cwm->metal_layer nextDrawable];\n\t\tafter_drawable_ns = os_monotonic_get_ns();\n''',
)

replace_once(
    macos,
    '''\t\t[blit endEncoding];\n\t\t(void)desired_present_time_ns;\n\t\t[command_buffer presentDrawable:drawable];\n\t\t[command_buffer commit];\n\t\t[command_buffer waitUntilCompleted];\n\t\tuint64_t after_metal_wait_ns = os_monotonic_get_ns();\n''',
    '''\t\t[blit endEncoding];\n\t\tbefore_present_call_ns = os_monotonic_get_ns();\n\t\t[command_buffer presentDrawable:drawable];\n\t\tafter_present_call_ns = os_monotonic_get_ns();\n\t\t[command_buffer commit];\n\t\tafter_commit_ns = os_monotonic_get_ns();\n\t\t[command_buffer waitUntilCompleted];\n\t\tafter_metal_wait_ns = os_monotonic_get_ns();\n\t\tdrawable_presented_time_s = [drawable presentedTime];\n\t\tgpu_start_time_s = [command_buffer GPUStartTime];\n\t\tgpu_end_time_s = [command_buffer GPUEndTime];\n''',
)

replace_once(
    macos,
    '''\tuint64_t now_ns = os_monotonic_get_ns();\n\tif (cwm->last_present_ns != 0 && now_ns > cwm->last_present_ns) {\n''',
    '''\tif (cwm->trace_present != NULL) {\n\t\tuint64_t latest_output_ns = atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);\n\t\tfprintf(cwm->trace_present,\n\t\t        "%llu,%llu,%" PRIi64 ",%" PRIi64 ",%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.17g,%.17g,%.17g\\n",\n\t\t        (unsigned long long)frame_id, (unsigned long long)before_vk_wait_ns, desired_present_time_ns,\n\t\t        present_slop_ns, index, (unsigned long long)timeline_semaphore_value, (unsigned long long)after_vk_wait_ns,\n\t\t        (unsigned long long)after_drawable_ns, (unsigned long long)before_present_call_ns,\n\t\t        (unsigned long long)after_present_call_ns, (unsigned long long)after_commit_ns,\n\t\t        (unsigned long long)after_metal_wait_ns, (unsigned long long)latest_output_ns,\n\t\t        drawable_presented_time_s, gpu_start_time_s, gpu_end_time_s);\n\t\tcwm->trace_present_rows++;\n\t\tif (cwm->trace_present_rows % 256 == 0) {\n\t\t\tfflush(cwm->trace_present);\n\t\t}\n\t}\n\n\tuint64_t now_ns = os_monotonic_get_ns();\n\tif (cwm->last_present_ns != 0 && now_ns > cwm->last_present_ns) {\n''',
)

replace_once(
    macos,
    '''\tstruct comp_window_macos *cwm = (struct comp_window_macos *)ct;\n\tuint64_t vblank_ns = atomic_exchange_explicit(&cwm->latest_vblank_ns, 0, memory_order_acquire);\n\tif (vblank_ns == 0 || vblank_ns == cwm->last_vblank_ns || cwm->base.upc == NULL) {\n\t\treturn VK_SUCCESS;\n\t}\n\n\tu_pc_update_vblank_from_display_control(cwm->base.upc, (int64_t)vblank_ns);\n''',
    '''\tstruct comp_window_macos *cwm = (struct comp_window_macos *)ct;\n\tuint64_t vblank_ns = atomic_exchange_explicit(&cwm->latest_vblank_ns, 0, memory_order_acquire);\n\tuint64_t displaylink_now_ns = atomic_load_explicit(&cwm->latest_displaylink_now_ns, memory_order_acquire);\n\tuint64_t displaylink_output_ns = atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);\n\tuint64_t displaylink_callback_ns = atomic_load_explicit(&cwm->latest_displaylink_callback_ns, memory_order_acquire);\n\tif (vblank_ns == 0 || vblank_ns == cwm->last_vblank_ns || cwm->base.upc == NULL) {\n\t\treturn VK_SUCCESS;\n\t}\n\n\tuint64_t consumed_ns = os_monotonic_get_ns();\n\tuint64_t previous_vblank_ns = cwm->last_vblank_ns;\n\tif (cwm->trace_vblank != NULL) {\n\t\tfprintf(cwm->trace_vblank, "%llu,%llu,%llu,%llu,%" PRIi64 ",%" PRIi64 ",%llu,%" PRIi64 "\\n",\n\t\t        (unsigned long long)consumed_ns, (unsigned long long)displaylink_callback_ns,\n\t\t        (unsigned long long)displaylink_now_ns, (unsigned long long)displaylink_output_ns,\n\t\t        (int64_t)displaylink_output_ns - (int64_t)displaylink_now_ns,\n\t\t        (int64_t)displaylink_callback_ns - (int64_t)displaylink_output_ns,\n\t\t        previous_vblank_ns != 0 && vblank_ns > previous_vblank_ns\n\t\t            ? (unsigned long long)(vblank_ns - previous_vblank_ns)\n\t\t            : 0ULL,\n\t\t        cwm->display_period_ns);\n\t\tcwm->trace_vblank_rows++;\n\t\tif (cwm->trace_vblank_rows % 256 == 0) {\n\t\t\tfflush(cwm->trace_vblank);\n\t\t}\n\t}\n\n\tu_pc_update_vblank_from_display_control(cwm->base.upc, (int64_t)vblank_ns);\n''',
)

replace_once(
    macos,
    '''\tif (cwm->display_link != NULL) {\n\t\tCVDisplayLinkStop(cwm->display_link);\n\t\tCVDisplayLinkRelease(cwm->display_link);\n\t\tcwm->display_link = NULL;\n\t}\n\tcomp_window_macos_free_images(cwm);\n''',
    '''\tif (cwm->display_link != NULL) {\n\t\tCVDisplayLinkStop(cwm->display_link);\n\t\tCVDisplayLinkRelease(cwm->display_link);\n\t\tcwm->display_link = NULL;\n\t}\n\tmacos_timing_trace_close(cwm);\n\tcomp_window_macos_free_images(cwm);\n''',
)

replace_once(
    macos,
    '''\tstruct comp_window_macos *cwm = U_TYPED_CALLOC(struct comp_window_macos);\n\tcomp_target_swapchain_init_and_set_fnptrs(&cwm->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);\n''',
    '''\tstruct comp_window_macos *cwm = U_TYPED_CALLOC(struct comp_window_macos);\n\tmacos_timing_trace_open(cwm);\n\tcomp_target_swapchain_init_and_set_fnptrs(&cwm->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);\n''',
)

print("Applied macOS PSVR2 timing diagnostics")

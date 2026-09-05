// Copyright 2020-2021, Collabora, Ltd.
// Copyright 2023, Jan Schmidt
// Copyright 2024, Joel Valenciano
// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PSVR2 HMD device
 *
 * @author Jan Schmidt <jan@centricular.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Joel Valenciano <joelv1907@gmail.com>
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_psvr2
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <xrt/xrt_defines.h>
#include <xrt/xrt_byte_order.h>
#include "xrt/xrt_device.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_tracking.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_clock_tracking.h"
#include "math/m_mathinclude.h"
#include "math/m_relation_history.h"
#include "math/m_filter_one_euro.h"
#include "math/m_filter_fifo.h"

#include "tracking/t_dead_reckoning.h"

#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "util/u_frame.h"
#include "util/u_logging.h"
#include "util/u_sink.h"
#include "util/u_time.h"
#include "util/u_trace_marker.h"
#include "util/u_var.h"
#include "util/u_debug.h"

#include "psvr2_protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libusb.h>


#define NUM_CAM_XFERS 1

#define PSVR2_TRACE(p, ...) U_LOG_XDEV_IFL_T(&p->base, p->log_level, __VA_ARGS__)
#define PSVR2_TRACE_HEX(p, data, data_size) U_LOG_XDEV_IFL_T_HEX(&p->base, p->log_level, data, data_size)
#define PSVR2_DEBUG(p, ...) U_LOG_XDEV_IFL_D(&p->base, p->log_level, __VA_ARGS__)
#define PSVR2_DEBUG_HEX(p, data, data_size) U_LOG_XDEV_IFL_D_HEX(&p->base, p->log_level, data, data_size)
#define PSVR2_WARN(p, ...) U_LOG_XDEV_IFL_W(&p->base, p->log_level, __VA_ARGS__)
#define PSVR2_ERROR(p, ...) U_LOG_XDEV_IFL_E(&p->base, p->log_level, __VA_ARGS__)

#define TIMESTAMP_SAMPLES 100

struct imu_record
{
	uint32_t vts_us;
	int16_t accel[3];
	int16_t gyro[3];
	uint16_t dp_frame_cnt;
	uint16_t dp_line_cnt;
	uint16_t imu_ts_us;
	uint16_t status;
};

struct psvr2_et_eye_data
{
	bool gaze_point_valid;
	// gaze point in meters
	struct xrt_vec3 gaze_point;

	bool gaze_direction_valid;
	// gaze direction (normalized vector)
	struct xrt_vec3 gaze_direction;

	struct m_filter_euro_vec3 gaze_direction_filter;
	struct xrt_vec3 filtered_gaze_direction;

	// whether the pupil diameter is valid
	bool pupil_diameter_valid;
	// pupil diameter in meters
	float pupil_diameter;

	bool unk_float_2_valid;
	struct xrt_vec2 unk_float_2;

	bool unk_float_4_valid;
	struct xrt_vec2 unk_float_4;

	bool blink_valid;
	bool blink;

	float blink_interp;
};

struct psvr2_et_combined_data
{
	bool gaze_point_valid;
	struct xrt_vec3 gaze_point;

	bool gaze_direction_valid;
	struct xrt_vec3 gaze_direction;

	struct m_filter_euro_vec3 gaze_direction_filter;
	struct xrt_vec3 filtered_gaze_direction;

	bool is_valid;

	bool unk_float_8_valid;
	float unk_float_8;

	bool unk_float3_pair_valid;
	struct xrt_vec3 unk_float_12;
	struct xrt_vec3 unk_float_15;
	struct xrt_vec3 unk_float_18;
};

struct psvr2_et_data
{
	struct os_thread_helper eye_tracking_thread;

	bool want_enabled;
	bool force_enable;
	bool enabled;

	struct m_relation_history *gaze_relation_history;

	struct os_mutex data_mutex;
	bool data_mutex_created;

	struct psvr2_et_eye_data eyes[2];
	struct psvr2_et_combined_data combined;

	bool processed_sample_packet;

	uint32_t last_remote_report_sample_time_us;
	timepoint_ns last_remote_report_sample_time_ns;

	bool unk_float_4_valid;
	float unk_float_4;

	bool unk_float_5_valid;
	float unk_float_5;
};

/*!
 * PSVR2 HMD device
 *
 * @implements xrt_device
 */
struct psvr2_hmd
{
	struct xrt_device base;

	struct xrt_pose pose;

	enum u_logging_level log_level;

	struct os_mutex data_lock;
	bool data_lock_initialized;

	/* Device status */
	uint8_t dprx_status;
	xrt_atomic_s32_t proximity_sensor;
	bool function_button;

	bool ipd_updated;
	uint8_t ipd_mm;

	bool camera_enable;
	enum psvr2_camera_mode camera_mode;
	struct u_var_button camera_enable_btn;
	struct u_var_button camera_mode_btn;

	struct u_var_button brightness_btn;
	float brightness;

	/* IMU input data */
	uint32_t last_imu_vts_us;
	uint16_t last_imu_ts;
	struct xrt_vec3 last_gyro;
	struct xrt_vec3 last_accel;

	/* SLAM input data */
	uint32_t last_slam_vts_us;
	struct xrt_pose last_slam_pose;

	struct xrt_pose slam_correction_pose;
	struct u_var_button slam_correction_set_btn;
	struct u_var_button slam_correction_reset_btn;

	struct xrt_pose T_imu_head;

	/* Display parameters */
	struct u_device_simple_info info;

	/* Camera debug sinks */
	struct u_sink_debug debug_sinks[4];

	/* USB communication */
	libusb_context *ctx;
	libusb_device_handle *dev;
	bool auxiliary_streams_enabled;

	struct os_thread_helper usb_thread;
	int usb_complete;
	int usb_active_xfers;

	struct libusb_transfer *status_xfer;
	struct libusb_transfer *slam_xfer;
	struct libusb_transfer *camera_xfers[NUM_CAM_XFERS];
	struct libusb_transfer *led_detector_xfer;
	struct libusb_transfer *relocalizer_xfer;
	struct libusb_transfer *vd_xfer;
	struct libusb_transfer *gaze_xfer;

	float distortion_calibration[8];

	/* Timing data */
	int timestamp_samples;

	timepoint_ns last_imu_vts_ns;
	timepoint_ns last_slam_vts_ns;
	timepoint_ns system_zero_ns;
	timepoint_ns last_imu_ns;

	time_duration_ns hw2mono_vts;
	time_duration_ns hw2mono_imu;

	/* Tracking state */
	struct m_relation_history *slam_relation_history;
	struct m_ff_vec3_f32 *ff_gyro;
	uint64_t timing_query_count;
	time_duration_ns timing_prediction_total_ns;
	time_duration_ns timing_imu_after_slam_total_ns;
	time_duration_ns timing_prediction_after_imu_total_ns;

	/* Eye State */
	bool eye_feature_enabled;
	bool face_feature_enabled;

	struct psvr2_et_data et_data;
};

/// Casting helper function
static inline struct psvr2_hmd *
psvr2_hmd(struct xrt_device *xdev)
{
	return (struct psvr2_hmd *)xdev;
}

enum psvr2_hmd_input_name
{
	PSVR2_HMD_INPUT_HEAD_POSE,
	PSVR2_HMD_INPUT_FUNCTION_BUTTON,
	PSVR2_HMD_INPUT_EYE_GAZE_POSE,
	PSVR2_HMD_INPUT_FB_FACE_TRACKING2_VISUAL,
	PSVR2_HMD_INPUT_HTC_EYE_FACE_TRACKING,
	PSVR2_HMD_INPUT_ANDROID_FACE_TRACKING,
	PSVR2_HMD_INPUT_COUNT,
};

void
psvr2_compute_distortion_asymmetric(
    float *calibration, struct xrt_uv_triplet *distCoords, int eEye, float fU, float fV);

bool
psvr2_usb_xfer_continue(struct libusb_transfer *xfer, const char *type);

bool
send_psvr2_control(struct psvr2_hmd *hmd, uint16_t report_id, uint8_t subcmd, uint8_t *pkt_data, uint32_t pkt_len);

void
psvr2_free_et_data(struct psvr2_hmd *hmd);

int
psvr2_start_gaze_tracking(struct psvr2_hmd *hmd);

xrt_result_t
psvr2_get_face_tracking(struct xrt_device *xdev,
                        enum xrt_input_name facial_expression_type,
                        int64_t at_timestamp_ns,
                        struct xrt_facial_expression_set *out_value);

/*
 * Experimental PSVR2 orientation predictor, modelled on the GAV macOS player.
 * Keep Monado's ordinary dead reckoning for position and as the default path,
 * but optionally replace the predicted orientation by integrating every real
 * high-rate gyro sample newer than the latest SLAM pose, then extrapolating
 * only the remainder from the newest IMU sample to the requested timestamp.
 */
static inline bool
psvr2_explicit_gyro_integration_enabled(void)
{
	static int enabled = -1;
	if (enabled >= 0) {
		return enabled != 0;
	}

	const char *value = getenv("PSVR2_GYRO_INTEGRATION");
	enabled = value != NULL &&
	          (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "TRUE") == 0 ||
	           strcmp(value, "yes") == 0 || strcmp(value, "YES") == 0);
	if (enabled) {
		fprintf(stderr,
		        "PSVR2: explicit gyro integration enabled (SLAM -> 2 kHz IMU samples -> prediction target)\n");
	}
	return enabled != 0;
}

static inline struct xrt_quat
psvr2_quat_multiply(struct xrt_quat a, struct xrt_quat b)
{
	return (struct xrt_quat){
	    .x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	    .y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	    .z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	    .w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

static inline void
psvr2_integrate_gyro_step(struct xrt_quat *orientation, const struct xrt_vec3 *gyro, time_duration_ns dt_ns)
{
	if (dt_ns <= 0) {
		return;
	}

	const float dt_s = (float)((double)dt_ns / (double)U_TIME_1S_IN_NS);
	const float speed = sqrtf(gyro->x * gyro->x + gyro->y * gyro->y + gyro->z * gyro->z);
	struct xrt_quat delta;
	if (speed < 1e-8f) {
		delta = (struct xrt_quat){
		    .x = gyro->x * dt_s * 0.5f,
		    .y = gyro->y * dt_s * 0.5f,
		    .z = gyro->z * dt_s * 0.5f,
		    .w = 1.0f,
		};
	} else {
		const float half_angle = speed * dt_s * 0.5f;
		const float scale = sinf(half_angle) / speed;
		delta = (struct xrt_quat){
		    .x = gyro->x * scale,
		    .y = gyro->y * scale,
		    .z = gyro->z * scale,
		    .w = cosf(half_angle),
		};
	}

	*orientation = psvr2_quat_multiply(*orientation, delta);
	math_quat_normalize(orientation);
}

static inline void
psvr2_apply_dead_reckoning(struct m_ff_vec3_f32 *gyro_ff,
                           struct m_ff_vec3_f32 *accel_ff,
                           const struct xrt_vec3 *gravity_correction,
                           timepoint_ns when_ns,
                           const struct xrt_space_relation *base_rel,
                           timepoint_ns base_rel_ts,
                           struct xrt_space_relation *out_relation)
{
	/* Preserve the existing Monado result, especially its position prediction. */
	t_apply_dead_reckoning(gyro_ff, accel_ff, gravity_correction, when_ns, base_rel, base_rel_ts, out_relation);

	if (!psvr2_explicit_gyro_integration_enabled() || gyro_ff == NULL || when_ns <= base_rel_ts) {
		return;
	}

	struct xrt_quat orientation = base_rel->pose.orientation;
	timepoint_ns cursor_ns = base_rel_ts;
	struct xrt_vec3 newest_gyro = {0};
	bool have_integrated_sample = false;

	/*
	 * FIFO index zero is newest. Walk it backwards so samples are integrated
	 * in chronological order, using their actual VTS-derived timestamps.
	 */
	const size_t sample_count = m_ff_vec3_f32_get_num(gyro_ff);
	for (size_t n = sample_count; n > 0; --n) {
		struct xrt_vec3 gyro;
		uint64_t sample_ts_u64 = 0;
		if (!m_ff_vec3_f32_get(gyro_ff, n - 1, &gyro, &sample_ts_u64)) {
			continue;
		}
		const timepoint_ns sample_ts = (timepoint_ns)sample_ts_u64;
		if (sample_ts <= base_rel_ts || sample_ts > when_ns) {
			continue;
		}
		if (sample_ts > cursor_ns) {
			psvr2_integrate_gyro_step(&orientation, &gyro, sample_ts - cursor_ns);
			cursor_ns = sample_ts;
			newest_gyro = gyro;
			have_integrated_sample = true;
		}
	}

	/*
	 * If no sample is newer than SLAM (the streams are independent), use the
	 * newest available gyro for the whole remainder, matching the existing
	 * driver's high-rate fallback. Otherwise extrapolate only after the newest
	 * measured sample.
	 */
	if (!have_integrated_sample) {
		uint64_t sample_ts = 0;
		if (!m_ff_vec3_f32_get(gyro_ff, 0, &newest_gyro, &sample_ts)) {
			return;
		}
	}

	if (when_ns > cursor_ns) {
		psvr2_integrate_gyro_step(&orientation, &newest_gyro, when_ns - cursor_ns);
	}

	out_relation->pose.orientation = orientation;
	math_quat_rotate_derivative(&orientation, &newest_gyro, &out_relation->angular_velocity);
	out_relation->relation_flags = (enum xrt_space_relation_flags)(
	    out_relation->relation_flags | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
}

/* Replace only calls occurring after this header is included (not the call
 * inside psvr2_apply_dead_reckoning above). This keeps the experiment confined
 * to the PSVR2 driver without changing the generic dead-reckoning helper. */
#define t_apply_dead_reckoning psvr2_apply_dead_reckoning

#ifdef __cplusplus
}
#endif

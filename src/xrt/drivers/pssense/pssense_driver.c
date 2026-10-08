// Copyright 2023, Collabora, Ltd.
// Copyright 2023, Jarett Millard
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PlayStation Sense controller prober and driver code.
 * @author Jarett Millard <jarett.millard@gmail.com>
 * @ingroup drv_pssense
 */

#include "xrt/xrt_prober.h"

#include "os/os_hid.h"
#include "os/os_threading.h"
#include "os/os_time.h"

#include "math/m_api.h"
#include "math/m_vec3.h"

#include "tracking/t_constellation.h"
#include "tracking/t_imu.h"

#include "constellation/t_constellation_tracker.h"
#include "constellation/t_led_sync_refinement.h"
#include "constellation/t_led_phase_bootstrap.h"
#include "constellation/t_imu_optical_filter.h"

#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_linux.h"
#include "util/u_logging.h"
#include "util/u_resampler.h"
#include "util/u_time.h"
#include "util/u_trace_marker.h"
#include "util/u_var.h"

#include "math/m_clock_tracking.h"
#include "math/m_imu_3dof.h"
#include "math/m_relation_history.h"
#include "math/m_space.h"

#include "pssense_interface.h"
#include "pssense_led_model.h"
#include "pssense_protocol.h"
#include "pssense_led_correction.h"
#include "pssense_clock.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>


/*!
 * @addtogroup drv_pssense
 * @{
 */

#define PSSENSE_TRACE(p, ...) U_LOG_XDEV_IFL_T(&p->base, p->log_level, __VA_ARGS__)
#define PSSENSE_DEBUG(p, ...) U_LOG_XDEV_IFL_D(&p->base, p->log_level, __VA_ARGS__)
#define PSSENSE_DEBUG_HEX(p, data, data_size) U_LOG_XDEV_IFL_D_HEX(&p->base, p->log_level, data, data_size)
#define PSSENSE_INFO(p, ...) U_LOG_XDEV_IFL_I(&p->base, p->log_level, __VA_ARGS__)
#define PSSENSE_WARN(p, ...) U_LOG_XDEV_IFL_W(&p->base, p->log_level, __VA_ARGS__)
#define PSSENSE_ERROR(p, ...) U_LOG_XDEV_IFL_E(&p->base, p->log_level, __VA_ARGS__)

#define PSSENSE_CONSTELLATION_GROUP_COUNT 32
#define PSSENSE_CONSTELLATION_CAMERA_COUNT 4
#define PSSENSE_CONSTELLATION_SYNC_TOLERANCE_NS U_TIME_1MS_IN_NS
#define PSSENSE_CONSTELLATION_STALE_NS (250 * U_TIME_1MS_IN_NS)
#define PSSENSE_CONSTELLATION_MAX_CAMERA_POSITION_DELTA_M 0.08f
#define PSSENSE_CONSTELLATION_MAX_CAMERA_ORIENTATION_DELTA_RAD (35.0f * (float)M_PI / 180.0f)
#define PSSENSE_CONSTELLATION_MAX_JUMP_POSITION_M 0.15f
#define PSSENSE_CONSTELLATION_MAX_JUMP_ORIENTATION_RAD (60.0f * (float)M_PI / 180.0f)

DEBUG_GET_ONCE_LOG_OPTION(pssense_log, "PSSENSE_LOG", U_LOGGING_INFO)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_synthetic_position, "PSSENSE_SYNTHETIC_POSITION", false)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_index_profile, "PSSENSE_INDEX_PROFILE", false)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_synthetic_arm_model, "PSSENSE_SYNTHETIC_ARM_MODEL", false)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_input_diagnostics, "PSSENSE_INPUT_DIAGNOSTICS", false)

DEBUG_GET_ONCE_BOOL_OPTION(pssense_pc_polling_rate, "PSSENSE_SET_PC_POLLING_RATE", true)
#ifdef XRT_OS_OSX
#define PSSENSE_FUTURE_LED_SCHEDULE_DEFAULT false
#else
#define PSSENSE_FUTURE_LED_SCHEDULE_DEFAULT false
#endif
DEBUG_GET_ONCE_BOOL_OPTION(pssense_future_led_schedule,
                           "PSSENSE_FUTURE_LED_SCHEDULE",
                           PSSENSE_FUTURE_LED_SCHEDULE_DEFAULT)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_timing_diag, "PSSENSE_TIMING_DIAG", false)
DEBUG_GET_ONCE_NUM_OPTION(pssense_led_period_id, "PSSENSE_LED_PERIOD_ID", -1)
DEBUG_GET_ONCE_NUM_OPTION(pssense_timing_fudge_100us, "PSSENSE_TIMING_FUDGE_100US", LONG_MIN)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_led_bootstrap, "PSSENSE_LED_BOOTSTRAP", false)
DEBUG_GET_ONCE_NUM_OPTION(pssense_led_bootstrap_lock_period_id, "PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID", 20)
DEBUG_GET_ONCE_NUM_OPTION(pssense_clock_offset_snap_us, "PSSENSE_CLOCK_OFFSET_SNAP_US", 0)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_clock_steady, "PSSENSE_CLOCK_STEADY", false)
DEBUG_GET_ONCE_NUM_OPTION(pssense_led_latch_interval_ms, "PSSENSE_LED_LATCH_INTERVAL_MS", 0)
DEBUG_GET_ONCE_NUM_OPTION(pssense_led_broad_s, "PSSENSE_LED_BROAD_S", 0)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_led_nominal_cycle, "PSSENSE_LED_NOMINAL_CYCLE", false)
DEBUG_GET_ONCE_OPTION(pssense_led_blink_sweep, "PSSENSE_LED_BLINK_SWEEP", "")
DEBUG_GET_ONCE_NUM_OPTION(pssense_led_blink_sweep_s, "PSSENSE_LED_BLINK_SWEEP_S", 4)
//! One nominal 59.94 Hz camera frame in thirds of a nanosecond, as observed in every Sony output report.
#define PSSENSE_NOMINAL_CYCLE_LENGTH 50050050u
DEBUG_GET_ONCE_NUM_OPTION(pssense_led_broad_period_id, "PSSENSE_LED_BROAD_PERIOD_ID", 0)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_led_bootstrap_blob_fallback, "PSSENSE_LED_BOOTSTRAP_BLOB_FALLBACK", false)
DEBUG_GET_ONCE_NUM_OPTION(pssense_led_bootstrap_lost_lit_percent, "PSSENSE_LED_BOOTSTRAP_LOST_LIT_PERCENT", 10)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_led_bootstrap_full_scan_fallback, "PSSENSE_LED_BOOTSTRAP_FULL_SCAN_FALLBACK", false)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_led_bootstrap_quick_lock, "PSSENSE_LED_BOOTSTRAP_QUICK_LOCK", false)
#ifdef XRT_OS_OSX
DEBUG_GET_ONCE_BOOL_OPTION(pssense_reconnect, "PSSENSE_RECONNECT", false)
#endif
DEBUG_GET_ONCE_BOOL_OPTION(pssense_led_bootstrap_keep_lock, "PSSENSE_LED_BOOTSTRAP_KEEP_LOCK", false)
DEBUG_GET_ONCE_NUM_OPTION(pssense_led_bootstrap_track_frames, "PSSENSE_LED_BOOTSTRAP_TRACK_FRAMES", 120)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_led_bootstrap_track, "PSSENSE_LED_BOOTSTRAP_TRACK", false)
DEBUG_GET_ONCE_OPTION(pssense_led_bootstrap_first, "PSSENSE_LED_BOOTSTRAP_FIRST", "")
DEBUG_GET_ONCE_BOOL_OPTION(pssense_align_imu_orientation, "PSSENSE_ALIGN_IMU_ORIENTATION", false)
/*
 * Score LED illumination by the tracker's per-controller LED-shaped blob counts (other controllers' claimed blobs,
 * lamps and window glare removed) instead of raw blob counts.
 */
DEBUG_GET_ONCE_BOOL_OPTION(pssense_led_bootstrap_led_blobs, "PSSENSE_LED_BOOTSTRAP_LED_BLOBS", false)
/*
 * Stricter bootstrap: reject locks seen by fewer than two cameras' worth of lit frames, and track only a ring that
 * added at least three blobs per camera (a smaller one turns every blob of noise into a full-scale correction).
 */
DEBUG_GET_ONCE_BOOL_OPTION(pssense_led_bootstrap_strict, "PSSENSE_LED_BOOTSTRAP_STRICT", false)
/*!
 * Start the first scan as a short narrow scan around this lit-window centre (the bootstrap's centre_us; locks have
 * been 16100-16600 us on this setup) instead of the full 38-step scan; rescans then start around the last lock.
 * Unset (-1) for the full scan. The always-lit fault starts during scans at ~0.4% per step.
 */
DEBUG_GET_ONCE_NUM_OPTION(pssense_led_bootstrap_hint_us, "PSSENSE_LED_BOOTSTRAP_HINT_US", -1)
//! Phase-tracking probes score joint-solve pose coverage (fraction of visible LEDs matched) instead of blob counts.
DEBUG_GET_ONCE_BOOL_OPTION(pssense_led_bootstrap_track_coverage, "PSSENSE_LED_BOOTSTRAP_TRACK_COVERAGE", false)
/*
 * Estimate the gyro bias online while the controller is still (gyro and accelerometer steady, reading 1 g) and subtract
 * it. The factory bias alone leaves the right Sense turning at 16-20 deg/s at rest in every session since 25 Sep (the
 * left at 2-4 deg/s), which the joint tracker's 3-degree orientation prior cannot absorb for long.
 */
DEBUG_GET_ONCE_BOOL_OPTION(pssense_gyro_bias_auto, "PSSENSE_GYRO_BIAS_AUTO", false)
/*
 * Fuse the IMU and the optical poses with an error-state EKF (t_imu_optical_filter) and report its pose: position and
 * orientation both in the optical (world) frame, predicted through optical gaps. Without it the output takes position
 * from interpolated optical poses and orientation from the IMU fusion's own world.
 */
DEBUG_GET_ONCE_BOOL_OPTION(pssense_filter, "PSSENSE_FILTER", false)
DEBUG_GET_ONCE_BOOL_OPTION(pssense_joint, "CONSTELLATION_TRACKER_JOINT", false)
/*
 * On shutdown, send LED_ALL_OFF for ~150 ms before closing, instead of stopping mid-schedule. The always-lit fault has
 * been seen "at the end" of a session (26 Sep, left, 004811), and two of the right's failed hinted scans came in the
 * first session after one that ended without a power cycle.
 */
DEBUG_GET_ONCE_BOOL_OPTION(pssense_leds_off_on_exit, "PSSENSE_LEDS_OFF_ON_EXIT", false)
/*
 * Log every change in the input-report bytes the driver does not otherwise use (unknown fields, the controller's CRC
 * failure count and padding), and a periodic count of changes per byte. For finding a controller-side flag when the
 * always-lit fault starts.
 */
DEBUG_GET_ONCE_BOOL_OPTION(pssense_input_diag, "PSSENSE_INPUT_DIAG", false)
/*
 * Pulse width (period id) for the LED bootstrap's wide scan; default MAX_PERIOD_ID (42, 2.1 ms). All seven located
 * onsets of the always-lit fault followed period-42 pulses within 1.5 s; PSVR2Toolkit's own latency calibration never
 * uses more than 32 (1.6 ms).
 */
DEBUG_GET_ONCE_NUM_OPTION(pssense_led_bootstrap_wide_period_id, "PSSENSE_LED_BOOTSTRAP_WIDE_PERIOD_ID", -1)

/*
 * Stress test for the always-lit fault: once a controller has been locked this many seconds, and no other controller
 * holds the scan token, rescan it from scratch (a full scan unless PSSENSE_LED_BOOTSTRAP_HINT_US is set). 0 = off.
 */
DEBUG_GET_ONCE_NUM_OPTION(pssense_led_bootstrap_stress_rescan_s, "PSSENSE_LED_BOOTSTRAP_STRESS_RESCAN_S", 0)
/*
 * Move each LED of the model by its measured offset (pssense_led_correction.h) before the model goes to the
 * constellation tracker. Replayed on recordings the corrected rings fit the cameras at 0.38-0.40 px instead of
 * 0.42-0.53 px, and the right controller gains 10% more poses. Fitted with the combined mode-4 calibration of 25 Sep
 * and worse with the older one. Not yet run on the headset.
 */
DEBUG_GET_ONCE_BOOL_OPTION(pssense_led_correction, "PSSENSE_LED_CORRECTION", false)
_Static_assert(ARRAY_SIZE(pssense_left_led_corrections) == ARRAY_SIZE(pssense_left_leds) &&
                   ARRAY_SIZE(pssense_right_led_corrections) == ARRAY_SIZE(pssense_right_leds) &&
                   ARRAY_SIZE(pssense_left_leds) == ARRAY_SIZE(pssense_right_leds),
               "one correction per LED, and both rings the same size");

//! Unused input-report bytes watched by PSSENSE_INPUT_DIAG.
#define PSSENSE_INPUT_DIAG_BYTES 24

//! Stillness statistics time constant, and how long the controller must be still before its mean gyro is the bias.
#define PSSENSE_GYRO_BIAS_TAU_S 0.25
#define PSSENSE_GYRO_BIAS_STILL_NS (600 * U_TIME_1MS_IN_NS)
//! Still: gyro standard deviation below this (rad/s), accelerometer standard deviation below this (m/s^2), and the
//! accelerometer within this of 1 g (m/s^2).
#define PSSENSE_GYRO_BIAS_MAX_GYRO_STD 0.03
#define PSSENSE_GYRO_BIAS_MAX_ACCEL_STD 0.10
#define PSSENSE_GYRO_BIAS_MAX_GRAVITY_ERROR 0.6

#define PSSENSE_FUTURE_LED_LEAD_NS (50 * U_TIME_1MS_IN_NS)

static struct xrt_binding_input_pair touch_inputs_pssense[] = {
    {XRT_INPUT_TOUCH_X_CLICK, XRT_INPUT_PSSENSE_SQUARE_CLICK},
    {XRT_INPUT_TOUCH_X_TOUCH, XRT_INPUT_PSSENSE_SQUARE_TOUCH},
    {XRT_INPUT_TOUCH_Y_CLICK, XRT_INPUT_PSSENSE_TRIANGLE_CLICK},
    {XRT_INPUT_TOUCH_Y_TOUCH, XRT_INPUT_PSSENSE_TRIANGLE_TOUCH},
    {XRT_INPUT_TOUCH_MENU_CLICK, XRT_INPUT_PSSENSE_PS_CLICK},
    {XRT_INPUT_TOUCH_A_CLICK, XRT_INPUT_PSSENSE_CROSS_CLICK},
    {XRT_INPUT_TOUCH_A_TOUCH, XRT_INPUT_PSSENSE_CROSS_TOUCH},
    {XRT_INPUT_TOUCH_B_CLICK, XRT_INPUT_PSSENSE_CIRCLE_CLICK},
    {XRT_INPUT_TOUCH_B_TOUCH, XRT_INPUT_PSSENSE_CIRCLE_TOUCH},
    {XRT_INPUT_TOUCH_SYSTEM_CLICK, XRT_INPUT_PSSENSE_PS_CLICK},
    {XRT_INPUT_TOUCH_SQUEEZE_VALUE, XRT_INPUT_PSSENSE_SQUEEZE_CLICK},
    {XRT_INPUT_TOUCH_TRIGGER_TOUCH, XRT_INPUT_PSSENSE_TRIGGER_TOUCH},
    {XRT_INPUT_TOUCH_TRIGGER_VALUE, XRT_INPUT_PSSENSE_TRIGGER_VALUE},
    {XRT_INPUT_TOUCH_THUMBSTICK_CLICK, XRT_INPUT_PSSENSE_THUMBSTICK_CLICK},
    {XRT_INPUT_TOUCH_THUMBSTICK_TOUCH, XRT_INPUT_PSSENSE_THUMBSTICK_TOUCH},
    {XRT_INPUT_TOUCH_THUMBSTICK, XRT_INPUT_PSSENSE_THUMBSTICK},
    {XRT_INPUT_TOUCH_GRIP_POSE, XRT_INPUT_PSSENSE_GRIP_POSE},
    {XRT_INPUT_TOUCH_AIM_POSE, XRT_INPUT_PSSENSE_AIM_POSE},
    {XRT_INPUT_TOUCH_TRIGGER_PROXIMITY, XRT_INPUT_PSSENSE_TRIGGER_PROXIMITY},
};

static struct xrt_binding_output_pair touch_outputs_pssense[] = {
    {XRT_OUTPUT_NAME_TOUCH_HAPTIC, XRT_OUTPUT_NAME_PSSENSE_VIBRATION},
};

static struct xrt_binding_input_pair simple_inputs_pssense[] = {
    {XRT_INPUT_SIMPLE_SELECT_CLICK, XRT_INPUT_PSSENSE_TRIGGER_VALUE},
    {XRT_INPUT_SIMPLE_MENU_CLICK, XRT_INPUT_PSSENSE_OPTIONS_CLICK},
    {XRT_INPUT_SIMPLE_GRIP_POSE, XRT_INPUT_PSSENSE_GRIP_POSE},
    {XRT_INPUT_SIMPLE_AIM_POSE, XRT_INPUT_PSSENSE_AIM_POSE},
};

static struct xrt_binding_output_pair simple_outputs_pssense[] = {
    {XRT_OUTPUT_NAME_SIMPLE_VIBRATION, XRT_OUTPUT_NAME_PSSENSE_VIBRATION},
};

/*
 * XR_KHR_generic_controller is a hardware-neutral fallback interaction
 * profile. Keep the mapping explicit so OpenXR applications can bind to
 * standard generic paths without pretending Sense is another vendor's
 * controller.
 *
 * L1/R1 is the closest squeeze actuator. Monado's existing input transform
 * converts its boolean click to the 0/1 float required by squeeze/value,
 * avoiding a second driver-level representation of the same physical input.
 * grip_surface first uses a driver's calibrated generic palm pose when one
 * is exposed, with the existing grip pose retained as a last-resort fallback.
 */
static struct xrt_binding_input_pair generic_inputs_pssense_left[] = {
    {XRT_INPUT_GENERIC_PRIMARY_CLICK, XRT_INPUT_PSSENSE_SQUARE_CLICK},
    {XRT_INPUT_GENERIC_SECONDARY_CLICK, XRT_INPUT_PSSENSE_TRIANGLE_CLICK},
    {XRT_INPUT_GENERIC_THUMBSTICK_CLICK, XRT_INPUT_PSSENSE_THUMBSTICK_CLICK},
    {XRT_INPUT_GENERIC_THUMBSTICK, XRT_INPUT_PSSENSE_THUMBSTICK},
    {XRT_INPUT_GENERIC_SQUEEZE_VALUE, XRT_INPUT_PSSENSE_SQUEEZE_CLICK},
    {XRT_INPUT_GENERIC_TRIGGER_VALUE, XRT_INPUT_PSSENSE_TRIGGER_VALUE},
    {XRT_INPUT_GENERIC_GRIP_POSE, XRT_INPUT_PSSENSE_GRIP_POSE},
    {XRT_INPUT_GENERIC_GRIP_SURFACE_POSE, XRT_INPUT_GENERIC_PALM_POSE},
    {XRT_INPUT_GENERIC_GRIP_SURFACE_POSE, XRT_INPUT_PSSENSE_GRIP_POSE},
    {XRT_INPUT_GENERIC_AIM_POSE, XRT_INPUT_PSSENSE_AIM_POSE},
};

static struct xrt_binding_input_pair generic_inputs_pssense_right[] = {
    {XRT_INPUT_GENERIC_PRIMARY_CLICK, XRT_INPUT_PSSENSE_CROSS_CLICK},
    {XRT_INPUT_GENERIC_SECONDARY_CLICK, XRT_INPUT_PSSENSE_CIRCLE_CLICK},
    {XRT_INPUT_GENERIC_THUMBSTICK_CLICK, XRT_INPUT_PSSENSE_THUMBSTICK_CLICK},
    {XRT_INPUT_GENERIC_THUMBSTICK, XRT_INPUT_PSSENSE_THUMBSTICK},
    {XRT_INPUT_GENERIC_SQUEEZE_VALUE, XRT_INPUT_PSSENSE_SQUEEZE_CLICK},
    {XRT_INPUT_GENERIC_TRIGGER_VALUE, XRT_INPUT_PSSENSE_TRIGGER_VALUE},
    {XRT_INPUT_GENERIC_GRIP_POSE, XRT_INPUT_PSSENSE_GRIP_POSE},
    {XRT_INPUT_GENERIC_GRIP_SURFACE_POSE, XRT_INPUT_GENERIC_PALM_POSE},
    {XRT_INPUT_GENERIC_GRIP_SURFACE_POSE, XRT_INPUT_PSSENSE_GRIP_POSE},
    {XRT_INPUT_GENERIC_AIM_POSE, XRT_INPUT_PSSENSE_AIM_POSE},
};

static struct xrt_binding_output_pair generic_outputs_pssense[] = {
    {XRT_OUTPUT_NAME_GENERIC_VIBRATION, XRT_OUTPUT_NAME_PSSENSE_VIBRATION},
};

static struct xrt_binding_input_pair index_inputs_pssense_left[] = {
    {XRT_INPUT_INDEX_SYSTEM_CLICK, XRT_INPUT_PSSENSE_PS_CLICK},
    {XRT_INPUT_INDEX_A_CLICK, XRT_INPUT_PSSENSE_SQUARE_CLICK},
    {XRT_INPUT_INDEX_A_TOUCH, XRT_INPUT_PSSENSE_SQUARE_TOUCH},
    {XRT_INPUT_INDEX_B_CLICK, XRT_INPUT_PSSENSE_TRIANGLE_CLICK},
    {XRT_INPUT_INDEX_B_TOUCH, XRT_INPUT_PSSENSE_TRIANGLE_TOUCH},
    {XRT_INPUT_INDEX_SQUEEZE_VALUE, XRT_INPUT_PSSENSE_SQUEEZE_PROXIMITY_FLOAT},
    {XRT_INPUT_INDEX_SQUEEZE_FORCE, XRT_INPUT_PSSENSE_SQUEEZE_PROXIMITY_FLOAT},
    {XRT_INPUT_INDEX_TRIGGER_CLICK, XRT_INPUT_PSSENSE_TRIGGER_CLICK},
    {XRT_INPUT_INDEX_TRIGGER_TOUCH, XRT_INPUT_PSSENSE_TRIGGER_TOUCH},
    {XRT_INPUT_INDEX_TRIGGER_VALUE, XRT_INPUT_PSSENSE_TRIGGER_VALUE},
    {XRT_INPUT_INDEX_THUMBSTICK, XRT_INPUT_PSSENSE_THUMBSTICK},
    {XRT_INPUT_INDEX_THUMBSTICK_CLICK, XRT_INPUT_PSSENSE_THUMBSTICK_CLICK},
    {XRT_INPUT_INDEX_THUMBSTICK_TOUCH, XRT_INPUT_PSSENSE_THUMBSTICK_TOUCH},
    {XRT_INPUT_INDEX_GRIP_POSE, XRT_INPUT_PSSENSE_GRIP_POSE},
    {XRT_INPUT_INDEX_AIM_POSE, XRT_INPUT_PSSENSE_AIM_POSE},
};

static struct xrt_binding_input_pair index_inputs_pssense_right[] = {
    {XRT_INPUT_INDEX_SYSTEM_CLICK, XRT_INPUT_PSSENSE_PS_CLICK},
    {XRT_INPUT_INDEX_A_CLICK, XRT_INPUT_PSSENSE_CROSS_CLICK},
    {XRT_INPUT_INDEX_A_TOUCH, XRT_INPUT_PSSENSE_CROSS_TOUCH},
    {XRT_INPUT_INDEX_B_CLICK, XRT_INPUT_PSSENSE_CIRCLE_CLICK},
    {XRT_INPUT_INDEX_B_TOUCH, XRT_INPUT_PSSENSE_CIRCLE_TOUCH},
    {XRT_INPUT_INDEX_SQUEEZE_VALUE, XRT_INPUT_PSSENSE_SQUEEZE_PROXIMITY_FLOAT},
    {XRT_INPUT_INDEX_SQUEEZE_FORCE, XRT_INPUT_PSSENSE_SQUEEZE_PROXIMITY_FLOAT},
    {XRT_INPUT_INDEX_TRIGGER_CLICK, XRT_INPUT_PSSENSE_TRIGGER_CLICK},
    {XRT_INPUT_INDEX_TRIGGER_TOUCH, XRT_INPUT_PSSENSE_TRIGGER_TOUCH},
    {XRT_INPUT_INDEX_TRIGGER_VALUE, XRT_INPUT_PSSENSE_TRIGGER_VALUE},
    {XRT_INPUT_INDEX_THUMBSTICK, XRT_INPUT_PSSENSE_THUMBSTICK},
    {XRT_INPUT_INDEX_THUMBSTICK_CLICK, XRT_INPUT_PSSENSE_THUMBSTICK_CLICK},
    {XRT_INPUT_INDEX_THUMBSTICK_TOUCH, XRT_INPUT_PSSENSE_THUMBSTICK_TOUCH},
    {XRT_INPUT_INDEX_GRIP_POSE, XRT_INPUT_PSSENSE_GRIP_POSE},
    {XRT_INPUT_INDEX_AIM_POSE, XRT_INPUT_PSSENSE_AIM_POSE},
};

static struct xrt_binding_output_pair index_outputs_pssense[] = {
    {XRT_OUTPUT_NAME_INDEX_HAPTIC, XRT_OUTPUT_NAME_PSSENSE_VIBRATION},
};

static struct xrt_binding_profile binding_profiles_pssense_left[] = {
    {
        .name = XRT_DEVICE_GENERIC_CONTROLLER,
        .inputs = generic_inputs_pssense_left,
        .input_count = ARRAY_SIZE(generic_inputs_pssense_left),
        .outputs = generic_outputs_pssense,
        .output_count = ARRAY_SIZE(generic_outputs_pssense),
    },
    {
        .name = XRT_DEVICE_TOUCH_CONTROLLER,
        .inputs = touch_inputs_pssense,
        .input_count = ARRAY_SIZE(touch_inputs_pssense),
        .outputs = touch_outputs_pssense,
        .output_count = ARRAY_SIZE(touch_outputs_pssense),
    },
    {
        .name = XRT_DEVICE_SIMPLE_CONTROLLER,
        .inputs = simple_inputs_pssense,
        .input_count = ARRAY_SIZE(simple_inputs_pssense),
        .outputs = simple_outputs_pssense,
        .output_count = ARRAY_SIZE(simple_outputs_pssense),
    },
    {
        .name = XRT_DEVICE_INDEX_CONTROLLER,
        .inputs = index_inputs_pssense_left,
        .input_count = ARRAY_SIZE(index_inputs_pssense_left),
        .outputs = index_outputs_pssense,
        .output_count = ARRAY_SIZE(index_outputs_pssense),
    },
};

static struct xrt_binding_profile binding_profiles_pssense_right[] = {
    {
        .name = XRT_DEVICE_GENERIC_CONTROLLER,
        .inputs = generic_inputs_pssense_right,
        .input_count = ARRAY_SIZE(generic_inputs_pssense_right),
        .outputs = generic_outputs_pssense,
        .output_count = ARRAY_SIZE(generic_outputs_pssense),
    },
    {
        .name = XRT_DEVICE_TOUCH_CONTROLLER,
        .inputs = touch_inputs_pssense,
        .input_count = ARRAY_SIZE(touch_inputs_pssense),
        .outputs = touch_outputs_pssense,
        .output_count = ARRAY_SIZE(touch_outputs_pssense),
    },
    {
        .name = XRT_DEVICE_SIMPLE_CONTROLLER,
        .inputs = simple_inputs_pssense,
        .input_count = ARRAY_SIZE(simple_inputs_pssense),
        .outputs = simple_outputs_pssense,
        .output_count = ARRAY_SIZE(simple_outputs_pssense),
    },
    {
        .name = XRT_DEVICE_INDEX_CONTROLLER,
        .inputs = index_inputs_pssense_right,
        .input_count = ARRAY_SIZE(index_inputs_pssense_right),
        .outputs = index_outputs_pssense,
        .output_count = ARRAY_SIZE(index_outputs_pssense),
    },
};

/*!
 * Indices where each input is in the input list.
 */
enum pssense_input_index
{
	PSSENSE_INDEX_PS_CLICK,
	PSSENSE_INDEX_SHARE_CLICK,
	PSSENSE_INDEX_OPTIONS_CLICK,
	PSSENSE_INDEX_SQUARE_CLICK,
	PSSENSE_INDEX_SQUARE_TOUCH,
	PSSENSE_INDEX_TRIANGLE_CLICK,
	PSSENSE_INDEX_TRIANGLE_TOUCH,
	PSSENSE_INDEX_CROSS_CLICK,
	PSSENSE_INDEX_CROSS_TOUCH,
	PSSENSE_INDEX_CIRCLE_CLICK,
	PSSENSE_INDEX_CIRCLE_TOUCH,
	PSSENSE_INDEX_SQUEEZE_CLICK,
	PSSENSE_INDEX_SQUEEZE_TOUCH,
	PSSENSE_INDEX_SQUEEZE_PROXIMITY,
	PSSENSE_INDEX_SQUEEZE_PROXIMITY_FLOAT,
	PSSENSE_INDEX_TRIGGER_CLICK,
	PSSENSE_INDEX_TRIGGER_TOUCH,
	PSSENSE_INDEX_TRIGGER_VALUE,
	PSSENSE_INDEX_TRIGGER_PROXIMITY,
	PSSENSE_INDEX_TRIGGER_PROXIMITY_FLOAT,
	PSSENSE_INDEX_THUMBSTICK,
	PSSENSE_INDEX_THUMBSTICK_CLICK,
	PSSENSE_INDEX_THUMBSTICK_TOUCH,
	PSSENSE_INDEX_GRIP_POSE,
	PSSENSE_INDEX_AIM_POSE,
	PSSENSE_INPUT_COUNT,
};

/*!
 * Parsed calibration data from the PlayStation Sense controller.
 */
struct pssense_parsed_calibration
{
	struct xrt_vec3 gyro_scale;
	struct xrt_vec3 accel_scale;

	struct xrt_vec3 accel_bias;
	struct xrt_vec3_i32 gyro_bias;
};

/*!
 * PlayStation Sense state parsed from a data packet.
 */
struct pssense_input_state
{
	uint64_t timestamp_ns;
	uint32_t seq_no;

	bool ps_click;
	bool share_click;
	bool options_click;
	bool square_click;
	bool square_touch;
	bool triangle_click;
	bool triangle_touch;
	bool cross_click;
	bool cross_touch;
	bool circle_click;
	bool circle_touch;
	bool squeeze_click;
	bool squeeze_touch;
	float squeeze_proximity;
	bool trigger_click;
	bool trigger_touch;
	float trigger_value;
	float trigger_proximity;
	bool thumbstick_click;
	bool thumbstick_touch;
	struct xrt_vec2 thumbstick;

	struct xrt_vec3_i32 gyro_raw;
	struct xrt_vec3_i32 accel_raw;

	bool battery_state_valid;
	bool battery_charging;
	//! Charge level from 0..1
	float battery_charge_percent;
};

/*!
 * A single PlayStation Sense Controller.
 *
 * @implements xrt_device
 * @implements xrt_frame_node
 * @implements t_timing_event_sink
 * @implements t_constellation_tracker_led_model
 * @implements t_constellation_tracker_device
 */
//! Online gyro bias state (PSSENSE_GYRO_BIAS_AUTO): exponential statistics of the factory-corrected IMU.
struct pssense_gyro_bias
{
	bool enabled;
	bool have_stats;
	double gyro_mean[3], gyro_sq[3], accel_mean[3], accel_sq[3];
	timepoint_ns last_ns;
	timepoint_ns still_since_ns;
	bool still_logged;
	uint32_t updates;
	struct xrt_vec3 bias;
};

struct pssense_device
{
	struct xrt_device base;
	struct xrt_frame_node node;
	struct t_timing_event_sink timing_event_sink;
	struct t_constellation_tracker_led_model led_model;
	//! This controller's own copy of the LEDs when PSSENSE_LED_CORRECTION moves them; led_model then points here.
	struct t_constellation_tracker_led corrected_leds[ARRAY_SIZE(pssense_left_leds)];
	struct t_constellation_tracker_device constellation_device;
	struct t_constellation_tracker_tracking_source constellation_tracking_source;

	bool usb;
	struct os_hid_device *hid;
	/*!
	 * PSSENSE_RECONNECT: the controller thread opens the HID itself when the controller connects (also after a
	 * disconnect) instead of ending. connected is guarded by the controller thread lock.
	 */
	bool reconnect;
	bool connected;
	uint16_t product_id;
	struct os_thread_helper controller_thread;
	//! Battery level last logged (BATTERY), so a low controller shows up in ordinary session logs; -1 none yet.
	float logged_battery_percent;
	bool logged_battery_charging;

	struct
	{
		struct m_clock_windowed_skew_tracker *clock_tracker;
		//! Offset for the LED schedule and IMU timestamps (bootstrap/future-schedule path), see
		//! pssense_clock.h.
		struct pssense_clock clock;
		double filtered_offset_ns;
		bool has_clock_offset;
		//! PSSENSE_TIMING_DIAG: best (largest-offset) sample of the current 100 ms window, logged as
		//! PSSENSE_CLOCK.
		timepoint_ns clock_log_window_ns;
		timepoint_ns clock_log_local_ns;
		timepoint_ns clock_log_remote_ns;
		uint32_t clock_log_samples;

		timepoint_ns latest_imu_time_ns;

		uint32_t imu_ticks_last;
		uint64_t imu_ticks_total;

		timepoint_ns latest_device_time_ns;

		uint32_t device_ticks_last;
		uint64_t device_ticks_total;

		uint32_t last_sent_host_timestamp_us;
	} timing;

	struct
	{
		struct pssense_constellation_candidate_group
		{
			int64_t timestamp_ns;
			bool emitted;
			bool disagreement_recorded;
			bool present[PSSENSE_CONSTELLATION_CAMERA_COUNT];
			struct t_constellation_tracker_sample samples[PSSENSE_CONSTELLATION_CAMERA_COUNT];
		} candidate_groups[PSSENSE_CONSTELLATION_GROUP_COUNT];
		uint32_t next_candidate_group;
		uint64_t candidate_count;
		uint64_t camera_candidate_count[PSSENSE_CONSTELLATION_CAMERA_COUNT];
		uint64_t fused_pose_count;
		uint64_t disagreement_count;
		uint64_t jump_rejection_count;
		int64_t last_optical_timestamp_ns;
		int64_t last_fused_timestamp_ns;
		uint32_t last_fused_camera_count;
		struct xrt_pose last_fused_pose;
		bool have_last_fused_pose;

		/* World-space rotation that aligns the corrected IMU orientation to trusted optical orientation. */
		struct xrt_quat optical_from_imu_orientation;
		bool have_optical_from_imu_orientation;
		int64_t optical_from_imu_timestamp_ns;

		struct m_relation_history *imu_relation_history;
		struct m_imu_3dof fusion;

		//! Online gyro bias (PSSENSE_GYRO_BIAS_AUTO): exponential statistics of the factory-corrected IMU.
		struct pssense_gyro_bias gyro_bias;

		//! PSSENSE_INPUT_DIAG: last value and change count of each watched byte, and when the summary last
		//! printed. PSSENSE_LED_BOOTSTRAP_STRESS_RESCAN_S: when the current lock began (0 when not locked).
		timepoint_ns stress_locked_since_ns;
		uint32_t stress_rescans;

		bool input_diag;
		bool input_diag_have;
		uint8_t input_diag_last[PSSENSE_INPUT_DIAG_BYTES];
		uint32_t input_diag_changes[PSSENSE_INPUT_DIAG_BYTES];
		timepoint_ns input_diag_summary_ns;

		//! PSSENSE_FILTER: IMU + optical EKF; NULL when off. Locked by controller_thread.
		struct t_imu_optical_filter *filter;
		uint64_t filter_last_logged_updates;
		struct xrt_pose pose;

		uint32_t received_frames;
		uint32_t last_exposure_sequence_id;
		timepoint_ns last_exposure_local_timestamp_ns;
		time_duration_ns average_exposure_interval_ns;

		bool increment_sequence_num;
		uint8_t led_sequence_num;
		//! Bumped whenever the LED schedule's content changes (new bootstrap output or sync sample).
		uint32_t led_content_generation;
		//! PSSENSE_LED_LATCH_INTERVAL_MS: the last latched schedule's content generation and host time.
		bool led_latched;
		uint32_t led_latched_content_generation;
		timepoint_ns led_latched_ns;
		//! PSSENSE_LED_BROAD_S: BROAD free-run in progress, since when, and PRESCAN anchors latched before it.
		bool led_broad_active;
		/*!
		 * PSSENSE_LED_BLINK_SWEEP (diagnostic): led_blink values stepped through once the lock is held, the
		 * current value, and generations so each step latches exactly once.
		 */
		uint8_t led_sweep_values[32][4];
		uint32_t led_sweep_count;
		uint32_t led_sweep_index;
		bool led_sweep_started, led_sweep_done;
		timepoint_ns led_sweep_step_ns;
		uint8_t led_blink[4];
		uint32_t led_sweep_generation, led_sweep_latched_generation;
		timepoint_ns led_broad_started_ns;
		uint32_t led_broad_anchors;
		uint32_t led_broad_windows;

		int32_t timing_fudge_100us;

		//! Locked by controller_thread.
		struct pssense_led_settings led_settings;

		bool use_constellation;
		struct t_constellation_tracker *constellation_tracker;
		struct xrt_tracking_origin *tracking_origin_before_constellation;
		t_constellation_device_id_t constellation_device_id;
		struct xrt_imu_sink *constellation_imu_sink;
		struct m_relation_history *constellation_relation_history;

		struct t_led_sync_refinement led_sync_refinement;
		uint8_t period_id;

		bool led_sync_sample_needs_marking;
		bool led_sync_sample_needs_sending;
		struct t_led_sync_sample latest_led_sync_sample;

		/*!
		 * Opt-in brightness-driven LED phase bootstrap (PSSENSE_LED_BOOTSTRAP=1). Replaces the pose-driven
		 * @ref t_led_sync_refinement while enabled. Locked by controller_thread.
		 */
		bool use_led_bootstrap;
		//! Feed the bootstrap LED-shaped per-controller counts rather than raw blob counts.
		bool led_bootstrap_led_blobs;
		//! PSSENSE_LED_BOOTSTRAP_STRICT is set.
		bool led_bootstrap_strict;
		struct t_led_phase_bootstrap led_bootstrap;
		//! Output generation last programmed into the LED settings.
		uint32_t led_bootstrap_programmed_generation;
		//! Held dark (and frozen) because another controller owns the scan.
		bool led_bootstrap_yielding;
		uint32_t led_bootstrap_status_frames;
		//! The LED bootstrap state and phase last written to a recorded dataset, to record only changes.
		uint32_t recorded_led_state;
		int64_t recorded_led_fudge_ns;
		//! Exposures spent waiting for the PSSENSE_LED_BOOTSTRAP_FIRST side to lock before our first scan.
		uint32_t led_bootstrap_first_wait_frames;

		struct xrt_pose T_led_imu;
	} tracking;

	enum xrt_hand hand;

	enum u_logging_level log_level;

	struct os_precise_sleeper sleeper;

	struct pssense_parsed_calibration calibration;
	bool has_calibration;

	//! Input state parsed from most recent packet
	struct pssense_input_state state;
	//! Pending output state to send to device
	struct
	{
		uint8_t next_seq_no;
		uint8_t packet_counter;

		struct u_resampler *pcm_haptics_resampler;

		bool send_vibration;
		uint8_t vibration_amplitude;
		uint8_t vibration_mode;

		uint64_t vibration_end_timestamp_ns;

		bool send_trigger_feedback;

		//! PSSENSE_LEDS_OFF_ON_EXIT: shutting down; send LED_ALL_OFF (latched with exit_led_sequence) until
		//! closed.
		bool exiting;
		uint8_t exit_led_sequence;
		enum pssense_adaptive_trigger_mode trigger_feedback_mode;
	} output;


	//! Compatibility mode: report a valid/tracked synthetic controller position
	//! while retaining real IMU orientation and real inputs.
	bool synthetic_position;
	//! Move the synthetic controller from a head-relative shoulder using the
	//! controller orientation as a simple arm model.
	bool synthetic_arm_model;
	//! Borrowed HMD device used by the synthetic arm model.
	struct xrt_device *head_xdev;
	//! Align the independent Sense IMU yaw frame to the HMD/world frame.
	bool orientation_alignment_initialized;
	struct xrt_quat orientation_alignment;

	//! Optional raw-input edge diagnostics for OpenVR/OpenXR binding debugging.
	bool input_diagnostics;
	bool diagnostic_trigger_initialized;
	bool diagnostic_trigger_click;

	struct
	{
		bool button_states;
		bool timing;
		bool tracking;
	} gui;
};

static struct pssense_device *
from_device(struct xrt_device *xdev)
{
	return container_of(xdev, struct pssense_device, base);
}

static struct pssense_device *
from_node(struct xrt_frame_node *node)
{
	return container_of(node, struct pssense_device, node);
}

static struct pssense_device *
from_timing_event_sink(struct t_timing_event_sink *sink)
{
	return container_of(sink, struct pssense_device, timing_event_sink);
}

static struct pssense_device *
from_constellation_device(struct t_constellation_tracker_device *device)
{
	return container_of(device, struct pssense_device, constellation_device);
}

static struct pssense_device *
from_constellation_tracking_source(struct t_constellation_tracker_tracking_source *tracking_source)
{
	return container_of(tracking_source, struct pssense_device, constellation_tracking_source);
}

const uint32_t CRC_POLYNOMIAL = 0xedb88320;

/*
 *
 * Internal functions
 *
 */

static uint32_t
crc32_le(uint32_t crc, uint8_t const *p, size_t len)
{
	int i;
	crc ^= 0xffffffff;
	while (len--) {
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ ((crc & 1) ? CRC_POLYNOMIAL : 0);
	}
	return crc ^ 0xffffffff;
}

static void
pssense_log_clock_sample_locked(struct pssense_device *pssense, timepoint_ns local_ns, timepoint_ns remote_ns)
{
	const timepoint_ns window_ns = 100 * U_TIME_1MS_IN_NS;
	if (pssense->timing.clock_log_samples > 0 && local_ns - pssense->timing.clock_log_window_ns >= window_ns) {
		const struct pssense_clock *c = &pssense->timing.clock;
		PSSENSE_INFO(pssense,
		             "PSSENSE_CLOCK side=%c local_ns=%" PRId64 " remote_ns=%" PRId64
		             " samples=%u envelope_ns=%.0f offset_ns=%.0f holding=%d rate_ppm=%.2f",
		             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', pssense->timing.clock_log_local_ns,
		             pssense->timing.clock_log_remote_ns, pssense->timing.clock_log_samples, c->envelope_ns,
		             c->offset_ns, c->holding ? 1 : 0, c->have_rate ? c->rate * 1e6 : 0.0);
		pssense->timing.clock_log_samples = 0;
	}
	if (pssense->timing.clock_log_samples == 0) {
		pssense->timing.clock_log_window_ns = local_ns;
	}
	if (pssense->timing.clock_log_samples == 0 ||
	    remote_ns - local_ns > pssense->timing.clock_log_remote_ns - pssense->timing.clock_log_local_ns) {
		pssense->timing.clock_log_local_ns = local_ns;
		pssense->timing.clock_log_remote_ns = remote_ns;
	}
	pssense->timing.clock_log_samples++;
}

static void
pssense_add_clock_offset_sample_locked_experimental(struct pssense_device *pssense,
                                                    timepoint_ns local_ns,
                                                    timepoint_ns remote_ns)
{
	struct pssense_clock *clock = &pssense->timing.clock;
	const bool first = !clock->have_offset;

	/*
	 * PSSENSE_CLOCK_STEADY: hold the mapping once the LED schedule has locked, advancing only at the fitted drift
	 * rate. The lock is measured against this mapping, so later sags and steps of the link's latency floor would
	 * otherwise move the pulse against the exposure (4-5 Oct: snaps of 250-980 us preceded long ring losses).
	 */
	pssense_clock_set_hold(clock, pssense->tracking.led_bootstrap.locks_acquired > 0);
	const bool was_holding = clock->holding;
	pssense_clock_push(clock, local_ns, remote_ns);
	const char side = pssense->hand == XRT_HAND_LEFT ? 'L' : 'R';
	if (clock->holding && !was_holding) {
		PSSENSE_INFO(pssense, "CLOCK_OFFSET side=%c event=hold rate_ppm=%.2f envelope_gap_us=%.1f", side,
		             clock->rate * 1e6, (clock->envelope_ns - clock->offset_ns) / 1000.0);
	}
	if (clock->snapped) {
		/*
		 * The first report can arrive milliseconds late, and at the smoothing rate the offset then creeps for
		 * tens of seconds, sliding every scheduled LED pulse against the camera exposures by the same amount.
		 */
		PSSENSE_INFO(pssense, "CLOCK_OFFSET side=%c event=snap delta_us=%.1f", side,
		             clock->snap_delta_ns / 1000.0);
		const double value[3] = {clock->snap_delta_ns, 0.0, 0.0};
		t_constellation_tracker_record_sync_event(
		    pssense->tracking.constellation_tracker, pssense->tracking.constellation_device_id,
		    (int64_t)os_monotonic_get_ns(), T_CONSTELLATION_SYNC_EVENT_CLOCK_SNAP, value);
	}
	if (debug_get_bool_option_pssense_timing_diag()) {
		pssense_log_clock_sample_locked(pssense, local_ns, remote_ns);
	}

	pssense->timing.filtered_offset_ns = clock->offset_ns;
	pssense->timing.has_clock_offset = true;
	if (!first) {
		t_led_sync_push_host_device_clock_offset(&pssense->tracking.led_sync_refinement,
		                                         (time_duration_ns)(pssense->timing.filtered_offset_ns));
	}
}

static void
pssense_add_clock_offset_sample_locked(struct pssense_device *pssense, timepoint_ns local_ns, timepoint_ns remote_ns)
{
	if (debug_get_bool_option_pssense_led_bootstrap() || debug_get_bool_option_pssense_future_led_schedule()) {
		pssense_add_clock_offset_sample_locked_experimental(pssense, local_ns, remote_ns);
	} else {
		m_clock_windowed_skew_tracker_push(pssense->timing.clock_tracker, local_ns, remote_ns);
		time_duration_ns skew_ns;
		if (m_clock_windowed_skew_tracker_get_skew(pssense->timing.clock_tracker, &skew_ns)) {
			pssense->timing.has_clock_offset = true;
			pssense->timing.filtered_offset_ns = -skew_ns;
			t_led_sync_push_host_device_clock_offset(&pssense->tracking.led_sync_refinement, -skew_ns);
		}
	}
}
static bool
pssense_host_ts_to_device_experimental(struct pssense_device *pssense,
                                       timepoint_ns host_timestamp_ns,
                                       timepoint_ns *out_device_timestamp_ns)
{
	if (!pssense->timing.has_clock_offset) {
		return false;
	}

	switch (pssense->tracking.latest_led_sync_sample.timestamp_mode) {
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_INVALID:
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_DEVICE_HOST_LATENCY: {
		*out_device_timestamp_ns = host_timestamp_ns + (timepoint_ns)(pssense->timing.filtered_offset_ns) +
		                           pssense->tracking.latest_led_sync_sample.timestamp.device_host_latency_ns;
		return true;
	}
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_HOST_DEVICE_CLOCK_OFFSET: {
		*out_device_timestamp_ns =
		    host_timestamp_ns + pssense->tracking.latest_led_sync_sample.timestamp.host_device_clock_offset_ns;
		return true;
	}
	}

	return false;
}

static bool
pssense_host_ts_to_device_upstream(struct pssense_device *pssense,
                                   timepoint_ns host_timestamp_ns,
                                   timepoint_ns *out_device_timestamp_ns)
{
	switch (pssense->tracking.latest_led_sync_sample.timestamp_mode) {
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_INVALID:
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_DEVICE_HOST_LATENCY: {
		if (!m_clock_windowed_skew_tracker_to_remote(pssense->timing.clock_tracker, host_timestamp_ns,
		                                             out_device_timestamp_ns)) {
			return false;
		}
		// The refinement routine only reports the device->host transfer latency in this mode, the driver
		// has to apply it on top of its own clock tracking.
		*out_device_timestamp_ns += pssense->tracking.latest_led_sync_sample.timestamp.device_host_latency_ns;
		return true;
	}
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_HOST_DEVICE_CLOCK_OFFSET: {
		*out_device_timestamp_ns =
		    host_timestamp_ns + pssense->tracking.latest_led_sync_sample.timestamp.host_device_clock_offset_ns;
		return true;
	}
	}

	return false;
}

static bool
pssense_host_ts_to_device(struct pssense_device *pssense,
                          timepoint_ns host_timestamp_ns,
                          timepoint_ns *out_device_timestamp_ns)
{
	if (debug_get_bool_option_pssense_led_bootstrap() || debug_get_bool_option_pssense_future_led_schedule()) {
		return pssense_host_ts_to_device_experimental(pssense, host_timestamp_ns, out_device_timestamp_ns);
	}
	return pssense_host_ts_to_device_upstream(pssense, host_timestamp_ns, out_device_timestamp_ns);
}

static bool
pssense_device_ts_to_host_experimental(struct pssense_device *pssense,
                                       timepoint_ns device_timestamp_ns,
                                       timepoint_ns *out_host_timestamp_ns)
{
	if (!pssense->timing.has_clock_offset) {
		return false;
	}

	switch (pssense->tracking.latest_led_sync_sample.timestamp_mode) {
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_INVALID:
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_DEVICE_HOST_LATENCY: {
		*out_host_timestamp_ns = device_timestamp_ns - (timepoint_ns)(pssense->timing.filtered_offset_ns) -
		                         pssense->tracking.latest_led_sync_sample.timestamp.device_host_latency_ns;
		return true;
	}
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_HOST_DEVICE_CLOCK_OFFSET: {
		*out_host_timestamp_ns = device_timestamp_ns -
		                         pssense->tracking.latest_led_sync_sample.timestamp.host_device_clock_offset_ns;
		return true;
	}
	}

	return false;
}

static bool
pssense_device_ts_to_host_upstream(struct pssense_device *pssense,
                                   timepoint_ns device_timestamp_ns,
                                   timepoint_ns *out_host_timestamp_ns)
{
	switch (pssense->tracking.latest_led_sync_sample.timestamp_mode) {
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_INVALID:
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_DEVICE_HOST_LATENCY: {
		if (!m_clock_windowed_skew_tracker_to_local(pssense->timing.clock_tracker, device_timestamp_ns,
		                                            out_host_timestamp_ns)) {
			return false;
		}
		*out_host_timestamp_ns -= pssense->tracking.latest_led_sync_sample.timestamp.device_host_latency_ns;
		return true;
	}
	case T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_HOST_DEVICE_CLOCK_OFFSET: {
		*out_host_timestamp_ns = device_timestamp_ns -
		                         pssense->tracking.latest_led_sync_sample.timestamp.host_device_clock_offset_ns;
		return true;
	}
	}

	return false;
}

static bool
pssense_device_ts_to_host(struct pssense_device *pssense,
                          timepoint_ns device_timestamp_ns,
                          timepoint_ns *out_host_timestamp_ns)
{
	if (debug_get_bool_option_pssense_led_bootstrap() || debug_get_bool_option_pssense_future_led_schedule()) {
		return pssense_device_ts_to_host_experimental(pssense, device_timestamp_ns, out_host_timestamp_ns);
	}
	return pssense_device_ts_to_host_upstream(pssense, device_timestamp_ns, out_host_timestamp_ns);
}

/*!
 * Reads one packet from the device, wrapping no data as EAGAIN. Does not block.
 */
static int
pssense_read_packet_data(struct pssense_device *pssense,
                         uint8_t *buffer,
                         size_t size,
                         timepoint_ns *out_receive_timestamp_ns)
{
	// Poll, don't block. Outer thread needs to run quick
	int ret = os_hid_read_with_timestamp(pssense->hid, buffer, size, 0, out_receive_timestamp_ns);

	// No data yet
	if (ret == 0) {
		return -EAGAIN;
	}

	if (ret < 0) {
		PSSENSE_ERROR(pssense, "Failed to read device '%i'!", ret);
		return ret;
	}

	return ret;
}

/*!
 * Track exponential mean and variance of the factory-corrected gyro and accelerometer; while they show the controller
 * still for long enough, take the gyro mean as the bias. Stillness is judged from the spread of the readings, not
 * their size, so it works whatever the bias is.
 */
static void
pssense_update_gyro_bias(struct pssense_device *pssense,
                         timepoint_ns now_ns,
                         const struct xrt_vec3 *gyro,
                         const struct xrt_vec3 *accel)
{
	struct pssense_gyro_bias *b = &pssense->tracking.gyro_bias;
	const double g[3] = {gyro->x, gyro->y, gyro->z};
	const double a[3] = {accel->x, accel->y, accel->z};
	if (!b->have_stats || now_ns <= b->last_ns) {
		for (int i = 0; i < 3; i++) {
			b->gyro_mean[i] = g[i];
			b->gyro_sq[i] = g[i] * g[i];
			b->accel_mean[i] = a[i];
			b->accel_sq[i] = a[i] * a[i];
		}
		b->have_stats = true;
		b->last_ns = now_ns;
		b->still_since_ns = 0;
		return;
	}
	double dt = (double)(now_ns - b->last_ns) * 1e-9;
	b->last_ns = now_ns;
	double alpha = dt / (PSSENSE_GYRO_BIAS_TAU_S + dt);
	double gyro_var = 0.0, accel_var = 0.0, accel_len2 = 0.0;
	for (int i = 0; i < 3; i++) {
		b->gyro_mean[i] += alpha * (g[i] - b->gyro_mean[i]);
		b->gyro_sq[i] += alpha * (g[i] * g[i] - b->gyro_sq[i]);
		b->accel_mean[i] += alpha * (a[i] - b->accel_mean[i]);
		b->accel_sq[i] += alpha * (a[i] * a[i] - b->accel_sq[i]);
		gyro_var += fmax(0.0, b->gyro_sq[i] - b->gyro_mean[i] * b->gyro_mean[i]);
		accel_var += fmax(0.0, b->accel_sq[i] - b->accel_mean[i] * b->accel_mean[i]);
		accel_len2 += b->accel_mean[i] * b->accel_mean[i];
	}
	bool still = sqrt(gyro_var) < PSSENSE_GYRO_BIAS_MAX_GYRO_STD &&
	             sqrt(accel_var) < PSSENSE_GYRO_BIAS_MAX_ACCEL_STD &&
	             fabs(sqrt(accel_len2) - MATH_GRAVITY_M_S2) < PSSENSE_GYRO_BIAS_MAX_GRAVITY_ERROR;
	if (!still) {
		b->still_since_ns = 0;
		b->still_logged = false;
		return;
	}
	if (b->still_since_ns == 0) {
		b->still_since_ns = now_ns;
	}
	if (now_ns - b->still_since_ns < PSSENSE_GYRO_BIAS_STILL_NS) {
		return;
	}
	b->bias = (struct xrt_vec3){(float)b->gyro_mean[0], (float)b->gyro_mean[1], (float)b->gyro_mean[2]};
	b->updates++;
	if (!b->still_logged) {
		b->still_logged = true;
		PSSENSE_INFO(
		    pssense, "GYRO_BIAS side=%c event=still bias_deg_s=%.2f,%.2f,%.2f magnitude_deg_s=%.2f updates=%u",
		    pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', b->bias.x * 180.0 / M_PI, b->bias.y * 180.0 / M_PI,
		    b->bias.z * 180.0 / M_PI, m_vec3_len(b->bias) * 180.0 / M_PI, b->updates);
		const double value[3] = {b->bias.x, b->bias.y, b->bias.z};
		t_constellation_tracker_record_sync_event(pssense->tracking.constellation_tracker,
		                                          pssense->tracking.constellation_device_id, now_ns,
		                                          T_CONSTELLATION_SYNC_EVENT_GYRO_BIAS, value);
	}
}

static void
pssense_update_fusion(struct pssense_device *pssense)
{
	// We don't have calibration yet, so we can't do anything
	if (!pssense->has_calibration) {
		return;
	}

	struct xrt_vec3 gyro = {
	    .x = (pssense->state.gyro_raw.x - pssense->calibration.gyro_bias.x) * pssense->calibration.gyro_scale.x,
	    .y = (pssense->state.gyro_raw.y - pssense->calibration.gyro_bias.y) * pssense->calibration.gyro_scale.y,
	    .z = (pssense->state.gyro_raw.z - pssense->calibration.gyro_bias.z) * pssense->calibration.gyro_scale.z,
	};

	struct xrt_vec3 accel = {
	    .x = (pssense->state.accel_raw.x - pssense->calibration.accel_bias.x) * pssense->calibration.accel_scale.x,
	    .y = (pssense->state.accel_raw.y - pssense->calibration.accel_bias.y) * pssense->calibration.accel_scale.y,
	    .z = (pssense->state.accel_raw.z - pssense->calibration.accel_bias.z) * pssense->calibration.accel_scale.z,
	};

	if (pssense->tracking.gyro_bias.enabled) {
		pssense_update_gyro_bias(pssense, pssense->timing.latest_imu_time_ns, &gyro, &accel);
		gyro.x -= pssense->tracking.gyro_bias.bias.x;
		gyro.y -= pssense->tracking.gyro_bias.bias.y;
		gyro.z -= pssense->tracking.gyro_bias.bias.z;
	}

	m_imu_3dof_update(&pssense->tracking.fusion, pssense->timing.latest_imu_time_ns, &accel, &gyro);
	pssense->tracking.pose.orientation = pssense->tracking.fusion.rot;

	struct xrt_space_relation space_relation = {
	    .pose = pssense->tracking.pose,
	    .relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                      XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT,
	    .angular_velocity = gyro,
	};
	m_relation_history_push(pssense->tracking.imu_relation_history, &space_relation,
	                        pssense->timing.latest_imu_time_ns);

	if (pssense->tracking.filter != NULL) {
		// The filter's body frame is the LED model frame the optical poses use: rotate the IMU vectors into it.
		struct xrt_quat led_from_imu;
		math_quat_invert(&pssense->tracking.T_led_imu.orientation, &led_from_imu);
		struct xrt_vec3 gyro_led, accel_led;
		math_quat_rotate_vec3(&led_from_imu, &gyro, &gyro_led);
		math_quat_rotate_vec3(&led_from_imu, &accel, &accel_led);
		timepoint_ns host_ns;
		if (pssense_device_ts_to_host(pssense, pssense->timing.latest_imu_time_ns, &host_ns)) {
			t_imu_optical_filter_push_imu(pssense->tracking.filter, host_ns, &accel_led, &gyro_led);
		}
	}

	if (pssense->tracking.constellation_imu_sink != NULL) {
		struct xrt_imu_sample sample = {
		    .accel_m_s2 =
		        {
		            .x = accel.x,
		            .y = accel.y,
		            .z = accel.z,
		        },
		    .gyro_rad_secs =
		        {
		            .x = gyro.x,
		            .y = gyro.y,
		            .z = gyro.z,
		        },
		};

		if (pssense_device_ts_to_host(pssense,                            //
		                              pssense->timing.latest_imu_time_ns, //
		                              &sample.timestamp_ns)) {            //
			xrt_sink_push_imu(pssense->tracking.constellation_imu_sink, &sample);

			const struct pssense_gyro_bias *b = &pssense->tracking.gyro_bias;
			const double applied_bias[3] = {b->enabled ? b->bias.x : 0.0, b->enabled ? b->bias.y : 0.0,
			                                b->enabled ? b->bias.z : 0.0};
			t_constellation_tracker_record_imu_timing(
			    pssense->tracking.constellation_tracker, pssense->tracking.constellation_device_id,
			    sample.timestamp_ns, (int64_t)pssense->timing.latest_imu_time_ns,
			    (double)((int64_t)pssense->timing.latest_imu_time_ns - sample.timestamp_ns), applied_bias);
		}
	}
}

static int
pssense_handle_packet(struct pssense_device *pssense,
                      timepoint_ns recv_time_ns,
                      const struct pssense_input_report_common *data)
{
	// Final input state
	struct pssense_input_state input = {
	    .timestamp_ns = recv_time_ns,
	};

#if 0 // IMU rate test
	static timepoint_ns last[2] = {0};
	static double rate[2] = {133, 133};
	rate[pssense->hand] =
	    (rate[pssense->hand] * 0.999) + ((1 / time_ns_to_s(recv_time_ns - last[pssense->hand])) * 0.001);
	printf("%d\trate: %lfhz\n", pssense->hand, rate[pssense->hand]);
	last[pssense->hand] = recv_time_ns;
#endif

	uint32_t seq_no = __le32_to_cpu(data->seq_no);
	if (input.seq_no != 0 && seq_no != input.seq_no + 1) {
		PSSENSE_WARN(pssense, "Missed seq no %u. Previous was %u", seq_no, input.seq_no);
	}
	input.seq_no = seq_no;

	// Update input state
	input.ps_click = (data->buttons[1] & 16) != 0;
	input.squeeze_touch = (data->buttons[2] & 8) != 0;
	input.squeeze_proximity = data->squeeze_proximity / 255.0f;
	input.trigger_touch = (data->buttons[1] & 128) != 0;
	input.trigger_value = data->trigger_value / 255.0f;
	input.trigger_proximity = data->trigger_proximity / 255.0f;
	input.thumbstick.x = (data->thumbstick_x - 128) / 128.0f;
	input.thumbstick.y = (data->thumbstick_y - 128) / -128.0f;
	input.thumbstick_touch = (data->buttons[2] & 4) != 0;

	if (pssense->hand == XRT_HAND_LEFT) {
		input.share_click = (data->buttons[1] & 1) != 0;
		input.square_click = (data->buttons[0] & 1) != 0;
		input.square_touch = (data->buttons[2] & 2) != 0;
		input.triangle_click = (data->buttons[0] & 8) != 0;
		input.triangle_touch = (data->buttons[2] & 1) != 0;
		input.squeeze_click = (data->buttons[0] & 16) != 0;
		input.trigger_click = (data->buttons[0] & 64) != 0;
		input.thumbstick_click = (data->buttons[1] & 4) != 0;
	} else if (pssense->hand == XRT_HAND_RIGHT) {
		input.options_click = (data->buttons[1] & 2) != 0;
		input.cross_click = (data->buttons[0] & 2) != 0;
		input.cross_touch = (data->buttons[2] & 2) != 0;
		input.circle_click = (data->buttons[0] & 4) != 0;
		input.circle_touch = (data->buttons[2] & 1) != 0;
		input.squeeze_click = (data->buttons[0] & 32) != 0;
		input.trigger_click = (data->buttons[0] & 128) != 0;
		input.thumbstick_click = (data->buttons[1] & 8) != 0;
	}

	// Update IMU data
	uint32_t imu_ticks = __le32_to_cpu(data->imu_ticks);
	// Wrap-aware signed delta; negative means an out-of-order report. The first
	// sample is always accepted since there is no previous tick to compare with.
	int32_t imu_ticks_delta = (int32_t)(imu_ticks - pssense->timing.imu_ticks_last);
	if (imu_ticks_delta >= 0 || pssense->timing.imu_ticks_total == 0) {
		pssense->timing.imu_ticks_total += (uint32_t)(imu_ticks - pssense->timing.imu_ticks_last);
		pssense->timing.imu_ticks_last = imu_ticks;

		pssense->timing.latest_imu_time_ns = IMU_TICKS_TO_NS(pssense->timing.imu_ticks_total);

		input.gyro_raw.x = (int16_t)__le16_to_cpu(data->gyro[0]);
		input.gyro_raw.y = (int16_t)__le16_to_cpu(data->gyro[1]);
		input.gyro_raw.z = (int16_t)__le16_to_cpu(data->gyro[2]);

		input.accel_raw.x = (int16_t)__le16_to_cpu(data->accel[0]);
		input.accel_raw.y = (int16_t)__le16_to_cpu(data->accel[1]);
		input.accel_raw.z = (int16_t)__le16_to_cpu(data->accel[2]);
	} else {
		PSSENSE_WARN(pssense, "Time went backwards. Check your play area for black holes.");
	}

	uint32_t device_ticks = __le32_to_cpu(data->device_timestamp_ticks);
	int32_t device_ticks_delta = (int32_t)(device_ticks - pssense->timing.device_ticks_last);
	if (device_ticks_delta >= 0 || pssense->timing.device_ticks_total == 0) {
		pssense->timing.device_ticks_total += (uint32_t)(device_ticks - pssense->timing.device_ticks_last);
		pssense->timing.device_ticks_last = device_ticks;

		pssense->timing.latest_device_time_ns = IMU_TICKS_TO_NS(pssense->timing.device_ticks_total);
	} else {
		PSSENSE_WARN(pssense, "Device time went backwards. Check your play area for black holes.");
	}

	// Battery state is upper 4 bits
	uint8_t battery_state = data->battery_state >> 4;

	// Charge values go from 0..10, so add 5% and cap at 100% so we never show 0% charge
	float battery_percent = MIN(1.0f, (data->battery_state & 0xf) * .1f + .05);

	bool battery_state_valid, charging;
	if (battery_state == CHARGE_STATE_DISCHARGING) {
		battery_state_valid = true;
		charging = false;
	} else if (battery_state == CHARGE_STATE_CHARGING) {
		battery_state_valid = true;
		charging = true;
	} else if (battery_state == CHARGE_STATE_FULL) {
		battery_state_valid = true;
		charging = true;
		battery_percent = 1.0f;
	} else if (battery_state == CHARGE_STATE_ABNORMAL_VOLTAGE) {
		battery_state_valid = false;
		PSSENSE_WARN(pssense, "Unable to determine charge state: abnormal voltage");
	} else if (battery_state == CHARGE_STATE_ABNORMAL_TEMP) {
		battery_state_valid = false;
		PSSENSE_WARN(pssense, "Unable to determine charge state: abnormal temp");
	} else if (battery_state == CHARGE_STATE_CHARGING_ERROR) {
		battery_state_valid = false;
		PSSENSE_WARN(pssense, "Unable to determine charge state: charging error");
	} else {
		battery_state_valid = false;
		PSSENSE_WARN(pssense, "Unable to determine charge state: unknown reason");
	}

	input.battery_state_valid = battery_state_valid;
	if (battery_state_valid) {
		if (charging != input.battery_charging || battery_percent != input.battery_charge_percent) {
			PSSENSE_TRACE(pssense, "Battery at %.f%%, %s", battery_percent * 100,
			              charging ? "charging" : "discharging");
		}
		input.battery_charging = charging;
		input.battery_charge_percent = battery_percent;
		if (pssense->logged_battery_percent < 0.0f || charging != pssense->logged_battery_charging ||
		    fabsf(battery_percent - pssense->logged_battery_percent) >= 0.05f) {
			PSSENSE_INFO(pssense, "BATTERY side=%c percent=%.0f charging=%d",
			             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', battery_percent * 100.0f,
			             charging ? 1 : 0);
			pssense->logged_battery_percent = battery_percent;
			pssense->logged_battery_charging = charging;
		}
	}

	os_thread_helper_lock(&pssense->controller_thread);

	// Mark the LED sync refinement sample as applied
	uint32_t latest_host_send_time = __le32_to_cpu(data->host_timestamp);
	if (latest_host_send_time != pssense->timing.last_sent_host_timestamp_us &&
	    pssense->tracking.led_sync_sample_needs_marking) {
		t_led_sync_mark_latest_sample_applied(&pssense->tracking.led_sync_refinement, recv_time_ns);
		pssense->tracking.led_sync_sample_needs_marking = false;
	}

	// Update the clock offset
	pssense_add_clock_offset_sample_locked(pssense, recv_time_ns, pssense->timing.latest_device_time_ns);

	pssense->state = input;
	pssense_update_fusion(pssense);

	os_thread_helper_unlock(&pssense->controller_thread);

	return 0;
}


/*!
 * PSSENSE_INPUT_DIAG: watch the input-report bytes nothing else reads. Bytes that change at most 20 times are logged on
 * each change (flags, states); busier ones (counters) only appear in the 10 s summary of change counts.
 */
static void
pssense_input_diag(struct pssense_device *pssense,
                   timepoint_ns recv_time_ns,
                   const struct pssense_bluetooth_input_report *report)
{
	static const char *const names[PSSENSE_INPUT_DIAG_BYTES] = {
	    "unknown1[0]", "unknown1[1]", "unknown2",    "unknown3[0]",       "unknown3[1]", "unknown3[2]",
	    "unknown3[3]", "unknown3[4]", "unknown3[5]", "unknown3[6]",       "unknown4[0]", "unknown4[1]",
	    "unknown4[2]", "unknown4[3]", "unknown5",    "crc_failure_count", "padding[0]",  "padding[1]",
	    "padding[2]",  "padding[3]",  "padding[4]",  "padding[5]",        "padding[6]",  "bt_header",
	};
	const struct pssense_input_report_common *c = &report->common;
	uint8_t now[PSSENSE_INPUT_DIAG_BYTES] = {
	    c->unknown1[0],     c->unknown1[1],     c->unknown2,        c->unknown3[0],
	    c->unknown3[1],     c->unknown3[2],     c->unknown3[3],     c->unknown3[4],
	    c->unknown3[5],     c->unknown3[6],     c->unknown4[0],     c->unknown4[1],
	    c->unknown4[2],     c->unknown4[3],     report->unknown5,   report->crc_failure_count,
	    report->padding[0], report->padding[1], report->padding[2], report->padding[3],
	    report->padding[4], report->padding[5], report->padding[6], report->bt_header,
	};
	const char side = pssense->hand == XRT_HAND_LEFT ? 'L' : 'R';
	if (!pssense->tracking.input_diag_have) {
		memcpy(pssense->tracking.input_diag_last, now, sizeof(now));
		pssense->tracking.input_diag_have = true;
		pssense->tracking.input_diag_summary_ns = recv_time_ns;
		char buf[PSSENSE_INPUT_DIAG_BYTES * 3 + 1];
		for (int i = 0; i < PSSENSE_INPUT_DIAG_BYTES; i++) {
			snprintf(buf + i * 3, 4, "%02x ", now[i]);
		}
		PSSENSE_INFO(pssense, "INPUT_DIAG side=%c event=initial host_ns=%" PRIi64 " bytes=%s", side,
		             recv_time_ns, buf);
		return;
	}
	for (int i = 0; i < PSSENSE_INPUT_DIAG_BYTES; i++) {
		if (now[i] == pssense->tracking.input_diag_last[i]) {
			continue;
		}
		if (++pssense->tracking.input_diag_changes[i] <= 20) {
			PSSENSE_INFO(pssense, "INPUT_DIAG side=%c event=change host_ns=%" PRIi64 " field=%s %02x->%02x",
			             side, recv_time_ns, names[i], pssense->tracking.input_diag_last[i], now[i]);
		}
		pssense->tracking.input_diag_last[i] = now[i];
	}
	if (recv_time_ns - pssense->tracking.input_diag_summary_ns >= (timepoint_ns)10 * U_TIME_1S_IN_NS) {
		char buf[PSSENSE_INPUT_DIAG_BYTES * 12 + 1] = {0};
		size_t used = 0;
		for (int i = 0; i < PSSENSE_INPUT_DIAG_BYTES && used < sizeof(buf); i++) {
			if (pssense->tracking.input_diag_changes[i] > 0) {
				used += (size_t)snprintf(buf + used, sizeof(buf) - used, "%d:%u ", i,
				                         pssense->tracking.input_diag_changes[i]);
			}
		}
		char values[PSSENSE_INPUT_DIAG_BYTES * 2 + 1];
		for (int i = 0; i < PSSENSE_INPUT_DIAG_BYTES; ++i)
			snprintf(values + i * 2, 3, "%02x", now[i]);
		PSSENSE_INFO(pssense,
		             "INPUT_DIAG side=%c event=summary host_ns=%" PRIi64 " bytes=%s changes_by_byte=%s", side,
		             recv_time_ns, values, used ? buf : "none");
		pssense->tracking.input_diag_summary_ns = recv_time_ns;
	}
}

static int
pssense_handle_read(struct pssense_device *pssense)
{
	int ret;

	// Report data
	uint8_t buf[INPUT_REPORT_BLUETOOTH_LENGTH] = {0};
	timepoint_ns recv_time_ns = 0;
	ret = pssense_read_packet_data(pssense, buf, sizeof(buf), &recv_time_ns);

	// Backends without receive timestamps use the dequeue time as before.
	if (recv_time_ns == 0) {
		recv_time_ns = os_monotonic_get_ns();
	}

	if (ret == -EAGAIN) {
		// No data yet, not an error
		return 0;
	}

	if (ret < 0) {
		PSSENSE_ERROR(pssense, "Error reading from device: %d", ret);
		return ret;
	}

	switch (buf[0]) {
	case INPUT_REPORT_ID_USB: {
		struct pssense_usb_input_report data = {0};
		if (ret != sizeof(data)) {
			PSSENSE_ERROR(pssense, "Unexpected USB input report size %d (expected %zu)", ret, sizeof(data));
			return -EINVAL;
		}

		memcpy(&data, buf, sizeof(data));

		return pssense_handle_packet(pssense, recv_time_ns, &data.common);
	}
	case INPUT_REPORT_ID_BLUETOOTH: {
		struct pssense_bluetooth_input_report data = {0};
		if (ret != sizeof(data)) {
			PSSENSE_ERROR(pssense, "Unexpected Bluetooth input report size %d (expected %zu)", ret,
			              sizeof(data));
			return -EINVAL;
		}

		memcpy(&data, buf, sizeof(data));

		// Verify the CRC of the packet
		uint32_t expected_crc = __le32_to_cpu(data.crc);
		uint32_t crc = crc32_le(0, &INPUT_REPORT_CRC32_SEED, 1);
		crc = crc32_le(crc, (uint8_t *)&data, sizeof(struct pssense_bluetooth_input_report) - 4);
		if (crc != expected_crc) {
			PSSENSE_WARN(pssense, "CRC mismatch; skipping input. Expected %08X but got %08X", expected_crc,
			             crc);
			return -EINVAL;
		}

		if (pssense->tracking.input_diag) {
			pssense_input_diag(pssense, recv_time_ns, &data);
		}

		return pssense_handle_packet(pssense, recv_time_ns, &data.common);
	}
	default: {
		PSSENSE_WARN(pssense, "Unhandled HID report id %u", buf[0]);
	}
	}

	return 0;
}

static void
pssense_set_output_report_settings_locked(struct pssense_device *pssense,
                                          struct pssense_output_settings *settings,
                                          bool do_vibration,
                                          uint64_t now_ns)
{
	if (now_ns >= pssense->output.vibration_end_timestamp_ns) {
		pssense->output.vibration_amplitude = 0;
	}

	if (pssense->output.send_vibration && do_vibration) {
		settings->flag1 |= OUTPUT_SETTINGS_ENABLE_VIBRATION_BITS | pssense->output.vibration_mode;
		settings->vibration_amplitude = pssense->output.vibration_amplitude;
		pssense->output.send_vibration = pssense->output.vibration_amplitude > 0;
	}

	if (pssense->output.send_trigger_feedback) {
		settings->flag1 |= PSSENSE_OUTPUT_SETTINGS_FLAG1_ADAPTIVE_TRIGGER_ENABLE;
		settings->trigger_settings.mode = pssense->output.trigger_feedback_mode;
		pssense->output.send_trigger_feedback = false;
	}

	// Give it some time to settle
	if (pssense->output.exiting) {
		settings->led_settings = pssense->tracking.led_settings;
		settings->led_settings.phase = LED_SYNC_PHASE_LED_ALL_OFF;
		settings->led_settings.sequence_number = pssense->output.exit_led_sequence;
	} else if (pssense->tracking.received_frames > 10) {
		settings->led_settings = pssense->tracking.led_settings;

#if 0
		PSSENSE_DEBUG(pssense, "Full LED settings: cycle length %uns, cycle position %luns, sequence number %u",
		              settings->led_settings.cycle_length / 3,
		              (timepoint_ns)IMU_TICKS_TO_NS(settings->led_settings.cycle_position),
		              pssense->tracking.led_sequence_num);

		PSSENSE_DEBUG_HEX(pssense, (uint8_t *)&settings->led_settings,
		                  sizeof(settings->led_settings));
#endif
	} else {
		settings->led_settings.phase = LED_SYNC_PHASE_LED_ALL_OFF;
	}

	pssense->output.next_seq_no = (pssense->output.next_seq_no + 1) % 16;

	uint32_t host_send_ts = os_monotonic_get_ns() / U_TIME_1US_IN_NS;

	// Set the host timestamp as *close* as possible to us sending the packet
	settings->host_timestamp_send_time_us = __cpu_to_le32(host_send_ts);

	if (pssense->tracking.led_sync_sample_needs_sending) {
		pssense->timing.last_sent_host_timestamp_us = host_send_ts;
		pssense->tracking.led_sync_sample_needs_sending = false;
		pssense->tracking.led_sync_sample_needs_marking = true;
	}
}

static size_t
pssense_prepare_bluetooth_output_report_locked(struct pssense_device *pssense, uint8_t *out_report)
{
	uint64_t timestamp_ns = os_monotonic_get_ns();

	struct pssense_ps5_output_report report = {
	    .report_id = OUTPUT_REPORT_ID_BLUETOOTH,
	    // low bits are always zero, to indicate we are using the PS5 packet format
	    .seq_no_mode = (pssense->output.next_seq_no << 4) | (0x0),
	    .tag = OUTPUT_REPORT_TAG,
	    // Packet counter needs to increment with every packet, or PCM haptics won't work.
	    .counter = pssense->output.packet_counter++,
	};

	float pcm_buf[PCM_HAPTIC_BUF_SIZE] = {0};
	size_t read_pcm_samples = u_resampler_read(pssense->output.pcm_haptics_resampler, pcm_buf, ARRAY_SIZE(pcm_buf));

	if (read_pcm_samples > 0) {
		for (size_t i = 0; i < read_pcm_samples; i++) {
			// Convert from float [-1, 1] to int8 [-128, 127].
			report.haptics[i] = (int8_t)(CLAMP(((pcm_buf[i] + 1.0f) * 0.5f * 255) - 128, -128, 127));
		}
	}

	pssense_set_output_report_settings_locked(pssense, &report.settings, read_pcm_samples == 0, timestamp_ns);

	uint32_t crc = crc32_le(0, &OUTPUT_REPORT_CRC32_SEED, 1);
	crc = crc32_le(crc, (uint8_t *)&report, sizeof(struct pssense_ps5_output_report) - 4);
	report.crc = __cpu_to_le32(crc);

	PSSENSE_TRACE(pssense,
	              "Setting vibration amplitude: %u, mode: %02X, trigger feedback mode: %02X. Next seq no: %u. PCM "
	              "samples: %zu",
	              pssense->output.vibration_amplitude, pssense->output.vibration_mode,
	              pssense->output.trigger_feedback_mode, pssense->output.next_seq_no, read_pcm_samples);
#if 0
	PSSENSE_DEBUG_HEX(pssense, (uint8_t *)&report, sizeof(report));
#endif

	memcpy(out_report, &report, sizeof(report));
	return sizeof(report);
}

static size_t
pssense_prepare_usb_report_locked(struct pssense_device *pssense, uint8_t *out_report)
{
	uint64_t timestamp_ns = os_monotonic_get_ns();

	struct pssense_usb_output_report report = {
	    .seq_no_mode = (pssense->output.next_seq_no << 4) | (0x2),
	};

	pssense_set_output_report_settings_locked(pssense, &report.settings, true, timestamp_ns);

	memcpy(out_report, &report, sizeof(report));
	return sizeof(report);
}

static size_t
pssense_prepare_output_report_locked(struct pssense_device *pssense, uint8_t *out_report)
{
	if (pssense->usb) {
		return pssense_prepare_usb_report_locked(pssense, out_report);
	} else {
		return pssense_prepare_bluetooth_output_report_locked(pssense, out_report);
	}

	assert(!"unreachable");
	return -EINVAL;
}

//! Sets the controller to use the lower 133hz polling rate
static bool
pssense_set_pc_polling_rate(struct pssense_device *pssense)
{
	struct pssense_set_polling_rate_feature_report report = {
	    .report_id = SET_POLLING_RATE_FEATURE_REPORT_ID,
	    .unk = 0x0E,
	    .rate_1 = __cpu_to_le16(0x000C),
	    .rate_2 = __cpu_to_le16(0x0002),
	};

	uint32_t crc = crc32_le(0, &SET_FEATURE_REPORT_CRC32_SEED, 1);
	crc = crc32_le(crc, (uint8_t *)&report, sizeof(struct pssense_set_polling_rate_feature_report) - 4);
	report.crc = __cpu_to_le32(crc);

	int ret = os_hid_set_feature(pssense->hid, (uint8_t *)&report, sizeof report);
	if (ret < 0) {
		PSSENSE_ERROR(pssense, "Failed to set PC polling rate, reason %d", ret);
		return false;
	}

	return true;
}

/* Opt-in evidence at the actual HID boundary; planned schedules alone do not prove a report was sent. */
static void
pssense_log_written_report(
    struct pssense_device *pssense, const uint8_t *report, size_t size, int64_t start_ns, int64_t end_ns, int written)
{
	struct pssense_output_settings settings;
	const size_t offset = pssense->usb ? offsetof(struct pssense_usb_output_report, settings)
	                                   : offsetof(struct pssense_ps5_output_report, settings);
	memcpy(&settings, report + offset, sizeof(settings));
	char bytes[sizeof(struct pssense_ps5_output_report) * 2 + 1];
	for (size_t i = 0; i < size; ++i)
		snprintf(bytes + 2 * i, 3, "%02x", report[i]);
	os_thread_helper_lock(&pssense->controller_thread);
	int64_t input_ns = pssense->state.timestamp_ns;
	int64_t device_est_ns = 0;
	bool have_clock = pssense_host_ts_to_device(pssense, start_ns, &device_est_ns);
	os_thread_helper_unlock(&pssense->controller_thread);
	PSSENSE_INFO(pssense,
	             "PSSENSE_OUTPUT side=%c start_ns=%" PRIi64 " end_ns=%" PRIi64
	             " result=%d expected=%zu input_ns=%" PRIi64 " clock_valid=%d device_est_ns=%" PRIi64
	             " phase=%u led_seq=%u period_id=%u cycle_position=%u cycle_length=%u "
	             "flag1=%02x flag2=%02x status_led=%u bytes=%s",
	             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', start_ns, end_ns, written, size, input_ns, have_clock,
	             device_est_ns, settings.led_settings.phase, settings.led_settings.sequence_number,
	             settings.led_settings.period_id, __le32_to_cpu(settings.led_settings.cycle_position),
	             __le32_to_cpu(settings.led_settings.cycle_length), settings.flag1, settings.flag2,
	             settings.status_led_enable, bytes);
}

static bool
pssense_get_calibration_data(struct pssense_device *pssense);

static void
pssense_set_connected_bit(struct pssense_device *pssense, bool connected);

static void
pssense_led_bootstrap_set_waiting(int32_t bit, bool waiting);

static void
pssense_led_bootstrap_release(struct pssense_device *pssense, int64_t exposure_timestamp_ns);

/*!
 * Forget everything tied to one connection: the device clock restarts when the controller does, so the clock
 * mapping, the tick unwrapping and the histories keyed by device time all start again, and the LED schedule is
 * re-acquired. Called with the controller thread lock held.
 */
static void
pssense_reset_connection_locked(struct pssense_device *pssense)
{
	m_clock_windowed_skew_tracker_destroy(pssense->timing.clock_tracker);
	pssense->timing.clock_tracker = m_clock_windowed_skew_tracker_alloc(2048);
	struct pssense_clock_options clock_options = pssense->timing.clock.options;
	pssense_clock_init(&pssense->timing.clock, &clock_options);
	pssense->timing.filtered_offset_ns = 0.0;
	pssense->timing.has_clock_offset = false;
	pssense->timing.clock_log_window_ns = 0;
	pssense->timing.clock_log_local_ns = 0;
	pssense->timing.clock_log_remote_ns = 0;
	pssense->timing.clock_log_samples = 0;
	pssense->timing.latest_imu_time_ns = 0;
	pssense->timing.imu_ticks_last = 0;
	pssense->timing.imu_ticks_total = 0;
	pssense->timing.latest_device_time_ns = 0;
	pssense->timing.device_ticks_last = 0;
	pssense->timing.device_ticks_total = 0;
	pssense->timing.last_sent_host_timestamp_us = 0;

	m_relation_history_clear(pssense->tracking.imu_relation_history);
	m_relation_history_clear(pssense->tracking.constellation_relation_history);
	m_imu_3dof_close(&pssense->tracking.fusion);
	m_imu_3dof_init(&pssense->tracking.fusion, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);
	if (pssense->tracking.filter != NULL) {
		struct t_imu_optical_filter_params filter_params;
		t_imu_optical_filter_default_params(&filter_params);
		t_imu_optical_filter_destroy(&pssense->tracking.filter);
		pssense->tracking.filter = t_imu_optical_filter_create(&filter_params);
	}
	pssense->tracking.have_last_fused_pose = false;
	pssense->tracking.last_optical_timestamp_ns = 0;
	pssense->orientation_alignment_initialized = false;

	t_led_phase_bootstrap_stop(&pssense->tracking.led_bootstrap);
	pssense_led_bootstrap_release(pssense, 0);
	// Not waiting for a turn while disconnected, or the other controller would keep deferring to us.
	pssense_led_bootstrap_set_waiting(pssense->hand == XRT_HAND_LEFT ? 1 : 2, false);
	pssense->tracking.led_bootstrap_programmed_generation = UINT32_MAX;
	pssense->tracking.led_sync_sample_needs_sending = true;

	memset(&pssense->state, 0, sizeof(pssense->state));
}

//! PSSENSE_RECONNECT: open the controller if it is connected, and start a fresh connection. Thread lock not held.
static bool
pssense_try_connect(struct pssense_device *pssense)
{
#ifdef XRT_OS_OSX
	struct os_hid_device *hid = NULL;
	char product[128] = {0};
	if (os_hid_open_iokit_bluetooth(PSSENSE_VID, pssense->product_id, &hid, product, sizeof(product)) != 0) {
		return false;
	}

	os_thread_helper_lock(&pssense->controller_thread);
	pssense_reset_connection_locked(pssense);
	pssense->hid = hid;
	os_thread_helper_unlock(&pssense->controller_thread);

	if (!pssense_get_calibration_data(pssense)) {
		PSSENSE_ERROR(pssense, "CONNECTION side=%c event=calibration_failed, closing and retrying",
		              pssense->hand == XRT_HAND_LEFT ? 'L' : 'R');
		os_thread_helper_lock(&pssense->controller_thread);
		pssense->hid = NULL;
		os_thread_helper_unlock(&pssense->controller_thread);
		os_hid_destroy(hid);
		return false;
	}
	if (debug_get_bool_option_pssense_pc_polling_rate() && !pssense_set_pc_polling_rate(pssense)) {
		PSSENSE_ERROR(pssense, "PC polling rate requested, but got error when attempting to apply.");
	}

	os_thread_helper_lock(&pssense->controller_thread);
	pssense->connected = true;
	os_thread_helper_unlock(&pssense->controller_thread);
	pssense_set_connected_bit(pssense, true);
	PSSENSE_WARN(pssense, "CONNECTION side=%c event=connected product='%s'",
	             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', product);
	return true;
#else
	(void)pssense;
	return false;
#endif
}

//! PSSENSE_RECONNECT: the controller has gone (read error); close it and wait for it again. Thread lock not held.
static void
pssense_disconnect(struct pssense_device *pssense, int result)
{
	os_thread_helper_lock(&pssense->controller_thread);
	struct os_hid_device *hid = pssense->hid;
	pssense->hid = NULL;
	pssense->connected = false;
	pssense_reset_connection_locked(pssense);
	os_thread_helper_unlock(&pssense->controller_thread);
	pssense_set_connected_bit(pssense, false);
	if (hid != NULL) {
		os_hid_destroy(hid);
	}
	PSSENSE_WARN(pssense, "CONNECTION side=%c event=disconnected result=%d, waiting for it to reconnect",
	             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', result);
}

static void *
pssense_run_thread(void *ptr)
{
	U_TRACE_SET_THREAD_NAME("PS Sense");

	struct pssense_device *pssense = ptr;

#ifdef XRT_OS_LINUX
	u_linux_try_to_set_realtime_priority_on_thread(pssense->log_level, "PS Sense");
#endif

	os_thread_helper_lock(&pssense->controller_thread);

	// 32/3000hz (PCM haptic rate), this will *technically* run slightly fast, but like, that's fine.
	const time_duration_ns pcm_haptics_period_ns = 10666666;

	timepoint_ns next_output_ns = os_monotonic_get_ns();

#ifdef XRT_OS_OSX
	uint32_t output_failures = 0;
	int64_t last_output_error_log_ns = 0;
#endif
	int result = 0;
	while (os_thread_helper_is_running_locked(&pssense->controller_thread) && result >= 0) {
		if (pssense->hid == NULL) {
			// PSSENSE_RECONNECT: wait for the controller to connect.
			os_thread_helper_unlock(&pssense->controller_thread);
			bool attached = pssense_try_connect(pssense);
			if (!attached) {
				os_nanosleep(500 * U_TIME_1MS_IN_NS);
			}
			next_output_ns = os_monotonic_get_ns();
			os_thread_helper_lock(&pssense->controller_thread);
			continue;
		}
		os_thread_helper_unlock(&pssense->controller_thread);

		result = pssense_handle_read(pssense);
		if (result < 0 && pssense->reconnect) {
			pssense_disconnect(pssense, result);
			result = 0;
			os_thread_helper_lock(&pssense->controller_thread);
			continue;
		}

		if (result >= 0) {
			timepoint_ns now = os_monotonic_get_ns();

			if (now >= next_output_ns) {
				uint8_t output_report[sizeof(struct pssense_ps5_output_report)] = {0};
				os_thread_helper_lock(&pssense->controller_thread);
				size_t output_size = pssense_prepare_output_report_locked(pssense, output_report);
				os_thread_helper_unlock(&pssense->controller_thread);

				const bool trace_write = debug_get_bool_option_pssense_timing_diag();
				int64_t write_start_ns = trace_write ? os_monotonic_get_ns() : 0;
				int written = os_hid_write(pssense->hid, output_report, output_size);
				int64_t write_end_ns = trace_write ? os_monotonic_get_ns() : 0;
				if (trace_write)
					pssense_log_written_report(pssense, output_report, output_size, write_start_ns,
					                           write_end_ns, written);
				if (written != (int)output_size) {
#ifdef XRT_OS_OSX
					// IOKit output failures need not mean input has disconnected. Keep draining HID
					// input, retry fresh LED/haptic settings with backoff, and let read errors end
					// the loop on disconnect. A single failed write used to freeze the whole hand.
					output_failures++;
					if (output_failures == 1 || now - last_output_error_log_ns >= U_TIME_1S_IN_NS) {
						PSSENSE_WARN(
						    pssense,
						    "HID_OUTPUT side=%c event=failed result=%d consecutive=%u; "
						    "keeping input alive, retrying in 100 ms",
						    pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', written,
						    output_failures);
						last_output_error_log_ns = now;
					}
#else
					PSSENSE_WARN(pssense, "Failed to send output report side=%c: %d",
					             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', written);
					result = written < 0 ? written : -EIO;
#endif
				}
#ifdef XRT_OS_OSX
				else if (output_failures > 0) {
					PSSENSE_INFO(pssense, "HID_OUTPUT side=%c event=recovered failures=%u",
					             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', output_failures);
					output_failures = 0;
				}
#endif

				timepoint_ns write_done_ns = os_monotonic_get_ns();
				do {
					next_output_ns += pcm_haptics_period_ns;
				} while (next_output_ns <= write_done_ns);
#ifdef XRT_OS_OSX
				if (output_failures > 0)
					next_output_ns = write_done_ns + 100 * U_TIME_1MS_IN_NS;
#endif
			}
		}

		if (result >= 0) {
			// @note we don't break the earlier `now` out into the outer scope accessible from here since
			//       sending may take some non-negligible amount of time.
			timepoint_ns to_next_output = next_output_ns - os_monotonic_get_ns();

			// Only sleep if it's an increment greater than 50us, Linux doesn't like sleeping that short and
			// often oversleeps a bit, timing matters with PCM haptics and LED sync!
			if (to_next_output > (U_TIME_1US_IN_NS * 50L)) {
				// Sleep 1ms, or half the time to the next output report, whichever is smaller. We want
				// to wake up frequently enough to read incoming packets, but not so much that we waste
				// CPU time waking too often.
				os_precise_sleeper_nanosleep(&pssense->sleeper,
				                             MIN(to_next_output / 2, U_TIME_1MS_IN_NS));
			}
		}

		os_thread_helper_lock(&pssense->controller_thread);
	}

	if (result < 0) {
		PSSENSE_ERROR(pssense, "HID_INPUT side=%c event=thread_stopped result=%d",
		              pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', result);
	}
	os_thread_helper_unlock(&pssense->controller_thread);

	return NULL;
}

static void
pssense_get_imu_fusion_pose(struct pssense_device *pssense,
                            int64_t at_timestamp_ns,
                            struct xrt_space_relation *out_relation)
{
	timepoint_ns device_ts;
	if (!pssense_host_ts_to_device(pssense, at_timestamp_ns, &device_ts)) {
		(*out_relation) = (struct xrt_space_relation){0};
		return;
	}

	m_relation_history_get(pssense->tracking.imu_relation_history, device_ts, out_relation);
}

static void
pssense_get_corrected_imu_pose(struct pssense_device *pssense,
                               int64_t at_timestamp_ns,
                               struct xrt_space_relation *out_relation)
{
	pssense_get_imu_fusion_pose(pssense, at_timestamp_ns, out_relation);

	/* Put the IMU orientation in the same LED-model coordinate frame used by optical tracking. */
	struct xrt_relation_chain imu_chain = {0};
	struct xrt_pose imu_correction = XRT_POSE_IDENTITY;
	imu_correction.orientation = pssense->tracking.T_led_imu.orientation;
	m_relation_chain_push_pose(&imu_chain, &imu_correction);
	*m_relation_chain_reserve(&imu_chain) = *out_relation;
	m_relation_chain_resolve(&imu_chain, out_relation);
}

static void
pssense_get_constellation_pose_upstream(struct pssense_device *pssense,
                                        int64_t at_timestamp_ns,
                                        struct xrt_space_relation *out_relation)
{
	timepoint_ns device_ts;
	if (!pssense_host_ts_to_device(pssense, at_timestamp_ns, &device_ts)) {
		(*out_relation) = (struct xrt_space_relation){0};
		return;
	}

	m_relation_history_get(pssense->tracking.constellation_relation_history, device_ts, out_relation);
}

static void
pssense_get_constellation_pose(struct pssense_device *pssense,
                               int64_t at_timestamp_ns,
                               struct xrt_space_relation *out_relation)
{
	if (!pssense->tracking.use_led_bootstrap && pssense->tracking.filter == NULL &&
	    !debug_get_bool_option_pssense_joint() && !debug_get_bool_option_pssense_align_imu_orientation()) {
		pssense_get_constellation_pose_upstream(pssense, at_timestamp_ns, out_relation);
		return;
	}

	timepoint_ns device_ts;
	if (!pssense_host_ts_to_device(pssense, at_timestamp_ns, &device_ts)) {
		(*out_relation) = (struct xrt_space_relation){0};
		return;
	}

	if (pssense->tracking.filter != NULL &&
	    t_imu_optical_filter_get_relation(pssense->tracking.filter, at_timestamp_ns, out_relation)) {
		return;
	}

	struct xrt_space_relation optical = XRT_SPACE_RELATION_ZERO;
	struct xrt_space_relation imu = XRT_SPACE_RELATION_ZERO;
	m_relation_history_get(pssense->tracking.constellation_relation_history, device_ts, &optical);
	pssense_get_corrected_imu_pose(pssense, at_timestamp_ns, &imu);
	*out_relation = optical;
	bool optical_fresh =
	    pssense->tracking.last_optical_timestamp_ns > 0 &&
	    at_timestamp_ns >= pssense->tracking.last_optical_timestamp_ns &&
	    at_timestamp_ns - pssense->tracking.last_optical_timestamp_ns <= PSSENSE_CONSTELLATION_STALE_NS;
	if (!optical_fresh) {
		out_relation->relation_flags &=
		    ~(XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
		      XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);
	}
	/* Optical history supplies translation; the continuously integrated IMU supplies orientation. */
	if ((imu.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0) {
		/* Opt-in: rotate the IMU-world orientation into the optical world that the position is in. */
		if (debug_get_bool_option_pssense_align_imu_orientation() &&
		    pssense->tracking.have_optical_from_imu_orientation) {
			const struct xrt_quat *align = &pssense->tracking.optical_from_imu_orientation;
			struct xrt_quat aligned_orientation;
			struct xrt_vec3 aligned_angular_velocity;
			math_quat_rotate(align, &imu.pose.orientation, &aligned_orientation);
			math_quat_normalize(&aligned_orientation);
			math_quat_rotate_vec3(align, &imu.angular_velocity, &aligned_angular_velocity);
			imu.pose.orientation = aligned_orientation;
			imu.angular_velocity = aligned_angular_velocity;
		}
		out_relation->pose.orientation = imu.pose.orientation;
		out_relation->angular_velocity = imu.angular_velocity;
		out_relation->relation_flags &=
		    ~(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
		      XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
		out_relation->relation_flags |= imu.relation_flags & (XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		                                                      XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
		                                                      XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
	}
}

static void
parse_pssense_calibration(const struct pssense_calibration_data *calibration_data,
                          struct pssense_parsed_calibration *out_parsed)
{
	const float rad_per_sec_ref = M_PI * 3.f;

	int16_t gyro_plus_x = __le16_to_cpu(calibration_data->gyro_plus_x);
	int16_t gyro_minus_x = __le16_to_cpu(calibration_data->gyro_minus_x);
	int16_t gyro_plus_y = __le16_to_cpu(calibration_data->gyro_plus_y);
	int16_t gyro_minus_y = __le16_to_cpu(calibration_data->gyro_minus_y);
	int16_t gyro_plus_z = __le16_to_cpu(calibration_data->gyro_plus_z);
	int16_t gyro_minus_z = __le16_to_cpu(calibration_data->gyro_minus_z);

	int16_t accel_plus_x = __le16_to_cpu(calibration_data->accel_plus_x);
	int16_t accel_minus_x = __le16_to_cpu(calibration_data->accel_minus_x);
	int16_t accel_plus_y = __le16_to_cpu(calibration_data->accel_plus_y);
	int16_t accel_minus_y = __le16_to_cpu(calibration_data->accel_minus_y);
	int16_t accel_plus_z = __le16_to_cpu(calibration_data->accel_plus_z);
	int16_t accel_minus_z = __le16_to_cpu(calibration_data->accel_minus_z);

	struct xrt_vec3_i32 gyro_bias = {
	    .x = (int16_t)__le16_to_cpu(calibration_data->gyro_bias_x),
	    .y = (int16_t)__le16_to_cpu(calibration_data->gyro_bias_y),
	    .z = (int16_t)__le16_to_cpu(calibration_data->gyro_bias_z),
	};

	float gyro_span_x =
	    (fabsf((float)gyro_plus_x - (float)gyro_bias.x) + fabsf((float)gyro_minus_x - (float)gyro_bias.x)) / 2.0f;
	float gyro_span_y =
	    (fabsf((float)gyro_plus_y - (float)gyro_bias.y) + fabsf((float)gyro_minus_y - (float)gyro_bias.y)) / 2.0f;
	float gyro_span_z =
	    (fabsf((float)gyro_plus_z - (float)gyro_bias.z) + fabsf((float)gyro_minus_z - (float)gyro_bias.z)) / 2.0f;

	struct xrt_vec3 gyro_scale = {
	    .x = rad_per_sec_ref / gyro_span_x,
	    .y = rad_per_sec_ref / gyro_span_y,
	    .z = rad_per_sec_ref / gyro_span_z,
	};

	struct xrt_vec3 accel_bias = {
	    .x = (accel_plus_x + accel_minus_x) / 2.0f,
	    .y = (accel_plus_y + accel_minus_y) / 2.0f,
	    .z = (accel_plus_z + accel_minus_z) / 2.0f,
	};

	struct xrt_vec3 accel_scale = {
	    .x = (1.0f / (float)(accel_plus_x - accel_bias.x)) * MATH_GRAVITY_M_S2,
	    .y = (1.0f / (float)(accel_plus_y - accel_bias.y)) * MATH_GRAVITY_M_S2,
	    .z = (1.0f / (float)(accel_plus_z - accel_bias.z)) * MATH_GRAVITY_M_S2,
	};

	(*out_parsed) = XRT_C11_COMPOUND(struct pssense_parsed_calibration){
	    .gyro_bias = gyro_bias,
	    .gyro_scale = gyro_scale,
	    .accel_scale = accel_scale,
	};
}

/*!
 * Retrieving the calibration data report will switch the Sense controller from compat mode into full mode.
 */
static bool
pssense_get_calibration_data(struct pssense_device *pssense)
{
	struct pssense_calibration_data calibration_data = {0};

	// A read occasionally returns something other than a calibration part (seen on macOS: part ID 49, i.e. the 0x31
	// input report ID), which used to fail device creation outright. Retry it like a bad CRC, a bounded number of
	// times.
	const int max_attempts = 5;
	int attempt = 0;
	bool invalid_crc;
	do {
		invalid_crc = false;
		if (++attempt > max_attempts) {
			PSSENSE_ERROR(pssense, "Giving up on calibration data after %d attempts", max_attempts);
			return false;
		}

		// Calibration has to be read in two parts with two feature reads.
		for (int i = 0; i < 2; i++) {
			struct pssense_feature_report report_buffer = {.report_id = CALIBRATION_DATA_FEATURE_REPORT_ID};
			int ret = os_hid_get_feature(pssense->hid, report_buffer.report_id, (uint8_t *)&report_buffer,
			                             sizeof(report_buffer));

			if (ret < 0) {
				PSSENSE_ERROR(pssense, "Failed to retrieve calibration report: %d", ret);
				return false;
			}

			if (ret != sizeof(report_buffer)) {
				PSSENSE_ERROR(pssense, "Invalid byte count transferred, expected %zu got %d",
				              sizeof(report_buffer), ret);
				return false;
			}

			switch (report_buffer.part_id) {
			case CALIBRATION_DATA_PART_ID_1: {
				memcpy((uint8_t *)&calibration_data, report_buffer.data, sizeof(report_buffer.data));
				break;
			}
			case CALIBRATION_DATA_PART_ID_2: {
				memcpy(((uint8_t *)&calibration_data) + sizeof(report_buffer.data), report_buffer.data,
				       sizeof(report_buffer.data));
				break;
			}
			default: {
				PSSENSE_WARN(pssense, "Unknown calibration data part ID %u (attempt %d), retrying",
				             report_buffer.part_id, attempt);
				invalid_crc = true;
				continue;
			}
			}

			uint32_t crc = crc32_le(0, &FEATURE_REPORT_CRC32_SEED, 1);
			crc = crc32_le(crc, (uint8_t *)&report_buffer, sizeof(report_buffer) - 4);
			uint32_t expected_crc = __le32_to_cpu(report_buffer.crc);

			// Only check CRC on Bluetooth, CRC is zeroed out on USB.
			if (crc != expected_crc && !pssense->usb) {
				PSSENSE_WARN(pssense, "Invalid feature report CRC. Expected 0x%08X, actual 0x%08X",
				             expected_crc, crc);
				invalid_crc = true;
			}
		}
	} while (invalid_crc);

	parse_pssense_calibration(&calibration_data, &pssense->calibration);
	pssense->has_calibration = true;

	PSSENSE_DEBUG(pssense, "Calibration data retrieved and parsed successfully");

	return true;
}

static uint64_t
saturating_add_uint64(uint64_t a, uint64_t b)
{
	if (UINT64_MAX - a < b) {
		return UINT64_MAX;
	} else {
		return a + b;
	}
}

/*
 *
 * Frame node implementations
 *
 */

static void
pssense_node_break_apart(struct xrt_frame_node *node)
{
	struct pssense_device *pssense = from_node(node);

	// Make sure the USB thread is stopped
	os_thread_helper_stop_and_wait(&pssense->controller_thread);
}

/*!
 * Only one controller may scan at a time: blob counts cannot tell the controllers apart, so every other
 * controller holds its LEDs off while a scan runs. 0 = free, otherwise 1 + hand.
 */
static xrt_atomic_s32_t pssense_led_bootstrap_owner = 0;
//! Set once the side named by PSSENSE_LED_BOOTSTRAP_FIRST has locked.
static xrt_atomic_s32_t pssense_led_bootstrap_first_locked = 0;
/*!
 * Fair turns at the scan token: the controller that last held it, and the controllers waiting for it (bit 0 left,
 * bit 1 right). With quick retries both unlocked controllers become ready together, and on 5 Oct the left took every
 * turn for 110 s while the right never scanned again.
 */
static xrt_atomic_s32_t pssense_led_bootstrap_last_owner = 0;
static xrt_atomic_s32_t pssense_led_bootstrap_waiting = 0;
//! Connected controllers (PSSENSE_RECONNECT): bit 0 left, bit 1 right. A side waits for the first only if connected.
static xrt_atomic_s32_t pssense_connected_mask = 0;
//! With quick lock: set once the side named by PSSENSE_LED_BOOTSTRAP_FIRST has failed a scan (its ring not in view).
static xrt_atomic_s32_t pssense_led_bootstrap_first_failed = 0;

static void
pssense_led_bootstrap_set_waiting(int32_t bit, bool waiting)
{
	int32_t mask;
	do {
		mask = xrt_atomic_s32_load(&pssense_led_bootstrap_waiting);
		if (((mask & bit) != 0) == waiting) {
			return;
		}
	} while (xrt_atomic_s32_cmpxchg(&pssense_led_bootstrap_waiting, mask, waiting ? (mask | bit) : (mask & ~bit)) !=
	         mask);
}

static void
pssense_set_connected_bit(struct pssense_device *pssense, bool connected)
{
	int32_t bit = pssense->hand == XRT_HAND_LEFT ? 1 : 2;
	int32_t mask;
	do {
		mask = xrt_atomic_s32_load(&pssense_connected_mask);
	} while (xrt_atomic_s32_cmpxchg(&pssense_connected_mask, mask, connected ? (mask | bit) : (mask & ~bit)) !=
	         mask);
}
//! Exposure time (ms, wrapping) at which a controller last released the scan token; 0 before any release.
static xrt_atomic_s32_t pssense_led_bootstrap_release_ms = 0;
/*!
 * PSSENSE_LED_BOOTSTRAP_QUICK_LOCK: the hint (µs, a narrow-pulse start) from the most recent lock of any controller,
 * or -1. Both controllers' windows sit within ~1 ms of each other, so the second tries the first's lock before
 * scanning.
 */
static xrt_atomic_s32_t pssense_led_bootstrap_shared_hint_us = -1;
//! Token of the controller that last released the scan token, and whether it has been tracked since.
static xrt_atomic_s32_t pssense_led_bootstrap_release_token = 0;
static xrt_atomic_s32_t pssense_led_bootstrap_released_tracked = 0;

/*!
 * With LED-blob counts, a controller that has just locked stays lit (keep-lock), but the joint tracker needs a moment
 * to bootstrap and confirm its ring before its blobs are claimed. Until then they count as the next scanner's
 * background: on 25 Sep (212653) the right locked at 13.5 s, was tracked from ~15 s, and the left's baseline in
 * between took 3-5 of its blobs per camera on cameras 2 and 3, which then never reached the lit threshold.
 */
#define PSSENSE_LED_BOOTSTRAP_HANDOFF_MS 1500

//! Waiting longer than this (~20 s) for the preferred side gives up, in case it never connects.
#define PSSENSE_LED_BOOTSTRAP_FIRST_WAIT_FRAMES 1200

/*!
 * Opt-in (PSSENSE_LED_BOOTSTRAP_FIRST=L or R): only the named side may start the first scan. The right Sense has
 * repeatedly fallen into an always-lit, status-LED-off state while scanning second; scanning it first separates a
 * role effect from a device one.
 */
static bool
pssense_led_bootstrap_may_start_first_scan(struct pssense_device *pssense)
{
	const char *first = debug_get_option_pssense_led_bootstrap_first();
	if (first == NULL || (first[0] != 'L' && first[0] != 'R' && first[0] != 'l' && first[0] != 'r')) {
		return true;
	}
	char mine = pssense->hand == XRT_HAND_LEFT ? 'L' : 'R';
	char wanted = (first[0] == 'l' || first[0] == 'L') ? 'L' : 'R';
	if (mine == wanted || xrt_atomic_s32_cmpxchg(&pssense_led_bootstrap_first_locked, 1, 1) == 1 ||
	    pssense->tracking.led_bootstrap.locks_acquired > 0) {
		return true;
	}
	// Nor behind one that is not connected (PSSENSE_RECONNECT).
	int32_t wanted_bit = wanted == 'L' ? 1 : 2;
	if (pssense->reconnect && (xrt_atomic_s32_load(&pssense_connected_mask) & wanted_bit) == 0) {
		return true;
	}
	// Do not wait behind a controller that cannot be seen: on 5 Oct the left waited 45 s while the right was out
	// of view.
	if (pssense->tracking.led_bootstrap.options.quick_lock &&
	    xrt_atomic_s32_load(&pssense_led_bootstrap_first_failed) != 0) {
		return true;
	}
	if (++pssense->tracking.led_bootstrap_first_wait_frames > PSSENSE_LED_BOOTSTRAP_FIRST_WAIT_FRAMES) {
		if (pssense->tracking.led_bootstrap_first_wait_frames == PSSENSE_LED_BOOTSTRAP_FIRST_WAIT_FRAMES + 1) {
			PSSENSE_WARN(pssense, "LED_BOOTSTRAP side=%c event=first_wait_timeout waiting_for=%c", mine,
			             wanted);
		}
		return true;
	}
	return false;
}

static int32_t
pssense_led_bootstrap_token(struct pssense_device *pssense)
{
	return pssense->hand == XRT_HAND_LEFT ? 1 : 2;
}

static void
pssense_led_bootstrap_release(struct pssense_device *pssense, int64_t exposure_timestamp_ns)
{
	if (xrt_atomic_s32_cmpxchg(&pssense_led_bootstrap_owner, pssense_led_bootstrap_token(pssense), 0) ==
	        pssense_led_bootstrap_token(pssense) &&
	    exposure_timestamp_ns > 0) {
		int32_t ms = (int32_t)(exposure_timestamp_ns / U_TIME_1MS_IN_NS);
		xrt_atomic_s32_store(&pssense_led_bootstrap_released_tracked, 0);
		xrt_atomic_s32_store(&pssense_led_bootstrap_release_token, pssense_led_bootstrap_token(pssense));
		xrt_atomic_s32_store(&pssense_led_bootstrap_release_ms, ms == 0 ? 1 : ms);
	}
}

//! Whether enough time has passed since another controller released the scan token (LED-blob counts only).
static bool
pssense_led_bootstrap_handoff_settled(struct pssense_device *pssense, int64_t exposure_timestamp_ns)
{
	if (!pssense->tracking.led_bootstrap_led_blobs) {
		return true;
	}
	int32_t released = xrt_atomic_s32_load(&pssense_led_bootstrap_release_ms);
	if (released == 0) {
		return true;
	}
	int32_t now_ms = (int32_t)(exposure_timestamp_ns / U_TIME_1MS_IN_NS);
	int32_t elapsed = (int32_t)((uint32_t)now_ms - (uint32_t)released);
	/*
	 * With quick lock: the wait exists until the joint tracker claims the released controller's ring, so end it
	 * once that controller's optical pose is accepted rather than after the fixed time.
	 */
	if (debug_get_bool_option_pssense_led_bootstrap_quick_lock() &&
	    xrt_atomic_s32_load(&pssense_led_bootstrap_released_tracked) != 0) {
		return true;
	}
	return elapsed < 0 || elapsed >= PSSENSE_LED_BOOTSTRAP_HANDOFF_MS;
}

static void
pssense_node_destroy(struct xrt_frame_node *node)
{
	struct pssense_device *pssense = from_node(node);


	// Destroy the controller thread
	os_thread_helper_destroy(&pssense->controller_thread);

	// Deinit the precise sleeper
	os_precise_sleeper_deinit(&pssense->sleeper);

	if (pssense->output.pcm_haptics_resampler) {
		u_resampler_destroy(pssense->output.pcm_haptics_resampler);
		pssense->output.pcm_haptics_resampler = NULL;
	}

	m_imu_3dof_close(&pssense->tracking.fusion);
	m_clock_windowed_skew_tracker_destroy(pssense->timing.clock_tracker);

	if (pssense->hid != NULL) {
		os_hid_destroy(pssense->hid);
		pssense->hid = NULL;
	}

	// Relation histories are used from the frame context lifecycle in the constellation tracker device callbacks.
	m_relation_history_destroy(&pssense->tracking.imu_relation_history);
	m_relation_history_destroy(&pssense->tracking.constellation_relation_history);
	t_imu_optical_filter_destroy(&pssense->tracking.filter);

	// LED sync is used on the frame context lifecycle, so it needs to be destroyed in here.
	t_led_sync_refinement_destroy(&pssense->tracking.led_sync_refinement);
	pssense_led_bootstrap_release(pssense, 0);

	// Remove the variable tracking.
	u_var_remove_root(pssense);

	// Actually free the pointer
	free(pssense);
}

/*
 *
 * Timing event sink implementation
 *
 */

/*!
 * Advance the LED bootstrap for one exposure. Must be called with controller_thread locked.
 *
 * @return true if this controller's LEDs should be lit.
 */
/*!
 * With strict mode and the online gyro bias, probe only while the controller turns slower than this. A probe compares
 * three consecutive ~0.2 s stages; a moving, turning or covered ring changes its light between them. On 26 Sep (003433)
 * probes taken in normal movement read patterns like 1.6/5.3/4.9 blobs (dark in the middle) and walked the left's lock
 * 1 ms off its window.
 */
#define PSSENSE_LED_PROBE_MAX_ROTATION_RAD_S 0.35

static bool
pssense_led_bootstrap_steady_for_probe(struct pssense_device *pssense)
{
	struct pssense_gyro_bias *g = &pssense->tracking.gyro_bias;
	if (!pssense->tracking.led_bootstrap_strict || !g->enabled || !g->have_stats) {
		return true;
	}
	double x = g->gyro_mean[0] - g->bias.x, y = g->gyro_mean[1] - g->bias.y, z = g->gyro_mean[2] - g->bias.z;
	return sqrt(x * x + y * y + z * z) < PSSENSE_LED_PROBE_MAX_ROTATION_RAD_S;
}

//! When recording a dataset: note LED scans, locks, losses and phase moves as they happen.
static void
pssense_led_bootstrap_record_changes(struct pssense_device *pssense, int64_t exposure_timestamp_ns)
{
	const struct t_led_phase_bootstrap *b = &pssense->tracking.led_bootstrap;
	struct t_constellation_tracker *tracker = pssense->tracking.constellation_tracker;
	t_constellation_device_id_t id = pssense->tracking.constellation_device_id;
	uint32_t state = (uint32_t)b->state;
	uint32_t previous = pssense->tracking.recorded_led_state;
	bool scanning = state == T_LED_PHASE_BOOTSTRAP_WIDE_SCAN || state == T_LED_PHASE_BOOTSTRAP_NARROW_SCAN;
	bool was_scanning =
	    previous == T_LED_PHASE_BOOTSTRAP_WIDE_SCAN || previous == T_LED_PHASE_BOOTSTRAP_NARROW_SCAN;

	if (scanning && !was_scanning) {
		t_constellation_tracker_record_sync_event(tracker, id, exposure_timestamp_ns,
		                                          T_CONSTELLATION_SYNC_EVENT_LED_SCAN, NULL);
	}
	if (state == T_LED_PHASE_BOOTSTRAP_LOCKED && previous != T_LED_PHASE_BOOTSTRAP_LOCKED) {
		const double value[3] = {(double)b->fudge_offset_ns / 1000.0, (double)b->blink_ns / 1000.0, 0.0};
		t_constellation_tracker_record_sync_event(tracker, id, exposure_timestamp_ns,
		                                          T_CONSTELLATION_SYNC_EVENT_LED_LOCK, value);
	} else if (state != T_LED_PHASE_BOOTSTRAP_LOCKED && previous == T_LED_PHASE_BOOTSTRAP_LOCKED) {
		t_constellation_tracker_record_sync_event(tracker, id, exposure_timestamp_ns,
		                                          T_CONSTELLATION_SYNC_EVENT_LED_LOST, NULL);
	} else if (state == T_LED_PHASE_BOOTSTRAP_LOCKED &&
	           b->fudge_offset_ns != pssense->tracking.recorded_led_fudge_ns) {
		const double value[3] = {
		    (double)(b->fudge_offset_ns - pssense->tracking.recorded_led_fudge_ns) / 1000.0, 0.0, 0.0};
		t_constellation_tracker_record_sync_event(tracker, id, exposure_timestamp_ns,
		                                          T_CONSTELLATION_SYNC_EVENT_LED_PHASE_MOVE, value);
	}
	pssense->tracking.recorded_led_state = state;
	pssense->tracking.recorded_led_fudge_ns = b->fudge_offset_ns;
}

static void
pssense_led_sweep_parse(struct pssense_device *pssense)
{
	memset(pssense->tracking.led_blink, 0xff, sizeof(pssense->tracking.led_blink));
	const char *list = debug_get_option_pssense_led_blink_sweep();
	const char *p = list;
	while (p != NULL && *p != '\0' &&
	       pssense->tracking.led_sweep_count < ARRAY_SIZE(pssense->tracking.led_sweep_values)) {
		char token[16] = {0};
		size_t n = strcspn(p, ",");
		if (n > 0 && n < sizeof(token)) {
			memcpy(token, p, n);
			// Report byte order as in Sony's notation: "0affffff" is led_blink[0] = 0x0a, then three 0xff.
			uint8_t *v = pssense->tracking.led_sweep_values[pssense->tracking.led_sweep_count];
			unsigned int b[4] = {0xff, 0xff, 0xff, 0xff};
			int got = n == 2 ? sscanf(token, "%2x", &b[0])
			                 : sscanf(token, "%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3]);
			if ((n == 2 && got == 1) || (n == 8 && got == 4)) {
				for (int i = 0; i < 4; i++) {
					v[i] = (uint8_t)b[i];
				}
				pssense->tracking.led_sweep_count++;
			} else {
				PSSENSE_WARN(pssense, "PSSENSE_LED_BLINK_SWEEP: ignoring '%s' (want 2 or 8 hex digits)",
				             token);
			}
		}
		p += n;
		if (*p == ',') {
			p++;
		}
	}
}

static bool
pssense_led_sweep_active(const struct pssense_device *pssense)
{
	return pssense->tracking.led_sweep_count > 0 && !pssense->tracking.led_sweep_done;
}

//! Steps the led_blink sweep once the LED bootstrap holds its lock. Called with the controller thread locked.
static void
pssense_led_sweep_update_locked(struct pssense_device *pssense, timepoint_ns now_ns, bool locked)
{
	if (!pssense_led_sweep_active(pssense) || !locked) {
		return;
	}
	const timepoint_ns hold_ns =
	    (timepoint_ns)MAX(debug_get_num_option_pssense_led_blink_sweep_s(), 1) * U_TIME_1S_IN_NS;
	if (!pssense->tracking.led_sweep_started) {
		pssense->tracking.led_sweep_started = true;
		pssense->tracking.led_sweep_index = 0;
	} else if (now_ns - pssense->tracking.led_sweep_step_ns >= hold_ns) {
		pssense->tracking.led_sweep_index++;
	} else {
		return;
	}
	pssense->tracking.led_sweep_step_ns = now_ns;
	const char side = pssense->hand == XRT_HAND_LEFT ? 'L' : 'R';
	if (pssense->tracking.led_sweep_index >= pssense->tracking.led_sweep_count) {
		pssense->tracking.led_sweep_done = true;
		memset(pssense->tracking.led_blink, 0xff, sizeof(pssense->tracking.led_blink));
		PSSENSE_INFO(pssense, "LED_BLINK_SWEEP side=%c event=done exposure_ns=%" PRIi64, side,
		             pssense->tracking.last_exposure_local_timestamp_ns);
	} else {
		memcpy(pssense->tracking.led_blink,
		       pssense->tracking.led_sweep_values[pssense->tracking.led_sweep_index],
		       sizeof(pssense->tracking.led_blink));
		const uint8_t *v = pssense->tracking.led_blink;
		PSSENSE_INFO(
		    pssense,
		    "LED_BLINK_SWEEP side=%c event=step step=%u/%u value=%02x%02x%02x%02x broad=%d exposure_ns=%" PRIi64
		    " host_ns=%" PRIi64,
		    side, pssense->tracking.led_sweep_index + 1, pssense->tracking.led_sweep_count, v[0], v[1], v[2],
		    v[3], pssense->tracking.led_broad_active ? 1 : 0,
		    pssense->tracking.last_exposure_local_timestamp_ns, now_ns);
	}
	pssense->tracking.led_sweep_generation++;
}

static bool
pssense_led_bootstrap_update_locked(struct pssense_device *pssense, int64_t exposure_timestamp_ns)
{
	struct t_led_phase_bootstrap *b = &pssense->tracking.led_bootstrap;
	const int32_t me = pssense_led_bootstrap_token(pssense);
	int32_t owner = xrt_atomic_s32_cmpxchg(&pssense_led_bootstrap_owner, 0, 0);

	if (owner != 0 && owner != me) {
		// Another controller is scanning. Do not advance, so our own lock is not declared lost meanwhile.
		pssense->tracking.led_bootstrap_yielding = true;
		if (b->options.quick_lock && b->state == T_LED_PHASE_BOOTSTRAP_IDLE && b->locks_acquired == 0) {
			// Unlocked and kept from retrying: our turn comes next.
			pssense_led_bootstrap_set_waiting(pssense->hand == XRT_HAND_LEFT ? 1 : 2, true);
		}
		if (debug_get_bool_option_pssense_led_bootstrap_keep_lock() &&
		    b->state == T_LED_PHASE_BOOTSTRAP_LOCKED) {
			/*
			 * Opt-in: keep a locked controller lit. Its steady light becomes part of the scanning
			 * controller's dark baseline, which then scans cleanly (24 Sep, 230720), and it keeps
			 * tracking. Yielding means switching a lit controller to LED_ALL_OFF and back, and in the
			 * two-controller runs one controller repeatedly fell into an always-lit, status-LED-off state.
			 */
			return true;
		}
		return false;
	}
	pssense->tracking.led_bootstrap_yielding = false;

	const int32_t my_bit = pssense->hand == XRT_HAND_LEFT ? 1 : 2;
	const int32_t other_bit = 3 - my_bit;
	// Let a waiting controller go before us if we had the last turn.
	bool defer = b->options.quick_lock && xrt_atomic_s32_load(&pssense_led_bootstrap_last_owner) == me &&
	             (xrt_atomic_s32_load(&pssense_led_bootstrap_waiting) & other_bit) != 0;
	if (owner == 0 && t_led_phase_bootstrap_ready_to_scan(b) &&
	    pssense_led_bootstrap_may_start_first_scan(pssense) &&
	    pssense_led_bootstrap_handoff_settled(pssense, exposure_timestamp_ns) && !defer) {
		if (xrt_atomic_s32_cmpxchg(&pssense_led_bootstrap_owner, 0, me) == 0) {
			pssense_led_bootstrap_set_waiting(my_bit, false);
			xrt_atomic_s32_store(&pssense_led_bootstrap_last_owner, me);
			owner = me;
			int32_t shared_hint_us = xrt_atomic_s32_load(&pssense_led_bootstrap_shared_hint_us);
			if (b->options.quick_lock && b->locks_acquired == 0 && shared_hint_us >= 0) {
				// Not locked yet: try the other controller's lock first.
				b->next_hint_ns = (time_duration_ns)shared_hint_us * U_TIME_1US_IN_NS;
			}
			t_led_phase_bootstrap_start(b, pssense->tracking.average_exposure_interval_ns);
		}
	}
	if (b->options.quick_lock && t_led_phase_bootstrap_ready_to_scan(b) && owner != me) {
		pssense_led_bootstrap_set_waiting(my_bit, true);
	}

	long stress_s = debug_get_num_option_pssense_led_bootstrap_stress_rescan_s();
	if (stress_s > 0) {
		if (b->state != T_LED_PHASE_BOOTSTRAP_LOCKED) {
			pssense->tracking.stress_locked_since_ns = 0;
		} else if (pssense->tracking.stress_locked_since_ns == 0) {
			pssense->tracking.stress_locked_since_ns = exposure_timestamp_ns;
		} else if (exposure_timestamp_ns - pssense->tracking.stress_locked_since_ns >=
		               (timepoint_ns)stress_s * U_TIME_1S_IN_NS &&
		           owner == 0 && !t_led_phase_bootstrap_is_probing(b) &&
		           xrt_atomic_s32_cmpxchg(&pssense_led_bootstrap_owner, 0, me) == 0) {
			owner = me;
			pssense->tracking.stress_locked_since_ns = 0;
			pssense->tracking.stress_rescans++;
			PSSENSE_INFO(pssense, "LED_BOOTSTRAP side=%c event=stress_rescan count=%u",
			             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', pssense->tracking.stress_rescans);
			// This opt-in experiment must exercise the wide/narrow transition, not reuse the last hint.
			b->next_hint_ns = -1;
			t_led_phase_bootstrap_start(b, pssense->tracking.average_exposure_interval_ns);
		}
	}

	uint32_t locks_before = b->locks_acquired;
	(void)t_led_phase_bootstrap_push_exposure(b, exposure_timestamp_ns);
	if (b->options.quick_lock && b->locks_acquired != locks_before && b->next_hint_ns >= 0) {
		xrt_atomic_s32_store(
		    &pssense_led_bootstrap_shared_hint_us,
		    (int32_t)(t_led_phase_bootstrap_wrap(b->next_hint_ns, b->period_ns) / U_TIME_1US_IN_NS));
	}

	if (b->options.quick_lock && b->locks_acquired == 0 && b->hint_failures > 0) {
		const char *first = debug_get_option_pssense_led_bootstrap_first();
		char mine = pssense->hand == XRT_HAND_LEFT ? 'L' : 'R';
		if (first != NULL && (first[0] == mine || first[0] == mine + ('a' - 'A'))) {
			xrt_atomic_s32_store(&pssense_led_bootstrap_first_failed, 1);
		}
	}
	if (b->locks_acquired > 0 || t_led_phase_bootstrap_is_stuck_lit(b)) {
		const char *first = debug_get_option_pssense_led_bootstrap_first();
		char mine = pssense->hand == XRT_HAND_LEFT ? 'L' : 'R';
		if (first != NULL && (first[0] == mine || first[0] == mine + ('a' - 'A'))) {
			xrt_atomic_s32_cmpxchg(&pssense_led_bootstrap_first_locked, 0, 1);
		}
	}

	// A tracking probe changes this controller's light, so like a scan it needs the LEDs to itself.
	// Not during a BROAD free-run: the probe's offsets need PRESCAN anchors.
	if (t_led_phase_bootstrap_wants_probe(b) && pssense_led_bootstrap_steady_for_probe(pssense) &&
	    !pssense->tracking.led_broad_active && !pssense_led_sweep_active(pssense) &&
	    (owner == me || (owner == 0 && xrt_atomic_s32_cmpxchg(&pssense_led_bootstrap_owner, 0, me) == 0))) {
		owner = me;
		t_led_phase_bootstrap_begin_probe(b);
	}

	if (t_led_phase_bootstrap_is_scanning(b) && owner != me) {
		// A locked controller lost its LEDs and wants to rescan; it needs the token first.
		if (xrt_atomic_s32_cmpxchg(&pssense_led_bootstrap_owner, 0, me) != 0) {
			t_led_phase_bootstrap_stop(b);
		}
	} else if (!t_led_phase_bootstrap_is_scanning(b) && !t_led_phase_bootstrap_is_probing(b) && owner == me) {
		pssense_led_bootstrap_release(pssense, exposure_timestamp_ns);
	}

	if (b->output_generation != pssense->tracking.led_bootstrap_programmed_generation) {
		pssense->tracking.led_bootstrap_programmed_generation = b->output_generation;
		// All timing is absorbed into the fudge offset; the driver's own clock sync handles device time.
		pssense->tracking.latest_led_sync_sample = (struct t_led_sync_sample){
		    .timestamp.device_host_latency_ns = 0,
		    .timestamp_mode = T_LED_SYNC_SAMPLE_TIMESTAMP_MODE_DEVICE_HOST_LATENCY,
		    .fudge_offset_ns = b->fudge_offset_ns,
		    .blink_duration_ns = b->blink_ns,
		};
		pssense->tracking.period_id = DURATION_NS_TO_PERIOD_ID(b->blink_ns);
		pssense->tracking.led_sync_sample_needs_sending = true;
		pssense->tracking.led_sequence_num += 1;
		pssense->tracking.led_content_generation++;
	}

	if (b->state == T_LED_PHASE_BOOTSTRAP_LOCKED && ++pssense->tracking.led_bootstrap_status_frames >= 300) {
		pssense->tracking.led_bootstrap_status_frames = 0;
		PSSENSE_INFO(pssense,
		             "LED_BOOTSTRAP side=%c event=locked_status fudge_us=%.1f pulse_us=%.1f lit_reports=%u/%u "
		             "frames_since_lit=%u",
		             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', (double)b->fudge_offset_ns / 1000.0,
		             (double)b->blink_ns / 1000.0, b->locked_lit_reports, b->locked_reports,
		             b->frames_since_lit);
		b->locked_lit_reports = 0;
		b->locked_reports = 0;
	}

	pssense_led_bootstrap_record_changes(pssense, exposure_timestamp_ns);

	return t_led_phase_bootstrap_leds_enabled(b);
}

static void
pssense_timing_event_sink_push(struct t_timing_event_sink *sink, const struct t_timing_event *event)
{
	struct pssense_device *pssense = from_timing_event_sink(sink);

	// We only care about exposure start.
	if (event->type != T_TIMING_EVENT_TYPE_CAMERA_EXPOSURE_START) {
		return;
	}

	struct t_timing_event_camera_exposure_start camera_exposure = event->camera_exposure_start;

	PSSENSE_TRACE(pssense, "Received timing event: %d, seq id: %" PRIu64 ", timestamp: %" PRId64 "ns", event->type,
	              camera_exposure.sequence_id, camera_exposure.timestamp_ns);

	if (pssense->tracking.received_frames++ == 0) {
		pssense->tracking.last_exposure_sequence_id = camera_exposure.sequence_id;
		pssense->tracking.last_exposure_local_timestamp_ns = camera_exposure.timestamp_ns;
		return;
	}

	uint32_t sequence_id_delta = camera_exposure.sequence_id - pssense->tracking.last_exposure_sequence_id;
	if (sequence_id_delta == 0) {
		return;
	}

	time_duration_ns estimated_interval =
	    (camera_exposure.timestamp_ns - pssense->tracking.last_exposure_local_timestamp_ns) / (sequence_id_delta);

	// If available, use the frame period from the event
	if (camera_exposure.frame_period_ns > 0) {
		estimated_interval = camera_exposure.frame_period_ns;
	}

	// If this is the second frame we've received
	if (pssense->tracking.received_frames == 2) {
		pssense->tracking.average_exposure_interval_ns = estimated_interval;
		PSSENSE_TRACE(pssense, "Initial exposure interval: %" PRId64 "ns", estimated_interval);
	} else {
		// Iteratively average out the estimated interval to try to avoid noise
		pssense->tracking.average_exposure_interval_ns =
		    (estimated_interval * 0.1f) + (pssense->tracking.average_exposure_interval_ns * 0.9f);
		PSSENSE_TRACE(pssense, "Updated exposure interval: %" PRId64 "ns",
		              pssense->tracking.average_exposure_interval_ns);
	}

	pssense->tracking.last_exposure_sequence_id = camera_exposure.sequence_id;
	pssense->tracking.last_exposure_local_timestamp_ns = camera_exposure.timestamp_ns;

	bool future_led_schedule = debug_get_bool_option_pssense_future_led_schedule();
	bool use_led_bootstrap = pssense->tracking.use_led_bootstrap && pssense->tracking.use_constellation;
	bool run_optical_refinement =
	    !use_led_bootstrap && (!future_led_schedule || pssense->tracking.use_constellation);
	if (pssense->tracking.average_exposure_interval_ns > 0 && run_optical_refinement) {
		// Update the frame period to the one we're using internally and push the timing event
		struct t_timing_event_camera_exposure_start led_sync_event = event->camera_exposure_start;
		led_sync_event.frame_period_ns = pssense->tracking.average_exposure_interval_ns;
		t_led_sync_push_timing_event(&pssense->tracking.led_sync_refinement, &led_sync_event);
	}

	os_thread_helper_lock(&pssense->controller_thread);

	// update the LED settings
	if (pssense->tracking.received_frames > 10 && pssense->timing.has_clock_offset) {
		bool leds_lit = true;
		if (use_led_bootstrap && pssense->tracking.average_exposure_interval_ns > 0) {
			leds_lit = pssense_led_bootstrap_update_locked(pssense, camera_exposure.timestamp_ns);
		}

		// Update the sample from the LED sync routine
		if (run_optical_refinement && t_led_sync_get_sample(&pssense->tracking.led_sync_refinement,
		                                                    &pssense->tracking.latest_led_sync_sample)) {
			pssense->tracking.led_sync_sample_needs_sending = true;
			pssense->tracking.period_id =
			    DURATION_NS_TO_PERIOD_ID(pssense->tracking.latest_led_sync_sample.blink_duration_ns);
			pssense->tracking.led_sequence_num += 1;
			pssense->tracking.led_content_generation++;
		}

		uint8_t period_id = pssense->tracking.period_id;
		long requested_period_id = debug_get_num_option_pssense_led_period_id();
		if (requested_period_id > 0 && requested_period_id <= UINT8_MAX) {
			period_id = (uint8_t)requested_period_id;
		}

		// Convert the timestamp, latency offset will be applied within here. Must happen after fetching the
		// sample above, so the latency and the fudge offset below come from the same sample.
		timepoint_ns next_blink_time = 0;
		timepoint_ns now_ns = os_monotonic_get_ns();
		timepoint_ns schedule_host_ns = pssense->tracking.last_exposure_local_timestamp_ns;
		uint64_t periods_forward = 0;
		if (future_led_schedule && pssense->tracking.average_exposure_interval_ns > 0) {
			timepoint_ns target_host_ns = now_ns + PSSENSE_FUTURE_LED_LEAD_NS;
			if (schedule_host_ns < target_host_ns) {
				time_duration_ns delta_ns = target_host_ns - schedule_host_ns;
				time_duration_ns period_ns = pssense->tracking.average_exposure_interval_ns;
				periods_forward = (uint64_t)((delta_ns + period_ns - 1) / period_ns);
				schedule_host_ns += (time_duration_ns)periods_forward * period_ns;
			}
		}
		// Convert the timestamp, latency offset will be applied within here
		bool ts_valid = pssense_host_ts_to_device(pssense, schedule_host_ns, &next_blink_time);
		// We check if we have a clock offset above, so this will always return true
		if (!ts_valid) {
			os_thread_helper_unlock(&pssense->controller_thread);
			return;
		}

		next_blink_time += (int64_t)pssense->tracking.timing_fudge_100us * 100 * U_TIME_1US_IN_NS;
		// Apply the fudge offset, which will line up the blink center with exposure center
		next_blink_time += (int64_t)pssense->tracking.latest_led_sync_sample.fudge_offset_ns;

		// PSSENSE cycle position on the wire is the *center* of the exposure, but our LED sync assumes it's the
		// start of the exposure, so we need to make it blink later to account
		next_blink_time += PERIOD_ID_TO_DURATION_NS(period_id) / 2;

		// inside thirds of a nanosecond
		uint32_t cycle_length = pssense->tracking.average_exposure_interval_ns * 3;
		/*
		 * PSSENSE_LED_NOMINAL_CYCLE: Sony's driver sends a constant 50,050,050 (one nominal 59.94 Hz frame) in
		 * every report and corrects drift by re-anchoring; the measured average changes the value on almost
		 * every latch. Opt-in while the always-lit fault is investigated against Sony's command stream.
		 */
		if (debug_get_bool_option_pssense_led_nominal_cycle()) {
			cycle_length = PSSENSE_NOMINAL_CYCLE_LENGTH;
		}
		// in IMU ticks
		uint32_t cycle_position = NS_TO_IMU_TICKS(next_blink_time);

		/*
		 * PSSENSE_LED_LATCH_INTERVAL_MS: keep the latched anchor, letting the controller run on cycle_length,
		 * and re-latch only after the interval or when the schedule's content changes. Every new anchor carries
		 * the host/device mapping error of its moment; Sony's driver latches about once per second in PRESCAN
		 * (anchors 60 frames apart), where this driver otherwise latches every exposure.
		 */
		pssense_led_sweep_update_locked(pssense, now_ns,
		                                use_led_bootstrap && leds_lit &&
		                                    pssense->tracking.led_bootstrap.state ==
		                                        T_LED_PHASE_BOOTSTRAP_LOCKED);
		const bool sweep_changed =
		    pssense->tracking.led_sweep_latched_generation != pssense->tracking.led_sweep_generation;
		long latch_interval_ms = debug_get_num_option_pssense_led_latch_interval_ms();
		const uint8_t phase = leds_lit ? LED_SYNC_PHASE_PRESCAN : LED_SYNC_PHASE_LED_ALL_OFF;
		const char side = pssense->hand == XRT_HAND_LEFT ? 'L' : 'R';

		/*
		 * PSSENSE_LED_BROAD_S (experimental): Sony's driver, once tracking, alternates 10 s of BROAD with about
		 * 3 s of PRESCAN (three anchors 1 s apart). BROAD carries cycle_position 0: the controller keeps the
		 * last PRESCAN anchor and free-runs on cycle_length, so no anchor depends on the host/device mapping.
		 * Here: once the LED bootstrap holds its lock, latch three PRESCAN anchors 1 s apart, then BROAD for
		 * the configured time, and repeat. Any change of schedule content, LEDs going dark or losing the lock
		 * ends BROAD at once.
		 */
		const long broad_s = debug_get_num_option_pssense_led_broad_s();
		if (broad_s > 0) {
			const struct t_led_phase_bootstrap *b = &pssense->tracking.led_bootstrap;
			const bool steady = use_led_bootstrap && leds_lit && b->state == T_LED_PHASE_BOOTSTRAP_LOCKED &&
			                    !t_led_phase_bootstrap_is_probing(b) && pssense->tracking.led_latched &&
			                    pssense->tracking.led_latched_content_generation ==
			                        pssense->tracking.led_content_generation;
			latch_interval_ms = latch_interval_ms > 1000 ? latch_interval_ms : 1000;
			bool force_latch = false;
			if (pssense->tracking.led_broad_active) {
				const timepoint_ns elapsed_ns = now_ns - pssense->tracking.led_broad_started_ns;
				if (steady && elapsed_ns < (timepoint_ns)broad_s * U_TIME_1S_IN_NS) {
					if (sweep_changed) {
						// As Sony's driver does within BROAD: latch a new led_blink, keep the
						// anchor.
						memcpy(pssense->tracking.led_settings.led_blink,
						       pssense->tracking.led_blink,
						       sizeof(pssense->tracking.led_blink));
						pssense->tracking.led_settings.sequence_number =
						    pssense->tracking.led_sequence_num++;
						pssense->tracking.led_sweep_latched_generation =
						    pssense->tracking.led_sweep_generation;
					}
					os_thread_helper_unlock(&pssense->controller_thread);
					return;
				}
				PSSENSE_INFO(pssense, "LED_BROAD side=%c event=%s window=%u elapsed_ms=%.0f", side,
				             steady ? "end" : "abort", pssense->tracking.led_broad_windows,
				             (double)elapsed_ns / 1e6);
				pssense->tracking.led_broad_active = false;
				pssense->tracking.led_broad_anchors = 0;
				force_latch = true;
			} else if (!steady) {
				pssense->tracking.led_broad_anchors = 0;
			} else if (pssense->tracking.led_broad_anchors >= 3 &&
			           now_ns - pssense->tracking.led_latched_ns >= 75 * U_TIME_1MS_IN_NS) {
				uint8_t broad_period_id = period_id;
				long requested_broad = debug_get_num_option_pssense_led_broad_period_id();
				if (requested_broad > 0 && requested_broad <= UINT8_MAX) {
					broad_period_id = (uint8_t)requested_broad;
				}
				pssense->tracking.led_settings = (struct pssense_led_settings){
				    .phase = LED_SYNC_PHASE_BROAD,
				    .cycle_length = __cpu_to_le32(cycle_length),
				    .cycle_position = __cpu_to_le32(0),
				    .sequence_number = pssense->tracking.led_sequence_num++,
				    .led_blink = {0xFF, 0xFF, 0xFF, 0xFF},
				    .period_id = broad_period_id,
				};
				memcpy(pssense->tracking.led_settings.led_blink, pssense->tracking.led_blink,
				       sizeof(pssense->tracking.led_blink));
				pssense->tracking.led_sweep_latched_generation = pssense->tracking.led_sweep_generation;
				pssense->tracking.led_broad_active = true;
				pssense->tracking.led_broad_started_ns = now_ns;
				pssense->tracking.led_broad_windows++;
				pssense->tracking.led_latched_ns = now_ns;
				PSSENSE_INFO(pssense, "LED_BROAD side=%c event=start window=%u period_id=%u", side,
				             pssense->tracking.led_broad_windows, broad_period_id);
				os_thread_helper_unlock(&pssense->controller_thread);
				return;
			}
			/*
			 * Hold whenever the schedule's content is unchanged, not only when steady: scans, probes and
			 * LED-off periods otherwise re-latched every exposure (~50/s). On 5 Oct two right lockouts
			 * began in such bursts (a probe after a BROAD window, and one during a weak lock); Sony's
			 * driver never latches per frame.
			 */
			if (!force_latch && !sweep_changed && pssense->tracking.led_latched &&
			    pssense->tracking.led_settings.phase == phase &&
			    pssense->tracking.led_settings.period_id == period_id &&
			    pssense->tracking.led_latched_content_generation ==
			        pssense->tracking.led_content_generation &&
			    now_ns - pssense->tracking.led_latched_ns <
			        (timepoint_ns)latch_interval_ms * U_TIME_1MS_IN_NS) {
				os_thread_helper_unlock(&pssense->controller_thread);
				return;
			}
			if (steady || force_latch) {
				pssense->tracking.led_broad_anchors++;
			}
		} else if (latch_interval_ms > 0 && !sweep_changed && pssense->tracking.led_latched &&
		           pssense->tracking.led_settings.phase == phase &&
		           pssense->tracking.led_settings.period_id == period_id &&
		           pssense->tracking.led_latched_content_generation ==
		               pssense->tracking.led_content_generation &&
		           now_ns - pssense->tracking.led_latched_ns <
		               (timepoint_ns)latch_interval_ms * U_TIME_1MS_IN_NS) {
			os_thread_helper_unlock(&pssense->controller_thread);
			return;
		}
		pssense->tracking.led_latched = true;
		pssense->tracking.led_latched_content_generation = pssense->tracking.led_content_generation;
		pssense->tracking.led_latched_ns = now_ns;
		pssense->tracking.led_sweep_latched_generation = pssense->tracking.led_sweep_generation;

		if (debug_get_bool_option_pssense_timing_diag()) {
			timepoint_ns controller_now_ns = 0;
			timepoint_ns blink_host_est_ns = 0;
			bool controller_now_valid = pssense_host_ts_to_device(pssense, now_ns, &controller_now_ns);
			bool blink_host_valid = pssense_device_ts_to_host(pssense, next_blink_time, &blink_host_est_ns);
			PSSENSE_INFO(pssense,
			             "LED_SCHEDULE side=%c now=%" PRIi64 " raw_exposure=%" PRIi64 " age=%" PRIi64
			             " period=%" PRIi64 " forward=%" PRIu64 " projected=%" PRIi64
			             " projected_lead=%" PRIi64 " controller_now=%" PRIi64
			             " cycle_position=%u blink_host=%" PRIi64 " blink_minus_projected=%" PRIi64
			             " period_id=%u pulse=%" PRIi64,
			             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', now_ns,
			             pssense->tracking.last_exposure_local_timestamp_ns,
			             now_ns - pssense->tracking.last_exposure_local_timestamp_ns,
			             pssense->tracking.average_exposure_interval_ns, periods_forward, schedule_host_ns,
			             schedule_host_ns - now_ns, controller_now_valid ? controller_now_ns : -1,
			             cycle_position, blink_host_valid ? blink_host_est_ns : -1,
			             blink_host_valid ? blink_host_est_ns - schedule_host_ns : 0, period_id,
			             PERIOD_ID_TO_DURATION_NS(period_id));
		}

#if 0
		static int64_t jitter_integration = 0;

		PSSENSE_DEBUG(pssense, "(blink length %uus) drift %ldus",
		              (uint32_t)(IMU_TICKS_TO_NS(150LLU * 32 + 1250) / 1000), jitter_integration / 1000);
		jitter_integration += jitter;

		if (jitter_integration > U_TIME_1S_IN_NS) {
			jitter_integration = 0;
		}
#endif

		pssense->tracking.led_settings = (struct pssense_led_settings){
		    .phase = LED_SYNC_PHASE_PRESCAN,
		    .cycle_length = __cpu_to_le32(cycle_length),
		    .cycle_position = __cpu_to_le32(cycle_position),
		    .sequence_number = pssense->tracking.led_sequence_num,
		    .led_blink = {0xFF, 0xFF, 0xFF, 0xFF},
		    .period_id = period_id,
		};
		memcpy(pssense->tracking.led_settings.led_blink, pssense->tracking.led_blink,
		       sizeof(pssense->tracking.led_blink));
		if (!leds_lit) {
			pssense->tracking.led_settings.phase = LED_SYNC_PHASE_LED_ALL_OFF;
		}

		if (pssense->tracking.increment_sequence_num) {
			pssense->tracking.led_sequence_num += 1;
#if 0
			PSSENSE_DEBUG(pssense, "%lu\t%lu",
			              (pssense->tracking.last_exposure_local_timestamp_ns %
			               pssense->tracking.average_exposure_interval_ns) /
			                  1000,
			              (next_blink_time % pssense->tracking.average_exposure_interval_ns) / 1000);
#endif
		}
	}
	os_thread_helper_unlock(&pssense->controller_thread);
}

/*
 *
 * Constellation tracker device implementations
 *
 */

static void
pssense_push_camera_blob_count(struct t_constellation_tracker_device *device,
                               size_t camera_index,
                               int64_t timestamp_ns,
                               uint32_t blob_count)
{
	struct pssense_device *pssense = from_constellation_device(device);

	os_thread_helper_lock(&pssense->controller_thread);
	if (pssense->tracking.use_led_bootstrap && !pssense->tracking.led_bootstrap_led_blobs &&
	    !pssense->tracking.led_bootstrap_yielding) {
		t_led_phase_bootstrap_push_blob_count(&pssense->tracking.led_bootstrap, (uint32_t)camera_index,
		                                      timestamp_ns, blob_count);
	}
	os_thread_helper_unlock(&pssense->controller_thread);
}

static void
pssense_push_camera_led_blob_count(struct t_constellation_tracker_device *device,
                                   size_t camera_index,
                                   int64_t timestamp_ns,
                                   uint32_t led_blob_count,
                                   uint32_t matched_blob_count)
{
	struct pssense_device *pssense = from_constellation_device(device);

	os_thread_helper_lock(&pssense->controller_thread);
	if (pssense->tracking.use_led_bootstrap && pssense->tracking.led_bootstrap_led_blobs &&
	    !pssense->tracking.led_bootstrap_yielding) {
		t_led_phase_bootstrap_push_blob_count(&pssense->tracking.led_bootstrap, (uint32_t)camera_index,
		                                      timestamp_ns, led_blob_count);
	}
	if (pssense->tracking.use_led_bootstrap && !pssense->tracking.led_bootstrap_yielding) {
		t_led_phase_bootstrap_push_own_matched(&pssense->tracking.led_bootstrap, (uint32_t)camera_index,
		                                       timestamp_ns, matched_blob_count);
	}
	os_thread_helper_unlock(&pssense->controller_thread);
}

/*!
 * Commit an authoritative optical pose (a fused per-camera group, or a joint multi-camera solve): refresh the
 * optical<-IMU alignment, the last fused pose, LED sync and the optical relation history. Called with the
 * controller_thread lock held; returns with it released.
 */
static bool
pssense_commit_optical_pose_locked(struct pssense_device *pssense,
                                   struct t_constellation_tracker_sample *sample,
                                   uint32_t camera_count,
                                   bool reacquiring,
                                   bool joint)
{
	/* Refresh the optical<-IMU orientation alignment from every trusted fused pose. */
	struct xrt_space_relation fused_imu_relation = XRT_SPACE_RELATION_ZERO;
	pssense_get_corrected_imu_pose(pssense, sample->timestamp_ns, &fused_imu_relation);
	bool have_fused_imu_orientation =
	    (fused_imu_relation.relation_flags &
	     (XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT)) ==
	    (XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	if (have_fused_imu_orientation) {
		struct xrt_quat inverse_imu_orientation;
		math_quat_invert(&fused_imu_relation.pose.orientation, &inverse_imu_orientation);
		math_quat_rotate(&sample->pose.orientation, &inverse_imu_orientation,
		                 &pssense->tracking.optical_from_imu_orientation);
		math_quat_normalize(&pssense->tracking.optical_from_imu_orientation);
		pssense->tracking.have_optical_from_imu_orientation = true;
		pssense->tracking.optical_from_imu_timestamp_ns = sample->timestamp_ns;
	}

	if (pssense->tracking.filter != NULL) {
		// Noise grows with the solve's reprojection error: 2 mm and 0.46 deg at 0.5 px or better.
		float scale = (float)fmax(1.0, sample->metrics.reprojection_error / 0.5);
		enum t_imu_optical_filter_update_result result = t_imu_optical_filter_push_pose(
		    pssense->tracking.filter, sample->timestamp_ns, &sample->pose, 0.002f * scale, 0.008f * scale);
		struct t_imu_optical_filter_stats stats;
		t_imu_optical_filter_get_stats(pssense->tracking.filter, &stats);
		if (result == T_IMU_OPTICAL_FILTER_INITIALISED || result == T_IMU_OPTICAL_FILTER_REINITIALISED ||
		    stats.updates >= pssense->tracking.filter_last_logged_updates + 300) {
			pssense->tracking.filter_last_logged_updates = stats.updates;
			PSSENSE_INFO(pssense,
			             "FILTER side=%c event=%s updates=%" PRIu64 " rejections=%" PRIu64
			             " reinitialisations=%" PRIu64
			             " mahalanobis2=%.2f gyro_bias_deg_s=%.2f,%.2f,%.2f "
			             "accel_bias_m_s2=%.3f,%.3f,%.3f",
			             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R',
			             result == T_IMU_OPTICAL_FILTER_INITIALISED     ? "initialised"
			             : result == T_IMU_OPTICAL_FILTER_REINITIALISED ? "reinitialised"
			                                                            : "status",
			             stats.updates, stats.rejections, stats.reinitialisations, stats.last_mahalanobis2,
			             stats.gyro_bias_rad_s.x * 180.0 / M_PI, stats.gyro_bias_rad_s.y * 180.0 / M_PI,
			             stats.gyro_bias_rad_s.z * 180.0 / M_PI, stats.accel_bias_m_s2.x,
			             stats.accel_bias_m_s2.y, stats.accel_bias_m_s2.z);
		}
	}

	pssense->tracking.last_fused_pose = sample->pose;
	pssense->tracking.have_last_fused_pose = true;
	pssense->tracking.last_fused_timestamp_ns = sample->timestamp_ns;
	pssense->tracking.last_fused_camera_count = camera_count;
	pssense->tracking.fused_pose_count++;
	os_thread_helper_unlock(&pssense->controller_thread);

	/*
	 * The device callback contract allows replacing the camera-local solve. Only the
	 * synchronized fused (or jointly solved) pose is authoritative: it seeds tracker state,
	 * LED timing, and optical translation history exactly once for this camera epoch.
	 */
	t_led_sync_push_constellation_sample(&pssense->tracking.led_sync_refinement, sample);

	os_thread_helper_lock(&pssense->controller_thread);
	timepoint_ns optical_device_ts;
	bool have_optical_device_ts = pssense_host_ts_to_device(pssense, sample->timestamp_ns, &optical_device_ts);
	os_thread_helper_unlock(&pssense->controller_thread);

	if (have_optical_device_ts) {
		struct xrt_space_relation optical_relation = {
		    .pose = sample->pose,
		    .relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		                      XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
		                      XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT,
		};
		if (m_relation_history_push(pssense->tracking.constellation_relation_history, &optical_relation,
		                            optical_device_ts)) {
			os_thread_helper_lock(&pssense->controller_thread);
			pssense->tracking.last_optical_timestamp_ns =
			    MAX(pssense->tracking.last_optical_timestamp_ns, sample->timestamp_ns);
			os_thread_helper_unlock(&pssense->controller_thread);
		}
	}

	if (xrt_atomic_s32_load(&pssense_led_bootstrap_release_token) == pssense_led_bootstrap_token(pssense)) {
		xrt_atomic_s32_store(&pssense_led_bootstrap_released_tracked, 1);
	}

	PSSENSE_INFO(pssense,
	             "CONSTELLATION_FUSED_ACCEPT side=%c ts=%" PRIi64
	             " cameras=%u matched=%u reproj=%.3f reacquired=%u imu_alignment_valid=%u joint=%u",
	             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', sample->timestamp_ns, camera_count,
	             sample->metrics.matched_blob_count, sample->metrics.reprojection_error, reacquiring ? 1u : 0u,
	             have_fused_imu_orientation ? 1u : 0u, joint ? 1u : 0u);
	return true;
}

/*!
 * A pose solved jointly across cameras by the tracker (CONSTELLATION_TRACKER_JOINT=1). It is already the consensus
 * for its exposure, so the per-camera grouping, pairwise agreement gate and averaging are skipped. So is the
 * fresh-pose jump gate: the joint path re-acquires by bootstrap, which carries its own acceptance tests.
 */
static bool
pssense_accept_joint_sample(struct pssense_device *pssense, struct t_constellation_tracker_sample *sample)
{
	os_thread_helper_lock(&pssense->controller_thread);
	pssense->tracking.candidate_count++;

	bool reacquiring = false;
	if (pssense->tracking.have_last_fused_pose) {
		if (sample->timestamp_ns <= pssense->tracking.last_fused_timestamp_ns) {
			pssense->tracking.jump_rejection_count++;
			os_thread_helper_unlock(&pssense->controller_thread);
			return false;
		}
		int64_t age_ns = sample->timestamp_ns - pssense->tracking.last_fused_timestamp_ns;
		if (age_ns > PSSENSE_CONSTELLATION_STALE_NS) {
			reacquiring = true;
			struct xrt_pose *last = &pssense->tracking.last_fused_pose;
			float dx = last->position.x - sample->pose.position.x;
			float dy = last->position.y - sample->pose.position.y;
			float dz = last->position.z - sample->pose.position.z;
			PSSENSE_INFO(pssense,
			             "CONSTELLATION_REACQUIRE side=%c ts=%" PRIi64
			             " gap_ms=%.1f cameras=%u pos_delta_mm=%.1f joint=1",
			             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', sample->timestamp_ns,
			             (double)age_ns / 1000000.0, sample->joint_camera_count,
			             sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f);
		}
	}
	if (pssense->tracking.use_led_bootstrap && !pssense->tracking.led_bootstrap_yielding &&
	    sample->metrics.visible_led_count > 0) {
		t_led_phase_bootstrap_push_pose_coverage(&pssense->tracking.led_bootstrap, sample->timestamp_ns,
		                                         (float)sample->metrics.matched_blob_count /
		                                             (float)sample->metrics.visible_led_count);
	}
	return pssense_commit_optical_pose_locked(pssense, sample, sample->joint_camera_count, reacquiring, true);
}

static void
pssense_push_constellation_tracker_sample_upstream(struct t_constellation_tracker_device *device,
                                                   struct t_constellation_tracker_sample *sample)
{
	struct pssense_device *pssense = from_constellation_device(device);

	t_led_sync_push_constellation_sample(&pssense->tracking.led_sync_refinement, sample);

	os_thread_helper_lock(&pssense->controller_thread);
	timepoint_ns device_ts;
	if (!pssense_host_ts_to_device(pssense, sample->timestamp_ns, &device_ts)) {
		os_thread_helper_unlock(&pssense->controller_thread);
		return;
	}
	os_thread_helper_unlock(&pssense->controller_thread);

	struct xrt_space_relation relation = {
	    .pose = sample->pose,
	    .relation_flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                      XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT,
	};

	m_relation_history_push(pssense->tracking.constellation_relation_history, &relation, device_ts);
}

static bool
pssense_push_constellation_tracker_sample(struct t_constellation_tracker_device *device,
                                          struct t_constellation_tracker_sample *sample)
{
	struct pssense_device *pssense = from_constellation_device(device);
	struct t_constellation_tracker_sample fused = {0};

	if (sample->joint_camera_count > 0) {
		return pssense_accept_joint_sample(pssense, sample);
	}

	/*
	 * Camera-local solves are candidates only. Do not let one sparse or incorrect
	 * correspondence update LED timing, optical history, or the tracker's persistent
	 * pose prior before a synchronized multi-camera consensus exists.
	 */

	os_thread_helper_lock(&pssense->controller_thread);

	/*
	 * Diagnostic only: sample the same corrected IMU orientation used by the
	 * constellation tracking source at this camera exposure timestamp, then
	 * measure the camera-local optical candidate against it. Do not gate on
	 * this residual yet.
	 */
	struct xrt_space_relation imu_orientation_relation = XRT_SPACE_RELATION_ZERO;
	pssense_get_corrected_imu_pose(pssense, sample->timestamp_ns, &imu_orientation_relation);
	bool have_imu_orientation =
	    (imu_orientation_relation.relation_flags &
	     (XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT)) ==
	    (XRT_SPACE_RELATION_ORIENTATION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	float imu_delta_deg = NAN;
	float imu_aligned_delta_deg = NAN;
	double imu_alignment_age_ms = NAN;
	if (have_imu_orientation) {
		float imu_dot = fabsf(sample->pose.orientation.x * imu_orientation_relation.pose.orientation.x +
		                      sample->pose.orientation.y * imu_orientation_relation.pose.orientation.y +
		                      sample->pose.orientation.z * imu_orientation_relation.pose.orientation.z +
		                      sample->pose.orientation.w * imu_orientation_relation.pose.orientation.w);
		imu_delta_deg = 2.0f * acosf(CLAMP(imu_dot, 0.0f, 1.0f)) * 180.0f / (float)M_PI;

		if (pssense->tracking.have_optical_from_imu_orientation) {
			struct xrt_quat aligned_imu_orientation;
			math_quat_rotate(&pssense->tracking.optical_from_imu_orientation,
			                 &imu_orientation_relation.pose.orientation, &aligned_imu_orientation);
			float aligned_dot = fabsf(sample->pose.orientation.x * aligned_imu_orientation.x +
			                          sample->pose.orientation.y * aligned_imu_orientation.y +
			                          sample->pose.orientation.z * aligned_imu_orientation.z +
			                          sample->pose.orientation.w * aligned_imu_orientation.w);
			imu_aligned_delta_deg = 2.0f * acosf(CLAMP(aligned_dot, 0.0f, 1.0f)) * 180.0f / (float)M_PI;
			imu_alignment_age_ms =
			    (double)(sample->timestamp_ns - pssense->tracking.optical_from_imu_timestamp_ns) /
			    1000000.0;
		}
	}

	pssense->tracking.candidate_count++;
	if (sample->camera_index < PSSENSE_CONSTELLATION_CAMERA_COUNT) {
		pssense->tracking.camera_candidate_count[sample->camera_index]++;
	}
	PSSENSE_INFO(pssense,
	             "CONSTELLATION_CANDIDATE side=%c ts=%" PRIi64
	             " cam=%zu pos=(%.6f,%.6f,%.6f) quat=(%.6f,%.6f,%.6f,%.6f) matched=%u visible=%u reproj=%.3f "
	             "brightness=%.3f imu_valid=%u imu_quat=(%.6f,%.6f,%.6f,%.6f) imu_delta_deg=%.2f "
	             "imu_aligned_valid=%u imu_aligned_delta_deg=%.2f imu_alignment_age_ms=%.1f",
	             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', sample->timestamp_ns, sample->camera_index,
	             sample->pose.position.x, sample->pose.position.y, sample->pose.position.z,
	             sample->pose.orientation.x, sample->pose.orientation.y, sample->pose.orientation.z,
	             sample->pose.orientation.w, sample->metrics.matched_blob_count, sample->metrics.visible_led_count,
	             sample->metrics.reprojection_error, sample->average_brightness, have_imu_orientation ? 1u : 0u,
	             imu_orientation_relation.pose.orientation.x, imu_orientation_relation.pose.orientation.y,
	             imu_orientation_relation.pose.orientation.z, imu_orientation_relation.pose.orientation.w,
	             imu_delta_deg,
	             have_imu_orientation && pssense->tracking.have_optical_from_imu_orientation ? 1u : 0u,
	             imu_aligned_delta_deg, imu_alignment_age_ms);
	if (sample->camera_index >= PSSENSE_CONSTELLATION_CAMERA_COUNT || sample->metrics.matched_blob_count < 3 ||
	    !isfinite(sample->metrics.reprojection_error) || sample->metrics.reprojection_error > 5.0) {
		pssense->tracking.disagreement_count++;
		os_thread_helper_unlock(&pssense->controller_thread);
		return false;
	}

	struct pssense_constellation_candidate_group *group = NULL;
	for (size_t i = 0; i < PSSENSE_CONSTELLATION_GROUP_COUNT; i++) {
		if (llabs(pssense->tracking.candidate_groups[i].timestamp_ns - sample->timestamp_ns) <=
		    PSSENSE_CONSTELLATION_SYNC_TOLERANCE_NS) {
			group = &pssense->tracking.candidate_groups[i];
			break;
		}
	}
	if (group == NULL) {
		group = &pssense->tracking.candidate_groups[pssense->tracking.next_candidate_group];
		pssense->tracking.next_candidate_group =
		    (pssense->tracking.next_candidate_group + 1) % PSSENSE_CONSTELLATION_GROUP_COUNT;
		*group = (struct pssense_constellation_candidate_group){.timestamp_ns = sample->timestamp_ns};
	}
	group->samples[sample->camera_index] = *sample;
	group->present[sample->camera_index] = true;
	if (group->emitted) {
		os_thread_helper_unlock(&pssense->controller_thread);
		return false;
	}

	uint32_t best_anchor = 0;
	uint32_t best_camera_count = 0;
	for (uint32_t anchor = 0; anchor < PSSENSE_CONSTELLATION_CAMERA_COUNT; anchor++) {
		if (!group->present[anchor]) {
			continue;
		}
		uint32_t compatible = 0;
		for (uint32_t camera = 0; camera < PSSENSE_CONSTELLATION_CAMERA_COUNT; camera++) {
			if (!group->present[camera]) {
				continue;
			}
			struct xrt_pose *a = &group->samples[anchor].pose;
			struct xrt_pose *b = &group->samples[camera].pose;
			float dx = a->position.x - b->position.x;
			float dy = a->position.y - b->position.y;
			float dz = a->position.z - b->position.z;
			float position_delta = sqrtf(dx * dx + dy * dy + dz * dz);
			float dot = fabsf(a->orientation.x * b->orientation.x + a->orientation.y * b->orientation.y +
			                  a->orientation.z * b->orientation.z + a->orientation.w * b->orientation.w);
			float orientation_delta = 2.0f * acosf(CLAMP(dot, 0.0f, 1.0f));
			if (position_delta <= PSSENSE_CONSTELLATION_MAX_CAMERA_POSITION_DELTA_M &&
			    orientation_delta <= PSSENSE_CONSTELLATION_MAX_CAMERA_ORIENTATION_DELTA_RAD) {
				compatible++;
			}
		}
		if (compatible > best_camera_count) {
			best_camera_count = compatible;
			best_anchor = anchor;
		}
	}
	if (best_camera_count < 2) {
		uint32_t present_count = 0;
		for (uint32_t camera = 0; camera < PSSENSE_CONSTELLATION_CAMERA_COUNT; camera++) {
			present_count += group->present[camera] ? 1 : 0;
		}
		if (present_count >= 2 && !group->disagreement_recorded) {
			group->disagreement_recorded = true;
			pssense->tracking.disagreement_count++;
			for (uint32_t camera_a = 0; camera_a < PSSENSE_CONSTELLATION_CAMERA_COUNT; camera_a++) {
				if (!group->present[camera_a]) {
					continue;
				}
				for (uint32_t camera_b = camera_a + 1; camera_b < PSSENSE_CONSTELLATION_CAMERA_COUNT;
				     camera_b++) {
					if (!group->present[camera_b]) {
						continue;
					}
					struct t_constellation_tracker_sample *candidate_a = &group->samples[camera_a];
					struct t_constellation_tracker_sample *candidate_b = &group->samples[camera_b];
					float dx = candidate_a->pose.position.x - candidate_b->pose.position.x;
					float dy = candidate_a->pose.position.y - candidate_b->pose.position.y;
					float dz = candidate_a->pose.position.z - candidate_b->pose.position.z;
					float position_delta = sqrtf(dx * dx + dy * dy + dz * dz);
					float dot =
					    fabsf(candidate_a->pose.orientation.x * candidate_b->pose.orientation.x +
					          candidate_a->pose.orientation.y * candidate_b->pose.orientation.y +
					          candidate_a->pose.orientation.z * candidate_b->pose.orientation.z +
					          candidate_a->pose.orientation.w * candidate_b->pose.orientation.w);
					float orientation_delta = 2.0f * acosf(CLAMP(dot, 0.0f, 1.0f));
					PSSENSE_INFO(
					    pssense,
					    "CONSTELLATION_PAIR_REJECT side=%c ts=%" PRIi64
					    " cams=%u/%u dt_us=%.1f pos_delta_mm=%.1f orientation_delta_deg=%.1f",
					    pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', group->timestamp_ns, camera_a,
					    camera_b,
					    (double)llabs(candidate_a->timestamp_ns - candidate_b->timestamp_ns) /
					        1000.0,
					    position_delta * 1000.0f, orientation_delta * 180.0f / (float)M_PI);
				}
			}
			PSSENSE_INFO(pssense,
			             "Rejecting synchronized constellation candidates at %" PRIi64
			             ": %u cameras but no pair agrees within %.0f mm / %.0f deg",
			             sample->timestamp_ns, present_count,
			             PSSENSE_CONSTELLATION_MAX_CAMERA_POSITION_DELTA_M * 1000.0f,
			             PSSENSE_CONSTELLATION_MAX_CAMERA_ORIENTATION_DELTA_RAD * 180.0f / (float)M_PI);
		}
		os_thread_helper_unlock(&pssense->controller_thread);
		return false;
	}

	fused = group->samples[best_anchor];
	fused.pose = (struct xrt_pose)XRT_POSE_IDENTITY;
	fused.metrics = (struct t_constellation_tracker_sample_metrics){0};
	float quaternion[4] = {0};
	float total_weight = 0.0f;
	uint32_t fused_camera_count = 0;
	struct xrt_pose *anchor_pose = &group->samples[best_anchor].pose;
	for (uint32_t camera = 0; camera < PSSENSE_CONSTELLATION_CAMERA_COUNT; camera++) {
		if (!group->present[camera]) {
			continue;
		}
		struct t_constellation_tracker_sample *candidate = &group->samples[camera];
		float dx = anchor_pose->position.x - candidate->pose.position.x;
		float dy = anchor_pose->position.y - candidate->pose.position.y;
		float dz = anchor_pose->position.z - candidate->pose.position.z;
		float position_delta = sqrtf(dx * dx + dy * dy + dz * dz);
		float dot = anchor_pose->orientation.x * candidate->pose.orientation.x +
		            anchor_pose->orientation.y * candidate->pose.orientation.y +
		            anchor_pose->orientation.z * candidate->pose.orientation.z +
		            anchor_pose->orientation.w * candidate->pose.orientation.w;
		float orientation_delta = 2.0f * acosf(CLAMP(fabsf(dot), 0.0f, 1.0f));
		if (position_delta > PSSENSE_CONSTELLATION_MAX_CAMERA_POSITION_DELTA_M ||
		    orientation_delta > PSSENSE_CONSTELLATION_MAX_CAMERA_ORIENTATION_DELTA_RAD) {
			continue;
		}
		float weight =
		    (float)candidate->metrics.matched_blob_count /
		    (1.0f + (float)(candidate->metrics.reprojection_error * candidate->metrics.reprojection_error));
		float sign = dot < 0.0f ? -1.0f : 1.0f;
		fused.pose.position.x += weight * candidate->pose.position.x;
		fused.pose.position.y += weight * candidate->pose.position.y;
		fused.pose.position.z += weight * candidate->pose.position.z;
		quaternion[0] += weight * sign * candidate->pose.orientation.x;
		quaternion[1] += weight * sign * candidate->pose.orientation.y;
		quaternion[2] += weight * sign * candidate->pose.orientation.z;
		quaternion[3] += weight * sign * candidate->pose.orientation.w;
		fused.metrics.matched_blob_count += candidate->metrics.matched_blob_count;
		fused.metrics.visible_led_count += candidate->metrics.visible_led_count;
		fused.metrics.reprojection_error += weight * candidate->metrics.reprojection_error;
		fused.timestamp_ns = MAX(fused.timestamp_ns, candidate->timestamp_ns);
		total_weight += weight;
		fused_camera_count++;
	}
	fused.pose.position.x /= total_weight;
	fused.pose.position.y /= total_weight;
	fused.pose.position.z /= total_weight;
	fused.metrics.reprojection_error /= total_weight;
	float quaternion_norm = sqrtf(quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
	                              quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3]);
	fused.pose.orientation = (struct xrt_quat){quaternion[0] / quaternion_norm, quaternion[1] / quaternion_norm,
	                                           quaternion[2] / quaternion_norm, quaternion[3] / quaternion_norm};

	bool reacquiring = false;
	if (pssense->tracking.have_last_fused_pose) {
		if (fused.timestamp_ns <= pssense->tracking.last_fused_timestamp_ns) {
			group->emitted = true;
			pssense->tracking.jump_rejection_count++;
			os_thread_helper_unlock(&pssense->controller_thread);
			return false;
		}

		struct xrt_pose *last = &pssense->tracking.last_fused_pose;
		float dx = last->position.x - fused.pose.position.x;
		float dy = last->position.y - fused.pose.position.y;
		float dz = last->position.z - fused.pose.position.z;
		float position_delta = sqrtf(dx * dx + dy * dy + dz * dz);
		float dot = fabsf(
		    last->orientation.x * fused.pose.orientation.x + last->orientation.y * fused.pose.orientation.y +
		    last->orientation.z * fused.pose.orientation.z + last->orientation.w * fused.pose.orientation.w);
		float orientation_delta = 2.0f * acosf(CLAMP(dot, 0.0f, 1.0f));
		int64_t age_ns = fused.timestamp_ns - pssense->tracking.last_fused_timestamp_ns;

		if (age_ns <= PSSENSE_CONSTELLATION_STALE_NS) {
			if (position_delta > PSSENSE_CONSTELLATION_MAX_JUMP_POSITION_M ||
			    orientation_delta > PSSENSE_CONSTELLATION_MAX_JUMP_ORIENTATION_RAD) {
				group->emitted = true;
				pssense->tracking.jump_rejection_count++;
				os_thread_helper_unlock(&pssense->controller_thread);
				return false;
			}
		} else {
			reacquiring = true;
			PSSENSE_INFO(pssense,
			             "CONSTELLATION_REACQUIRE side=%c ts=%" PRIi64
			             " gap_ms=%.1f cameras=%u pos_delta_mm=%.1f orientation_delta_deg=%.1f",
			             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R', fused.timestamp_ns,
			             (double)age_ns / 1000000.0, fused_camera_count, position_delta * 1000.0f,
			             orientation_delta * 180.0f / (float)M_PI);
		}
	}

	group->emitted = true;
	*sample = fused;
	return pssense_commit_optical_pose_locked(pssense, sample, fused_camera_count, reacquiring, false);
}

/*
 *
 * Constellation tracking source implementations
 *
 */

static void
pssense_get_constellation_tracking_source_pose(struct t_constellation_tracker_tracking_source *tracking_source,
                                               int64_t when_ns,
                                               struct xrt_space_relation *out_relation)
{
	struct pssense_device *pssense = from_constellation_tracking_source(tracking_source);

	os_thread_helper_lock(&pssense->controller_thread);
	pssense_get_constellation_pose(pssense, when_ns, out_relation);
	os_thread_helper_unlock(&pssense->controller_thread);
}

/*
 *
 * Driver implementations
 *
 */

static void
pssense_device_destroy(struct xrt_device *xdev)
{
	struct pssense_device *pssense = from_device(xdev);

	if (debug_get_bool_option_pssense_leds_off_on_exit() && pssense->hid != NULL &&
	    pssense->controller_thread.initialized && os_thread_helper_is_running(&pssense->controller_thread)) {
		// Let the controller thread send LED_ALL_OFF (a new sequence number, so it latches) for a while first.
		os_thread_helper_lock(&pssense->controller_thread);
		pssense->output.exit_led_sequence = (uint8_t)(pssense->tracking.led_settings.sequence_number + 1);
		pssense->output.exiting = true;
		os_thread_helper_unlock(&pssense->controller_thread);
		os_nanosleep(150 * U_TIME_1MS_IN_NS);
		PSSENSE_INFO(pssense, "LEDS_OFF_ON_EXIT side=%c sent LED_ALL_OFF for 150 ms before closing",
		             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R');
	}

	// Stop the thread helper
	os_thread_helper_stop_and_wait(&pssense->controller_thread);

	// Don't free the pointer or destroy any resources,
	// since they may be needed after device destroy but before node destroy
}

static xrt_result_t
pssense_device_update_inputs(struct xrt_device *xdev)
{
	struct pssense_device *pssense = from_device(xdev);

	// Lock the data.
	os_thread_helper_lock(&pssense->controller_thread);

	timepoint_ns host_update_time_ns;
	if (!pssense_device_ts_to_host(pssense, pssense->timing.latest_device_time_ns, &host_update_time_ns)) {
		host_update_time_ns = pssense->state.timestamp_ns;
	}

	// Update all the inputs to the correct timestamp
	for (uint32_t i = 0; i < ((uint32_t)PSSENSE_INPUT_COUNT); i++) {
		pssense->base.inputs[i].timestamp = (int64_t)host_update_time_ns;
		pssense->base.inputs[i].active = pssense->connected;
	}
	pssense->base.inputs[PSSENSE_INDEX_PS_CLICK].value.boolean = pssense->state.ps_click;
	pssense->base.inputs[PSSENSE_INDEX_SHARE_CLICK].value.boolean = pssense->state.share_click;
	pssense->base.inputs[PSSENSE_INDEX_OPTIONS_CLICK].value.boolean = pssense->state.options_click;
	pssense->base.inputs[PSSENSE_INDEX_SQUARE_CLICK].value.boolean = pssense->state.square_click;
	pssense->base.inputs[PSSENSE_INDEX_SQUARE_TOUCH].value.boolean = pssense->state.square_touch;
	pssense->base.inputs[PSSENSE_INDEX_TRIANGLE_CLICK].value.boolean = pssense->state.triangle_click;
	pssense->base.inputs[PSSENSE_INDEX_TRIANGLE_TOUCH].value.boolean = pssense->state.triangle_touch;
	pssense->base.inputs[PSSENSE_INDEX_CROSS_CLICK].value.boolean = pssense->state.cross_click;
	pssense->base.inputs[PSSENSE_INDEX_CROSS_TOUCH].value.boolean = pssense->state.cross_touch;
	pssense->base.inputs[PSSENSE_INDEX_CIRCLE_CLICK].value.boolean = pssense->state.circle_click;
	pssense->base.inputs[PSSENSE_INDEX_CIRCLE_TOUCH].value.boolean = pssense->state.circle_touch;
	pssense->base.inputs[PSSENSE_INDEX_SQUEEZE_CLICK].value.boolean = pssense->state.squeeze_click;
	pssense->base.inputs[PSSENSE_INDEX_SQUEEZE_TOUCH].value.boolean = pssense->state.squeeze_touch;
	pssense->base.inputs[PSSENSE_INDEX_SQUEEZE_PROXIMITY].value.boolean = pssense->state.squeeze_proximity > 0.7f;
	pssense->base.inputs[PSSENSE_INDEX_SQUEEZE_PROXIMITY_FLOAT].value.vec1.x = pssense->state.squeeze_proximity;
	pssense->base.inputs[PSSENSE_INDEX_TRIGGER_CLICK].value.boolean = pssense->state.trigger_click;
	pssense->base.inputs[PSSENSE_INDEX_TRIGGER_TOUCH].value.boolean = pssense->state.trigger_touch;
	pssense->base.inputs[PSSENSE_INDEX_TRIGGER_VALUE].value.vec1.x = pssense->state.trigger_value;
	pssense->base.inputs[PSSENSE_INDEX_TRIGGER_PROXIMITY].value.boolean = pssense->state.trigger_proximity > 0.7f;
	pssense->base.inputs[PSSENSE_INDEX_TRIGGER_PROXIMITY_FLOAT].value.vec1.x = pssense->state.trigger_proximity;
	pssense->base.inputs[PSSENSE_INDEX_THUMBSTICK].value.vec2 = pssense->state.thumbstick;
	pssense->base.inputs[PSSENSE_INDEX_THUMBSTICK_CLICK].value.boolean = pssense->state.thumbstick_click;
	pssense->base.inputs[PSSENSE_INDEX_THUMBSTICK_TOUCH].value.boolean = pssense->state.thumbstick_touch;

	if (pssense->input_diagnostics && (!pssense->diagnostic_trigger_initialized ||
	                                   pssense->diagnostic_trigger_click != pssense->state.trigger_click)) {
		PSSENSE_WARN(pssense, "raw trigger edge: hand=%s click=%d touch=%d value=%.3f",
		             pssense->hand == XRT_HAND_LEFT ? "left" : "right", pssense->state.trigger_click ? 1 : 0,
		             pssense->state.trigger_touch ? 1 : 0, pssense->state.trigger_value);
		pssense->diagnostic_trigger_click = pssense->state.trigger_click;
		pssense->diagnostic_trigger_initialized = true;
	}

	// Done now.
	os_thread_helper_unlock(&pssense->controller_thread);

	return XRT_SUCCESS;
}

static xrt_result_t
set_vibration_output(struct pssense_device *pssense,
                     const struct xrt_output_value *value,
                     bool *send_vibration,
                     uint8_t *vibration_amplitude,
                     uint8_t *vibration_mode)
{
	switch (value->type) {
	case XRT_OUTPUT_VALUE_TYPE_VIBRATION: {
		*send_vibration = true;
		*vibration_amplitude = (uint8_t)(value->vibration.amplitude * 255.0f);
		*vibration_mode = OUTPUT_SETTINGS_VIBRATE_MODE_CLASSIC_RUMBLE;

		if (value->vibration.frequency != XRT_FREQUENCY_UNSPECIFIED) {
			if (value->vibration.frequency <= 70) {
				*vibration_mode = OUTPUT_SETTINGS_VIBRATE_MODE_LOW_60HZ;
			} else if (value->vibration.frequency >= 110) {
				*vibration_mode = OUTPUT_SETTINGS_VIBRATE_MODE_HIGH_120HZ;
			}
		}
		break;
	}
	case XRT_OUTPUT_VALUE_TYPE_PCM_VIBRATION: {
		os_thread_helper_lock(&pssense->controller_thread);
		// Reset the resampler if we're not appending.
		if (!value->pcm_vibration.append) {
			u_resampler_reset(pssense->output.pcm_haptics_resampler);
		}

		size_t samples_consumed =
		    u_resampler_write(pssense->output.pcm_haptics_resampler, value->pcm_vibration.buffer,
		                      value->pcm_vibration.buffer_size, value->pcm_vibration.sample_rate);
		os_thread_helper_unlock(&pssense->controller_thread);

		*value->pcm_vibration.samples_consumed = samples_consumed;
		break;
	}
	default: {
		U_LOG_XDEV_UNSUPPORTED_OUTPUT(&pssense->base, pssense->log_level, XRT_OUTPUT_NAME_PSSENSE_VIBRATION);
		return XRT_ERROR_OUTPUT_UNSUPPORTED;
		break;
	}
	}

	return XRT_SUCCESS;
}

static xrt_result_t
pssense_set_output(struct xrt_device *xdev, enum xrt_output_name name, const struct xrt_output_value *value)
{
	struct pssense_device *pssense = from_device(xdev);

	bool send_vibration = false;
	uint8_t vibration_amplitude;
	uint8_t vibration_mode;

	bool send_trigger_feedback = false;
	enum pssense_adaptive_trigger_mode trigger_feedback_mode;

	switch (name) {
	case XRT_OUTPUT_NAME_PSSENSE_VIBRATION: {
		xrt_result_t result =
		    set_vibration_output(pssense, value, &send_vibration, &vibration_amplitude, &vibration_mode);
		if (result != XRT_SUCCESS) {
			return result;
		}

		break;
	}
	case XRT_OUTPUT_NAME_PSSENSE_TRIGGER_FEEDBACK: {
		for (uint64_t i = 0; i < value->force_feedback.force_feedback_location_count; i++) {
			if (value->force_feedback.force_feedback[i].location ==
			    XRT_FORCE_FEEDBACK_LOCATION_LEFT_INDEX) {
				send_trigger_feedback = true;
				if (value->force_feedback.force_feedback[i].value > 0) {
					trigger_feedback_mode = TRIGGER_FEEDBACK_MODE_SIMPLE_FEEDBACK;
				} else {
					trigger_feedback_mode = TRIGGER_FEEDBACK_MODE_OFF;
				}
			}
		}

		break;
	}
	default: {
		U_LOG_XDEV_UNSUPPORTED_OUTPUT(&pssense->base, pssense->log_level, name);
		return XRT_ERROR_OUTPUT_UNSUPPORTED;
	}
	}

	timepoint_ns now = os_monotonic_get_ns();

	os_thread_helper_lock(&pssense->controller_thread);
	if (send_vibration && (vibration_amplitude != pssense->output.vibration_amplitude ||
	                       vibration_mode != pssense->output.vibration_mode)) {
		pssense->output.send_vibration = true;
		pssense->output.vibration_amplitude = vibration_amplitude;
		pssense->output.vibration_mode = vibration_mode;
		// Some applications (hello_xr has been seen doing this) will set the duration to INT64_MAX, so when
		// adding directly, it overflows and doesn't work. This prevents that.
		pssense->output.vibration_end_timestamp_ns = saturating_add_uint64(now, value->vibration.duration_ns);
	}

	if (send_trigger_feedback && trigger_feedback_mode != pssense->output.trigger_feedback_mode) {
		pssense->output.send_trigger_feedback = true;
		pssense->output.trigger_feedback_mode = trigger_feedback_mode;
	}
	os_thread_helper_unlock(&pssense->controller_thread);

	return XRT_SUCCESS;
}

xrt_result_t
pssense_get_output_limits(struct xrt_device *xdev, struct xrt_output_limits *limits)
{
	(*limits) = XRT_C11_COMPOUND(struct xrt_output_limits){
	    // PCM data is played back at 3000hz
	    .haptic_pcm_sample_rate = PCM_SAMPLE_RATE,
	};

	return XRT_SUCCESS;
}

static void
pssense_apply_synthetic_position(struct pssense_device *pssense, struct xrt_space_relation *out_relation)
{
	if (!pssense->synthetic_position || pssense->tracking.use_constellation) {
		return;
	}

	/*
	 * The PSVR2 builder already gives untracked left/right controller
	 * origins stable offsets (-/+0.2, 1.3, -0.5 m). The Sense driver has
	 * real 3DoF orientation but, without the constellation tracker, no
	 * optical position, so normally the position flags remain unset. Some
	 * OpenVR applications (notably older Alyx through OpenComposite) reject a
	 * controller pose unless position is also valid/tracked. In this opt-in
	 * compatibility mode, keep only the small grip/aim offsets as the
	 * device-local position so the builder's tracking-origin offset supplies
	 * the synthetic translation, and advertise it as tracked.
	 */
	out_relation->relation_flags =
	    (enum xrt_space_relation_flags)(out_relation->relation_flags | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	                                    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
}

static void
pssense_apply_synthetic_arm_model(struct pssense_device *pssense,
                                  int64_t at_timestamp_ns,
                                  struct xrt_space_relation *out_relation)
{
	if (!pssense->synthetic_position || !pssense->synthetic_arm_model || pssense->head_xdev == NULL ||
	    pssense->tracking.use_constellation) {
		return;
	}

	struct xrt_space_relation head_relation = XRT_SPACE_RELATION_ZERO;
	xrt_result_t xret = pssense->head_xdev->get_tracked_pose(pssense->head_xdev, XRT_INPUT_GENERIC_HEAD_POSE,
	                                                         at_timestamp_ns, &head_relation);
	if (xret != XRT_SUCCESS ||
	    (head_relation.relation_flags &
	     (XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)) !=
	        (XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_ORIENTATION_VALID_BIT)) {
		return;
	}

	/*
	 * Sense IMU yaw and PSVR2 SLAM yaw are independent. On the first valid
	 * arm-model query, align the controller's horizontal forward direction to
	 * the HMD's horizontal forward direction. Thereafter the alignment remains
	 * fixed so controller rotations continue to move naturally in room space.
	 */
	if (!pssense->orientation_alignment_initialized) {
		struct xrt_vec3 local_forward = {0.0f, 0.0f, -1.0f};
		struct xrt_vec3 controller_forward;
		struct xrt_vec3 head_forward;
		math_quat_rotate_vec3(&out_relation->pose.orientation, &local_forward, &controller_forward);
		math_quat_rotate_vec3(&head_relation.pose.orientation, &local_forward, &head_forward);
		controller_forward.y = 0.0f;
		head_forward.y = 0.0f;

		float controller_len =
		    sqrtf(controller_forward.x * controller_forward.x + controller_forward.z * controller_forward.z);
		float head_len = sqrtf(head_forward.x * head_forward.x + head_forward.z * head_forward.z);
		if (controller_len > 0.01f && head_len > 0.01f) {
			controller_forward.x /= controller_len;
			controller_forward.z /= controller_len;
			head_forward.x /= head_len;
			head_forward.z /= head_len;
			math_quat_from_vec_a_to_vec_b(&controller_forward, &head_forward,
			                              &pssense->orientation_alignment);
			pssense->orientation_alignment_initialized = true;
			PSSENSE_WARN(pssense, "synthetic arm model aligned to HMD/world yaw");
		}
	}

	if (pssense->orientation_alignment_initialized) {
		struct xrt_quat aligned_orientation;
		math_quat_rotate(&pssense->orientation_alignment, &out_relation->pose.orientation,
		                 &aligned_orientation);
		out_relation->pose.orientation = aligned_orientation;

		struct xrt_vec3 aligned_angvel;
		math_quat_rotate_vec3(&pssense->orientation_alignment, &out_relation->angular_velocity,
		                      &aligned_angvel);
		out_relation->angular_velocity = aligned_angvel;
	}

	/*
	 * Very small 3DoF arm model: put a virtual shoulder below and slightly
	 * behind each side of the HMD, then extend the hand along the controller's
	 * corrected forward direction. This gives useful positional movement from
	 * the real Sense orientation without pretending we have optical 6DoF.
	 */
	struct xrt_vec3 shoulder_local = {
	    pssense->hand == XRT_HAND_LEFT ? -0.18f : 0.18f,
	    -0.18f,
	    -0.05f,
	};
	struct xrt_vec3 shoulder_world;
	math_quat_rotate_vec3(&head_relation.pose.orientation, &shoulder_local, &shoulder_world);

	struct xrt_vec3 local_forward = {0.0f, 0.0f, -1.0f};
	struct xrt_vec3 hand_forward;
	math_quat_rotate_vec3(&out_relation->pose.orientation, &local_forward, &hand_forward);

	const float arm_length_m = 0.45f;
	out_relation->pose.position.x =
	    head_relation.pose.position.x + shoulder_world.x + hand_forward.x * arm_length_m;
	out_relation->pose.position.y =
	    head_relation.pose.position.y + shoulder_world.y + hand_forward.y * arm_length_m;
	out_relation->pose.position.z =
	    head_relation.pose.position.z + shoulder_world.z + hand_forward.z * arm_length_m;

	out_relation->linear_velocity = head_relation.linear_velocity;
	out_relation->relation_flags =
	    (enum xrt_space_relation_flags)(out_relation->relation_flags | XRT_SPACE_RELATION_POSITION_VALID_BIT |
	                                    XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
}

static struct xrt_quat
quat_from_x_rot(float x)
{
	return (struct xrt_quat){
	    .x = sinf(x * 0.5f),
	    .y = 0,
	    .z = 0,
	    .w = cosf(x * 0.5f),
	};
}

static xrt_result_t
pssense_get_tracked_pose(struct xrt_device *xdev,
                         enum xrt_input_name name,
                         int64_t at_timestamp_ns,
                         struct xrt_space_relation *out_relation)
{
	struct pssense_device *pssense = from_device(xdev);

	if (name != XRT_INPUT_PSSENSE_AIM_POSE && name != XRT_INPUT_PSSENSE_GRIP_POSE) {
		U_LOG_XDEV_UNSUPPORTED_INPUT(&pssense->base, pssense->log_level, name);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	struct xrt_relation_chain xrc = {0};
	struct xrt_pose pose_correction = XRT_POSE_IDENTITY;

	float sx = pssense->hand == XRT_HAND_LEFT ? 1.0f : -1.0f;
	struct xrt_pose T_steamvrroot_model = {
	    .orientation = quat_from_x_rot(DEG_TO_RAD(-39)),
	    .position = {.x = -sx * .01432f, .y = .007713f, .z = .10399f},
	};
	struct xrt_pose T_steamvrroot_xrgrip = {
	    .orientation = quat_from_x_rot(DEG_TO_RAD(20.6)),
	    .position = {.x = sx * .007, .y = -.00182941, .z = .1019482},
	};
	struct xrt_pose T_steamvrroot_xraim = {
	    .orientation = quat_from_x_rot(DEG_TO_RAD(-39.4)),
	    .position = {.x = sx * .007, .y = -.03894766, .z = .00949694},
	};

	switch (name) {
	case XRT_INPUT_PSSENSE_AIM_POSE: {
		m_relation_chain_push_pose(&xrc, &T_steamvrroot_xraim);
		break;
	}
	case XRT_INPUT_PSSENSE_GRIP_POSE: {
		m_relation_chain_push_pose(&xrc, &T_steamvrroot_xrgrip);
		break;
	}
	default: assert(!"Unreachable");
	}
	m_relation_chain_push_inverted_pose_if_not_identity(&xrc, &T_steamvrroot_model);

#if 0 // Presently unused OpenVR poses, but might be useful if we need to expose more stuff for st/openvr
	struct xrt_pose T_steamvrroot_xrhandmodel = {
	    .orientation = quat_from_x_rot(DEG_TO_RAD(39.4)),
	    .position = {.x = -sx * 11.25, .y = -1.82941, .z = 101.9482},
	};
	struct xrt_pose T_steamvrroot_handgrip = {
	    .orientation = quat_from_x_rot(DEG_TO_RAD(5.037)),
	    .position = {.x = 0, .y = 3, .z = 97},
	};
	struct xrt_pose T_steamvrroot_tip = {
	    .orientation = quat_from_x_rot(DEG_TO_RAD(37.4)),
	    .position = {.x = sx * 16.694, .y = -25.22, .z = 24.687},
	};
#endif

	// If we aren't using constellation tracking, rotate the IMU orientation so that it's facing the same direction
	// as the LED model is facing
	if (!pssense->tracking.use_constellation) {
		pose_correction.orientation = pssense->tracking.T_led_imu.orientation;
	}

	m_relation_chain_push_pose(&xrc, &pose_correction);

	struct xrt_space_relation *rel = m_relation_chain_reserve(&xrc);

	os_thread_helper_lock(&pssense->controller_thread);
	if (pssense->tracking.use_constellation) {
		pssense_get_constellation_pose(pssense, at_timestamp_ns, rel);
	} else {
		pssense_get_imu_fusion_pose(pssense, at_timestamp_ns, rel);
	}
	os_thread_helper_unlock(&pssense->controller_thread);

	m_relation_chain_resolve(&xrc, out_relation);
	pssense_apply_synthetic_position(pssense, out_relation);
	pssense_apply_synthetic_arm_model(pssense, at_timestamp_ns, out_relation);

	return XRT_SUCCESS;
}

static xrt_result_t
pssense_get_battery_status(struct xrt_device *xdev, bool *out_present, bool *out_charging, float *out_charge)
{
	struct pssense_device *pssense = from_device(xdev);

	if (!pssense->state.battery_state_valid) {
		*out_present = false;
		return XRT_SUCCESS;
	}

	*out_present = true;
	*out_charging = pssense->state.battery_charging;
	*out_charge = pssense->state.battery_charge_percent;

	return XRT_SUCCESS;
}

/*
 *
 * Exported functions
 *
 */

#define SET_INPUT(NAME) (pssense->base.inputs[PSSENSE_INDEX_##NAME].name = XRT_INPUT_PSSENSE_##NAME)

static struct xrt_device *
pssense_create_internal(struct os_hid_device *hid,
                        const char *product_name,
                        uint16_t product_id,
                        bool usb,
                        struct xrt_frame_context *xfctx,
                        struct t_timing_event_sink **out_timing_sink);

struct xrt_device *
pssense_create(struct xrt_prober *xp,
               struct xrt_prober_device *xpdev,
               struct xrt_frame_context *xfctx,
               struct t_timing_event_sink **out_timing_sink)
{
	struct os_hid_device *hid = NULL;
	int ret;

	// On USB, we need to use interface 2, rather than 0 like on Bluetooth.
	int iface = xpdev->bus == XRT_BUS_TYPE_USB ? 2 : 0;
	ret = xrt_prober_open_hid_interface(xp, xpdev, iface, &hid);
	if (ret != 0) {
		U_LOG_E("Failed to open HID interface for PlayStation Sense controller!");
		return NULL;
	}

	unsigned char product_name[128];
	ret = xrt_prober_get_string_descriptor( //
	    xp,                                 //
	    xpdev,                              //
	    XRT_PROBER_STRING_PRODUCT,          //
	    product_name,                       //
	    sizeof(product_name));              //
	if (ret <= 0) {
		U_LOG_E("Failed to get product name from Bluetooth device!");
		return NULL;
	}

	return pssense_create_internal(hid, (const char *)product_name, xpdev->product_id,
	                               xpdev->bus == XRT_BUS_TYPE_USB, xfctx, out_timing_sink);
}

bool
pssense_reconnect_requested(void)
{
#ifdef XRT_OS_OSX
	return debug_get_bool_option_pssense_reconnect();
#else
	return false;
#endif
}

struct xrt_device *
pssense_create_disconnected(uint16_t product_id,
                            struct xrt_frame_context *xfctx,
                            struct t_timing_event_sink **out_timing_sink)
{
	if (!pssense_reconnect_requested() || (product_id != PSSENSE_PID_LEFT && product_id != PSSENSE_PID_RIGHT)) {
		return NULL;
	}
	const char *name =
	    product_id == PSSENSE_PID_LEFT ? "PS VR2 Sense Controller (L)" : "PS VR2 Sense Controller (R)";
	return pssense_create_internal(NULL, name, product_id, false, xfctx, out_timing_sink);
}

static struct xrt_device *
pssense_create_internal(struct os_hid_device *hid,
                        const char *product_name,
                        uint16_t product_id,
                        bool usb,
                        struct xrt_frame_context *xfctx,
                        struct t_timing_event_sink **out_timing_sink)
{
	int ret;
	enum u_device_alloc_flags flags = U_DEVICE_ALLOC_TRACKING_NONE;
	struct pssense_device *pssense = U_DEVICE_ALLOCATE(struct pssense_device, flags, PSSENSE_INPUT_COUNT, 2);
	PSSENSE_DEBUG(pssense, "PlayStation Sense controller found");

	pssense->node.break_apart = pssense_node_break_apart;
	pssense->node.destroy = pssense_node_destroy;

	pssense->timing_event_sink.push_timing_event = pssense_timing_event_sink_push;

	pssense->constellation_device.push_constellation_tracker_sample =
	    pssense_push_constellation_tracker_sample_upstream;
	if (debug_get_bool_option_pssense_led_bootstrap() || debug_get_bool_option_pssense_filter() ||
	    debug_get_bool_option_pssense_joint()) {
		pssense->constellation_device.push_optical_sample = pssense_push_constellation_tracker_sample;
	}
	pssense->constellation_device.push_camera_blob_count = pssense_push_camera_blob_count;
	pssense->constellation_device.push_camera_led_blob_count = pssense_push_camera_led_blob_count;

	pssense->constellation_tracking_source.get_tracked_pose = pssense_get_constellation_tracking_source_pose;

	pssense->base.name = XRT_DEVICE_PSSENSE;
	snprintf(pssense->base.str, XRT_DEVICE_NAME_LEN, "%s", product_name);
	pssense->base.update_inputs = pssense_device_update_inputs;
	pssense->base.set_output = pssense_set_output;
	pssense->base.get_output_limits = pssense_get_output_limits;
	pssense->base.get_tracked_pose = pssense_get_tracked_pose;
	pssense->base.get_battery_status = pssense_get_battery_status;
	pssense->base.destroy = pssense_device_destroy;

	pssense->base.supported.orientation_tracking = true;
	pssense->base.supported.battery_status = true;
	pssense->base.supported.force_feedback = true;

	pssense->usb = usb;
	pssense->product_id = product_id;
	pssense->reconnect = pssense_reconnect_requested() && !usb;
	pssense->connected = hid != NULL;

	m_imu_3dof_init(&pssense->tracking.fusion, M_IMU_3DOF_USE_GRAVITY_DUR_20MS);
	pssense->tracking.gyro_bias.enabled = debug_get_bool_option_pssense_gyro_bias_auto();
	pssense->tracking.input_diag = debug_get_bool_option_pssense_input_diag();
	if (debug_get_bool_option_pssense_filter()) {
		struct t_imu_optical_filter_params filter_params;
		t_imu_optical_filter_default_params(&filter_params);
		pssense->tracking.filter = t_imu_optical_filter_create(&filter_params);
	}

	long timing_fudge_100us = debug_get_num_option_pssense_timing_fudge_100us();
	if (timing_fudge_100us == LONG_MIN) {
#ifdef XRT_OS_OSX
		timing_fudge_100us = debug_get_bool_option_pssense_future_led_schedule() ? 36 : 0;
#else
		timing_fudge_100us = 0;
#endif
	}
	pssense->tracking.timing_fudge_100us = (int32_t)CLAMP(timing_fudge_100us, INT32_MIN, INT32_MAX);
	pssense->tracking.increment_sequence_num = true;
	pssense->logged_battery_percent = -1.0f;
	pssense->timing.clock_tracker = m_clock_windowed_skew_tracker_alloc(2048);
	struct pssense_clock_options clock_options;
	pssense_clock_default_options(&clock_options);
	// Opt-in (PSSENSE_CLOCK_OFFSET_SNAP_US > 0): jump straight to the max-tracked offset on a large gap.
	clock_options.snap_ns = (double)debug_get_num_option_pssense_clock_offset_snap_us() * 1000.0;
	clock_options.steady = debug_get_bool_option_pssense_clock_steady();
	pssense_clock_init(&pssense->timing.clock, &clock_options);
	pssense_led_sweep_parse(pssense);

	m_relation_history_create(&pssense->tracking.imu_relation_history);
	m_relation_history_create(&pssense->tracking.constellation_relation_history);

	pssense->log_level = debug_get_log_option_pssense_log();
	pssense->synthetic_position = debug_get_bool_option_pssense_synthetic_position();
	pssense->synthetic_arm_model =
	    pssense->synthetic_position && debug_get_bool_option_pssense_synthetic_arm_model();
	pssense->input_diagnostics = debug_get_bool_option_pssense_input_diagnostics();
	pssense->base.supported.position_tracking = pssense->synthetic_position;
	bool index_profile = debug_get_bool_option_pssense_index_profile();
	pssense->hid = hid;

	if (pssense->synthetic_position) {
		PSSENSE_WARN(pssense,
		             "PSSENSE_SYNTHETIC_POSITION enabled: reporting a valid/tracked synthetic position");
	}
	if (pssense->synthetic_arm_model) {
		PSSENSE_WARN(pssense,
		             "PSSENSE_SYNTHETIC_ARM_MODEL enabled: hand position follows HMD and Sense orientation");
	}

	if (index_profile) {
		PSSENSE_WARN(pssense,
		             "PSSENSE_INDEX_PROFILE enabled: advertising Valve Index bindings backed by Sense inputs");
	}

	// Initialize the IMU orientation to be correct
	struct xrt_quat imu_orientation_quat = quat_from_x_rot(pssense_imu_angle);

	if (product_id == PSSENSE_PID_LEFT) {
		pssense->base.device_type = XRT_DEVICE_TYPE_LEFT_HAND_CONTROLLER;
		pssense->hand = XRT_HAND_LEFT;
		pssense->base.binding_profiles = binding_profiles_pssense_left;
		pssense->base.binding_profile_count =
		    ARRAY_SIZE(binding_profiles_pssense_left) - (index_profile ? 0 : 1);

		pssense->led_model.match_parameters = DEFAULT_MATCH_PARAMETERS;
		pssense->led_model.leds = pssense_left_leds;
		pssense->led_model.led_count = ARRAY_SIZE(pssense_left_leds);

		pssense->tracking.T_led_imu = (struct xrt_pose){
		    .orientation = imu_orientation_quat,
		    .position = T_led_imu_left,
		};
	} else if (product_id == PSSENSE_PID_RIGHT) {
		pssense->base.device_type = XRT_DEVICE_TYPE_RIGHT_HAND_CONTROLLER;
		pssense->hand = XRT_HAND_RIGHT;
		pssense->base.binding_profiles = binding_profiles_pssense_right;
		pssense->base.binding_profile_count =
		    ARRAY_SIZE(binding_profiles_pssense_right) - (index_profile ? 0 : 1);

		pssense->led_model.match_parameters = DEFAULT_MATCH_PARAMETERS;
		pssense->led_model.leds = pssense_right_leds;
		pssense->led_model.led_count = ARRAY_SIZE(pssense_right_leds);

		pssense->tracking.T_led_imu = (struct xrt_pose){
		    .orientation = imu_orientation_quat,
		    .position = T_led_imu_right,
		};
	} else {
		PSSENSE_ERROR(pssense, "Unable to determine controller type");
		pssense_device_destroy(&pssense->base);
		return NULL;
	}

	pssense->led_model.unique_blob_matches = debug_get_bool_option_pssense_led_bootstrap() ||
	                                         debug_get_bool_option_pssense_filter() ||
	                                         debug_get_bool_option_pssense_joint();

	if (debug_get_bool_option_pssense_led_correction()) {
		const struct xrt_vec3 *corrections =
		    pssense->hand == XRT_HAND_LEFT ? pssense_left_led_corrections : pssense_right_led_corrections;
		for (size_t i = 0; i < pssense->led_model.led_count; i++) {
			pssense->corrected_leds[i] = pssense->led_model.leds[i];
			math_vec3_accum(&corrections[i], &pssense->corrected_leds[i].position);
		}
		pssense->led_model.leds = pssense->corrected_leds;
		PSSENSE_INFO(pssense, "LED_CORRECTION side=%c leds=%zu", pssense->hand == XRT_HAND_LEFT ? 'L' : 'R',
		             pssense->led_model.led_count);
	}

	SET_INPUT(PS_CLICK);
	SET_INPUT(SHARE_CLICK);
	SET_INPUT(OPTIONS_CLICK);
	SET_INPUT(SQUARE_CLICK);
	SET_INPUT(SQUARE_TOUCH);
	SET_INPUT(TRIANGLE_CLICK);
	SET_INPUT(TRIANGLE_TOUCH);
	SET_INPUT(CROSS_CLICK);
	SET_INPUT(CROSS_TOUCH);
	SET_INPUT(CIRCLE_CLICK);
	SET_INPUT(CIRCLE_TOUCH);
	SET_INPUT(SQUEEZE_CLICK);
	SET_INPUT(SQUEEZE_TOUCH);
	SET_INPUT(SQUEEZE_PROXIMITY);
	SET_INPUT(SQUEEZE_PROXIMITY_FLOAT);
	SET_INPUT(TRIGGER_CLICK);
	SET_INPUT(TRIGGER_TOUCH);
	SET_INPUT(TRIGGER_VALUE);
	SET_INPUT(TRIGGER_PROXIMITY);
	SET_INPUT(TRIGGER_PROXIMITY_FLOAT);
	SET_INPUT(THUMBSTICK);
	SET_INPUT(THUMBSTICK_CLICK);
	SET_INPUT(THUMBSTICK_TOUCH);
	SET_INPUT(GRIP_POSE);
	SET_INPUT(AIM_POSE);

	// A controller created before it connects (PSSENSE_RECONNECT) reports inactive inputs until it does.
	for (uint32_t i = 0; i < ((uint32_t)PSSENSE_INPUT_COUNT); i++) {
		pssense->base.inputs[i].active = pssense->connected;
	}

	pssense->base.outputs[0].name = XRT_OUTPUT_NAME_PSSENSE_VIBRATION;
	pssense->base.outputs[1].name = XRT_OUTPUT_NAME_PSSENSE_TRIGGER_FEEDBACK;

	os_precise_sleeper_init(&pssense->sleeper);

	pssense->output.pcm_haptics_resampler = u_resampler_create(4000, PCM_SAMPLE_RATE);
	if (pssense->output.pcm_haptics_resampler == NULL) {
		PSSENSE_ERROR(pssense, "Failed to create PCM resampler");
		pssense_device_destroy(&pssense->base);
		return NULL;
	}

	// @note We don't do blink duration refinement right now because that needs to eventually adjust the latency
	//       offset as it goes and produces a worse result with the current implementation.
	struct t_led_sync_refinement_options led_sync_refinement_options = {
	    // @todo Once LED blink refinement is fixed, enable that again
	    // @todo Once optical clock sync is implemented in full, enable that here
	    .flags = T_LED_SYNC_REFINEMENT_FLAGS_HAS_LATENCY_CAP,
	    .initial_blink_duration_ns = PERIOD_ID_TO_DURATION_NS(30),
	    .min_blink_duration_ns = PERIOD_ID_TO_DURATION_NS(1),
	    .max_blink_duration_ns = PERIOD_ID_TO_DURATION_NS(MAX_PERIOD_ID),
	    .time_to_resync_ns = T_LED_SYNC_DEFAULT_RESYNC_TIME,
	    .settle_frames = 7,
	    // 8ms latency cap, something a bit overboard for bluetooth but better than searching the whole range
	    .latency_cap_ns = U_TIME_1MS_IN_NS * 8LL,
	};
	ret = t_led_sync_refinement_init(&pssense->tracking.led_sync_refinement, &led_sync_refinement_options);
	if (ret != 0) {
		PSSENSE_ERROR(pssense, "Failed to init LED sync refinement!");
		pssense_device_destroy(&pssense->base);
		return NULL;
	}

	pssense->tracking.period_id = DURATION_NS_TO_PERIOD_ID(led_sync_refinement_options.initial_blink_duration_ns);

	pssense->tracking.use_led_bootstrap = debug_get_bool_option_pssense_led_bootstrap();
	{
		struct t_led_phase_bootstrap_options bootstrap_options;
		t_led_phase_bootstrap_default_options(&bootstrap_options);
		bootstrap_options.log_level = pssense->log_level;
		bootstrap_options.label = pssense->hand == XRT_HAND_LEFT ? 'L' : 'R';
		long wide_period_id = debug_get_num_option_pssense_led_bootstrap_wide_period_id();
		wide_period_id = wide_period_id > 0 ? CLAMP(wide_period_id, 1, MAX_PERIOD_ID) : MAX_PERIOD_ID;
		bootstrap_options.wide_blink_ns = PERIOD_ID_TO_DURATION_NS(wide_period_id);
		bootstrap_options.narrow_blink_ns = PERIOD_ID_TO_DURATION_NS(9);
		long lock_period_id = debug_get_num_option_pssense_led_bootstrap_lock_period_id();
		lock_period_id = CLAMP(lock_period_id, 1, MAX_PERIOD_ID);
		bootstrap_options.lock_blink_ns = PERIOD_ID_TO_DURATION_NS(lock_period_id);
		if (debug_get_bool_option_pssense_led_bootstrap_track()) {
			long frames = debug_get_num_option_pssense_led_bootstrap_track_frames();
			bootstrap_options.track_interval_frames = frames > 0 ? (uint32_t)frames : 120;
		}
		pssense->tracking.led_bootstrap_led_blobs = debug_get_bool_option_pssense_led_bootstrap_led_blobs();
		pssense->tracking.led_bootstrap_strict = debug_get_bool_option_pssense_led_bootstrap_strict();
		if (debug_get_bool_option_pssense_led_bootstrap_strict()) {
			bootstrap_options.min_lock_peak_score = 2.0f;
			// One probe moves the lock at most 200 us: a noisy probe cannot take a centred lock (+-700 us)
			// out of its window, and three consistent probes still follow ~60 us/s.
			bootstrap_options.track_max_step_ns = 200 * U_TIME_1US_IN_NS;
			// Needs the joint tracker's per-device matched counts (push_camera_led_blob_count).
			bootstrap_options.detect_stuck_lit = true;
			// Healthy windows span at most 7 narrow steps (1950 us); a run lit to the scan's end beyond
			// that is checked dark before locking (5 Oct: 8 steps, the ring stuck lit mid-scan).
			bootstrap_options.stuck_check_unbounded_steps = 8;
			/*
			 * Retry a failed hinted scan (1, 2, 4, 8 s apart) before the full scan. On 4 Oct (000909) two
			 * hinted scans failed with the left out of view, the full scan followed, and its long wide
			 * pulses were followed by the always-lit fault, as at every onset so far. Lock centres have
			 * stayed inside the hint's +-1.5 ms.
			 */
			bootstrap_options.hint_retries = 4;
			// LED-shaped counts are nearly background-free, but average over every camera: a ring three of
			// four cameras saw added 2.9 per camera on 25 Sep. Raw counts need more margin over their
			// noise.
			bootstrap_options.track_min_ring_blobs =
			    pssense->tracking.led_bootstrap_led_blobs ? 1.5f : 3.0f;
		}
		bootstrap_options.track_use_pose_coverage =
		    debug_get_bool_option_pssense_led_bootstrap_track_coverage();
		/*
		 * Opt-in (PSSENSE_LED_BOOTSTRAP_BLOB_FALLBACK, and only with LED-shaped counts, as raw counts include
		 * the other ring and background light): steer an untracked probe by blob counts. On 5 Oct such probes
		 * moved a healthy left lock +400 us on counts like 0.50/2.12/6.12, from a moving hand rather than a
		 * slid window; it was then lit only at the window's centre. A lock that has really slid is now
		 * rescanned instead (lost_lit_fraction).
		 */
		bootstrap_options.track_blob_fallback = pssense->tracking.led_bootstrap_led_blobs &&
		                                        debug_get_bool_option_pssense_led_bootstrap_blob_fallback();
		long lost_lit_percent = debug_get_num_option_pssense_led_bootstrap_lost_lit_percent();
		bootstrap_options.lost_lit_fraction = (float)CLAMP(lost_lit_percent, 0, 100) / 100.0f;
		// At most one dim rescan per 30 s (1800 exposures): scans have preceded the always-lit fault.
		bootstrap_options.dim_rescan_cooldown_frames = 1800;
		long hint_us = debug_get_num_option_pssense_led_bootstrap_hint_us();
		if (hint_us >= 0) {
			// The hint is a narrow-pulse start offset, like the scan steps: centre minus half the narrow
			// pulse.
			bootstrap_options.hint_fudge_ns =
			    (time_duration_ns)hint_us * U_TIME_1US_IN_NS - bootstrap_options.narrow_blink_ns / 2;
			/*
			 * With a hint, never fall back to the full scan: retry hinted scans with backoff. Every
			 * always-lit fault on 5 Oct began on entering a full scan or in a burst of scans, and lock
			 * centres have always stayed within the hint's +-1.5 ms.
			 * PSSENSE_LED_BOOTSTRAP_FULL_SCAN_FALLBACK=1 restores it.
			 */
			bootstrap_options.full_scan_fallback =
			    debug_get_bool_option_pssense_led_bootstrap_full_scan_fallback();
			bootstrap_options.quick_lock = debug_get_bool_option_pssense_led_bootstrap_quick_lock();
			// After a failed scan, a quick check every ~0.5 s idle (about 1.5 s a cycle with the baseline).
			bootstrap_options.quick_retry_frames = bootstrap_options.quick_lock ? 30 : 0;
		}
		t_led_phase_bootstrap_init(&pssense->tracking.led_bootstrap, &bootstrap_options);
		// Force the first update to program the bootstrap's output, replacing any refinement sample.
		pssense->tracking.led_bootstrap_programmed_generation = UINT32_MAX;
	}
	if (pssense->tracking.use_led_bootstrap) {
		PSSENSE_INFO(pssense, "LED phase bootstrap enabled (replaces pose-driven LED sync refinement)%s%s%s",
		             pssense->tracking.led_bootstrap_led_blobs ? ", LED-shaped per-controller blob counts" : "",
		             debug_get_bool_option_pssense_led_bootstrap_strict() ? ", strict" : "",
		             debug_get_bool_option_pssense_led_bootstrap_track_coverage() ? ", coverage probes" : "");
	}

	ret = os_thread_helper_init(&pssense->controller_thread);
	if (ret != 0) {
		PSSENSE_ERROR(pssense, "Failed to init threading!");
		pssense_device_destroy(&pssense->base);
		return NULL;
	}

	ret = os_thread_helper_start(&pssense->controller_thread, pssense_run_thread, pssense);
	if (ret != 0) {
		PSSENSE_ERROR(pssense, "Failed to start thread!");
		pssense_device_destroy(&pssense->base);
		return NULL;
	}

	// Try to set the PC polling rate if the user requested it. Without a connection yet, the thread does this and
	// reads the calibration once the controller connects.
	if (hid != NULL && debug_get_bool_option_pssense_pc_polling_rate() && //
	    !pssense_set_pc_polling_rate(pssense)) {
		PSSENSE_ERROR(pssense, "PC polling rate requested, but got error when attempting to apply.");
	}

	pssense_set_connected_bit(pssense, hid != NULL);
	if (hid == NULL) {
		PSSENSE_WARN(pssense, "CONNECTION side=%c event=waiting: created before the controller connected",
		             pssense->hand == XRT_HAND_LEFT ? 'L' : 'R');
	} else if (!pssense_get_calibration_data(pssense)) {
		PSSENSE_ERROR(pssense, "Failed to retrieve calibration data");
		pssense_device_destroy(&pssense->base);
		return NULL;
	}

	u_var_add_root(pssense, pssense->base.str, false);
	u_var_add_log_level(pssense, &pssense->log_level, "Log level");

	u_var_add_gui_header(pssense, &pssense->gui.button_states, "Button States");
	u_var_add_bool(pssense, &pssense->state.ps_click, "PS Click");
	if (pssense->hand == XRT_HAND_LEFT) {
		u_var_add_bool(pssense, &pssense->state.share_click, "Share Click");
		u_var_add_bool(pssense, &pssense->state.square_click, "Square Click");
		u_var_add_bool(pssense, &pssense->state.square_touch, "Square Touch");
		u_var_add_bool(pssense, &pssense->state.triangle_click, "Triangle Click");
		u_var_add_bool(pssense, &pssense->state.triangle_touch, "Triangle Touch");
	} else if (pssense->hand == XRT_HAND_RIGHT) {
		u_var_add_bool(pssense, &pssense->state.options_click, "Options Click");
		u_var_add_bool(pssense, &pssense->state.cross_click, "Cross Click");
		u_var_add_bool(pssense, &pssense->state.cross_touch, "Cross Touch");
		u_var_add_bool(pssense, &pssense->state.circle_click, "Circle Click");
		u_var_add_bool(pssense, &pssense->state.circle_touch, "Circle Touch");
	}
	u_var_add_bool(pssense, &pssense->state.squeeze_click, "Squeeze Click");
	u_var_add_bool(pssense, &pssense->state.squeeze_touch, "Squeeze Touch");
	u_var_add_ro_f32(pssense, &pssense->state.squeeze_proximity, "Squeeze Proximity");
	u_var_add_bool(pssense, &pssense->state.trigger_click, "Trigger Click");
	u_var_add_bool(pssense, &pssense->state.trigger_touch, "Trigger Touch");
	u_var_add_ro_f32(pssense, &pssense->state.trigger_value, "Trigger");
	u_var_add_ro_f32(pssense, &pssense->state.trigger_proximity, "Trigger Proximity");
	u_var_add_ro_f32(pssense, &pssense->state.thumbstick.x, "Thumbstick X");
	u_var_add_ro_f32(pssense, &pssense->state.thumbstick.y, "Thumbstick Y");
	u_var_add_bool(pssense, &pssense->state.thumbstick_click, "Thumbstick Click");
	u_var_add_bool(pssense, &pssense->state.thumbstick_touch, "Thumbstick Touch");

	u_var_add_gui_header(pssense, &pssense->gui.timing, "Timing");
	u_var_add_ro_i64_ns(pssense, &pssense->timing.latest_imu_time_ns, "Latest IMU Time (ns)");
	u_var_add_ro_u64(pssense, &pssense->timing.imu_ticks_total, "Latest IMU Time (ticks)");
	u_var_add_ro_i64_ns(pssense, &pssense->timing.latest_device_time_ns, "Latest Device Time (ns)");
	u_var_add_ro_u64(pssense, &pssense->timing.device_ticks_total, "Latest Device Time (ticks)");

	u_var_add_gui_header(pssense, &pssense->gui.tracking, "Tracking");
	u_var_add_ro_vec3_i32(pssense, &pssense->state.gyro_raw, "Raw Gyro");
	u_var_add_ro_vec3_i32(pssense, &pssense->state.accel_raw, "Raw Accel");
	u_var_add_bool(pssense, &pssense->has_calibration, "Has Calibration");
	u_var_add_ro_vec3_i32(pssense, &pssense->calibration.gyro_bias, "Gyro Bias");
	u_var_add_ro_vec3_f32(pssense, &pssense->calibration.gyro_scale, "Gyro Scale");
	u_var_add_ro_vec3_f32(pssense, &pssense->calibration.accel_bias, "Accel Bias");
	u_var_add_ro_vec3_f32(pssense, &pssense->calibration.accel_scale, "Accel Scale");
	u_var_add_pose(pssense, &pssense->tracking.pose, "Pose");
	m_imu_3dof_add_vars(&pssense->tracking.fusion, pssense, "3dof Fusion");
	u_var_add_ro_u32(pssense, &pssense->tracking.received_frames, "Received Frames");
	u_var_add_ro_u32(pssense, &pssense->tracking.last_exposure_sequence_id, "Last Exposure Sequence ID");
	u_var_add_ro_i64_ns(pssense, &pssense->tracking.last_exposure_local_timestamp_ns,
	                    "Last Exposure Timestamp (ns)");
	u_var_add_ro_i64_ns(pssense, &pssense->tracking.average_exposure_interval_ns, "Average Exposure Interval (ns)");
	u_var_add_bool(pssense, &pssense->tracking.increment_sequence_num, "Increment LED Sequence Number");
	u_var_add_u8(pssense, &pssense->tracking.led_sequence_num, "LED Sequence Number");
	u_var_add_u8(pssense, &pssense->tracking.period_id, "LED Blink Period ID");
	u_var_add_i32(pssense, &pssense->tracking.timing_fudge_100us, "Timing Fudge (100us)");
	u_var_add_bool(pssense, &pssense->tracking.use_constellation, "Use Constellation Tracking");

	xrt_frame_context_add(xfctx, &pssense->node);

	if (out_timing_sink != NULL) {
		*out_timing_sink = &pssense->timing_event_sink;
	}

	return &pssense->base;
}

void
pssense_set_head_device(struct xrt_device *controller, struct xrt_device *head)
{
	if (controller == NULL) {
		return;
	}

	struct pssense_device *pssense = (struct pssense_device *)controller;
	pssense->head_xdev = head;

	/*
	 * When the arm model returns positions in the HMD's world coordinates, do
	 * not let the generic builder add the old fixed (-/+0.2, 1.3, -0.5) origin.
	 */
	if (pssense->synthetic_position && pssense->synthetic_arm_model && controller->tracking_origin != NULL &&
	    head != NULL && head->tracking_origin != NULL) {
		controller->tracking_origin->type = head->tracking_origin->type;
		controller->tracking_origin->initial_offset = (struct xrt_pose)XRT_POSE_IDENTITY;
		snprintf(controller->tracking_origin->name, XRT_TRACKING_NAME_LEN, "PS VR2 synthetic Sense tracking");
	}
}

int
pssense_add_to_constellation_tracker(struct xrt_device *xdev, struct t_constellation_tracker *tracker)
{
	struct pssense_device *pssense = from_device(xdev);
	if (pssense->tracking.constellation_tracker != NULL) {
		PSSENSE_ERROR(pssense, "Controller is already attached to a constellation tracker");
		return -1;
	}

	struct t_constellation_tracker_device_params params = {
	    .led_model = pssense->led_model,
	    .tracking_source = &pssense->constellation_tracking_source,
	};
	int ret = t_constellation_tracker_add_device(tracker, &params, &pssense->constellation_device,
	                                             &pssense->tracking.constellation_device_id);
	if (ret < 0) {
		PSSENSE_ERROR(pssense, "Failed to add device to constellation tracker: %d", ret);
		return -1;
	}

	os_thread_helper_lock(&pssense->controller_thread);
	pssense->tracking.constellation_imu_sink = params.imu_sink;
	pssense->tracking.constellation_tracker = tracker;
	pssense->tracking.use_constellation = true;
	pssense->base.supported.position_tracking = true;
	os_thread_helper_unlock(&pssense->controller_thread);

	pssense->tracking.tracking_origin_before_constellation = pssense->base.tracking_origin;
	pssense->base.tracking_origin = t_constellation_tracker_get_tracking_origin(tracker);

	return 0;
}

void
pssense_remove_from_constellation_tracker(struct xrt_device *xdev)
{
	struct pssense_device *pssense = from_device(xdev);
	struct t_constellation_tracker *tracker = NULL;
	t_constellation_device_id_t device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID;

	os_thread_helper_lock(&pssense->controller_thread);
	tracker = pssense->tracking.constellation_tracker;
	if (tracker == NULL) {
		os_thread_helper_unlock(&pssense->controller_thread);
		return;
	}
	device_id = pssense->tracking.constellation_device_id;
	pssense->tracking.constellation_imu_sink = NULL;
	pssense->tracking.constellation_tracker = NULL;
	pssense->tracking.constellation_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID;
	pssense->tracking.use_constellation = false;
	pssense->base.supported.position_tracking = pssense->synthetic_position;
	t_led_phase_bootstrap_stop(&pssense->tracking.led_bootstrap);
	pssense_led_bootstrap_release(pssense, 0);
	os_thread_helper_unlock(&pssense->controller_thread);
	pssense->base.tracking_origin = pssense->tracking.tracking_origin_before_constellation;
	pssense->tracking.tracking_origin_before_constellation = NULL;

	if (tracker != NULL && device_id != XRT_CONSTELLATION_INVALID_DEVICE_ID) {
		(void)t_constellation_tracker_remove_device(tracker, device_id);
	}
}

bool
pssense_get_constellation_diagnostics(struct xrt_device *xdev,
                                      struct pssense_constellation_diagnostics *out_diagnostics)
{
	if (xdev == NULL || xdev->name != XRT_DEVICE_PSSENSE || out_diagnostics == NULL) {
		return false;
	}
	struct pssense_device *pssense = from_device(xdev);
	os_thread_helper_lock(&pssense->controller_thread);
	*out_diagnostics = (struct pssense_constellation_diagnostics){
	    .attached = pssense->tracking.constellation_tracker != NULL,
	    .device_id = pssense->tracking.constellation_device_id,
	    .candidate_count = pssense->tracking.candidate_count,
	    .fused_pose_count = pssense->tracking.fused_pose_count,
	    .disagreement_count = pssense->tracking.disagreement_count,
	    .jump_rejection_count = pssense->tracking.jump_rejection_count,
	    .last_fused_timestamp_ns = pssense->tracking.last_fused_timestamp_ns,
	    .last_fused_camera_count = pssense->tracking.last_fused_camera_count,
	    .led_bootstrap_enabled = pssense->tracking.use_led_bootstrap,
	    .led_bootstrap_state = (uint32_t)pssense->tracking.led_bootstrap.state,
	    .led_bootstrap_fudge_ns = pssense->tracking.led_bootstrap.fudge_offset_ns,
	    .led_bootstrap_pulse_ns = pssense->tracking.led_bootstrap.blink_ns,
	    .led_bootstrap_scans = pssense->tracking.led_bootstrap.scans_attempted,
	    .led_bootstrap_locks = pssense->tracking.led_bootstrap.locks_acquired,
	};
	memcpy(out_diagnostics->camera_candidate_count, pssense->tracking.camera_candidate_count,
	       sizeof(out_diagnostics->camera_candidate_count));
	os_thread_helper_unlock(&pssense->controller_thread);
	return true;
}

/*!
 * @}
 */

// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header defining the constellation tracker parameters and functions.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_tracking.h"

#include "tracking/t_constellation.h"

#include "tracking/t_tracking.h"


#ifdef __cplusplus
extern "C" {
#endif

#define XRT_CONSTELLATION_MAX_TRACKING_MOSAICS (1)

struct t_constellation_tracker;

/*!
 * @public @memberof t_constellation_tracker

 * A constellation tracker camera is a single camera that the constellation tracker will use to track devices. The
 * constellation tracker will provide the blob sink for this camera.
 */
struct t_constellation_tracker_camera
{
	//! The calibration for this camera
	struct t_camera_calibration calibration;

	//! The position of this camera, in the mosaic's tracking origin.
	struct xrt_pose pose_in_origin;
	/*
	 * Whether this camera has a concrete pose in the tracking origin, or if we don't know the current position and
	 * need to compute it.
	 */
	bool has_concrete_pose;

	/*!
	 * The blob sink for this camera, this is an out parameter filled in by the constellation tracker, and you are
	 * expected to pass this to your blobwatch implementation.
	 */
	struct t_blob_sink *blob_sink;
};

/*!
 * @public @memberof t_constellation_tracker
 *
 * A constellation tracker camera mosaic is a set of cameras that may or may not be physically attached, but
 * importantly, they all fire at the same time and are synchronized with each other.
 */
struct t_constellation_tracker_camera_mosaic
{
	/*
	 * The constellation tracking source for this mosaic, if any. This is used as the origin of any cameras in the
	 * mosaic.
	 */
	struct t_constellation_tracker_tracking_source *tracking_origin;

	//! The cameras in this mosaic, with their blob sinks filled in by the constellation tracker.
	struct t_constellation_tracker_camera cameras[XRT_TRACKING_MAX_CAMS];
	//! The number of cameras in this mosaic.
	size_t num_cameras;
};

/*!
 * @public @memberof t_constellation_tracker
 *
 * Parameters for adding a device to the constellation tracker.
 */
struct t_constellation_tracker_device_params
{
	//! The constellation pattern for this device.
	struct t_constellation_tracker_led_model led_model;

	/*
	 * An optional tracking source to give the constellation tracker extra information to better throw out bad
	 * guesses when finding the device.
	 */
	struct t_constellation_tracker_tracking_source *tracking_source;

	/*!
	 * The IMU sink for this device. This pointer is filled in by the constellation tracker. After adding the device
	 * to the constellation tracker, drivers should send IMU samples to this sink.
	 */
	struct xrt_imu_sink *imu_sink;
};

enum t_constellation_tracker_flags
{
	T_CONSTELLATION_TRACKER_FLAGS_NONE = 0,
	/*!
	 * Whether the constellation tracker should be deterministic.
	 * Turn this on if you require consistent results, at the cost of performance.
	 */
	T_CONSTELLATION_TRACKER_FLAGS_DETERMINISTIC = 1 << 0,
};

/*!
 * @public @memberof t_constellation_tracker
 */
struct t_constellation_tracker_params
{
	//! Flags for the constellation tracker, see t_constellation_tracker_flags
	enum t_constellation_tracker_flags flags;
	struct t_constellation_tracker_camera_mosaic mosaics[XRT_CONSTELLATION_MAX_TRACKING_MOSAICS];
	size_t num_mosaics;
};

/*!
 * @public @memberof t_constellation_tracker
 */
int
t_constellation_tracker_create(struct xrt_frame_context *xfctx,
                               struct t_constellation_tracker_params *params,
                               struct t_constellation_tracker **out_tracker);

int
t_constellation_tracker_add_device(struct t_constellation_tracker *tracker,
                                   struct t_constellation_tracker_device_params *params,
                                   struct t_constellation_tracker_device *device,
                                   t_constellation_device_id_t *out_device_id);

int
t_constellation_tracker_remove_device(struct t_constellation_tracker *tracker, t_constellation_device_id_t device);

struct xrt_tracking_origin *
t_constellation_tracker_get_tracking_origin(struct t_constellation_tracker *tracker);


/*
 *
 * Dataset extension records. Each call does nothing unless the tracker is recording a dataset
 * (CONSTELLATION_TRACKER_DATA_RECORDER_OUTPUT), so drivers can call them unconditionally.
 *
 */

//! What a recorded @ref t_constellation_tracker_record_sync_event means, and what its three values hold.
enum t_constellation_sync_event_kind
{
	//! The device-to-host clock offset jumped. value[0]: the jump, ns.
	T_CONSTELLATION_SYNC_EVENT_CLOCK_SNAP = 1,
	//! A new online gyro bias was adopted. value: the bias, rad/s, IMU frame.
	T_CONSTELLATION_SYNC_EVENT_GYRO_BIAS = 2,
	//! The LED phase was locked to the cameras. value[0]: phase fudge, us; value[1]: pulse, us.
	T_CONSTELLATION_SYNC_EVENT_LED_LOCK = 3,
	//! The LED phase lock was lost.
	T_CONSTELLATION_SYNC_EVENT_LED_LOST = 4,
	//! An LED phase scan started.
	T_CONSTELLATION_SYNC_EVENT_LED_SCAN = 5,
	//! The scheduled LED phase moved without a full rescan. value[0]: the move, us.
	T_CONSTELLATION_SYNC_EVENT_LED_PHASE_MOVE = 6,
};

//! How a recorded head pose was obtained.
enum t_constellation_head_pose_source_flags
{
	//! Interpolated between SLAM poses; otherwise predicted past the newest one.
	T_CONSTELLATION_HEAD_POSE_INTERPOLATED = 1u << 0,
	//! The head pose is exact, for example from a simulation.
	T_CONSTELLATION_HEAD_POSE_EXACT = 1u << 1,
};

//! What a recorded ground-truth pose is.
enum t_constellation_ground_truth_flags
{
	//! The pose comes from a simulation and is exact.
	T_CONSTELLATION_GROUND_TRUTH_SYNTHETIC = 1u << 0,
	//! Only the orientation is known.
	T_CONSTELLATION_GROUND_TRUTH_ORIENTATION_ONLY = 1u << 1,
};

//! Store a UTF-8 JSON description of the recording session.
void
t_constellation_tracker_record_session_info(struct t_constellation_tracker *tracker, const char *json);

void
t_constellation_tracker_record_sync_event(struct t_constellation_tracker *tracker,
                                          t_constellation_device_id_t device_id,
                                          int64_t host_ns,
                                          enum t_constellation_sync_event_kind kind,
                                          const double value[3]);

/*!
 * Record how the IMU sample with host time @p host_ns, pushed to the device's IMU sink, was timed.
 *
 * @param device_ns         The sample's own device time, unwrapped.
 * @param clock_offset_ns   The device-to-host offset used: host = device - offset.
 * @param applied_gyro_bias The online gyro bias already subtracted from the pushed gyro, rad/s; may be NULL.
 */
void
t_constellation_tracker_record_imu_timing(struct t_constellation_tracker *tracker,
                                          t_constellation_device_id_t device_id,
                                          int64_t host_ns,
                                          int64_t device_ns,
                                          double clock_offset_ns,
                                          const double applied_gyro_bias[3]);

/*!
 * Record the head pose a tracking-origin query returned for @p timestamp_ns.
 *
 * @param source_ns    Host time of the newest SLAM pose it was derived from.
 * @param source_flags @ref t_constellation_head_pose_source_flags
 */
void
t_constellation_tracker_record_head_pose(struct t_constellation_tracker *tracker,
                                         int64_t timestamp_ns,
                                         const struct xrt_space_relation *Txr_world_head,
                                         int64_t source_ns,
                                         uint32_t source_flags);

//! Record an externally known device pose, in the world the camera poses use (OpenXR convention).
void
t_constellation_tracker_record_ground_truth(struct t_constellation_tracker *tracker,
                                            t_constellation_device_id_t device_id,
                                            int64_t timestamp_ns,
                                            const struct xrt_pose *Txr_world_device,
                                            float position_sigma_m,
                                            float orientation_sigma_rad,
                                            uint32_t flags);

//! Record a free-text marker; @p device_id may be XRT_CONSTELLATION_INVALID_DEVICE_ID.
void
t_constellation_tracker_record_annotation(struct t_constellation_tracker *tracker,
                                          t_constellation_device_id_t device_id,
                                          int64_t host_ns,
                                          const char *text);

#ifdef __cplusplus
}
#endif

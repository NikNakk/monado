// Copyright 2020-2021, Collabora, Ltd.
// Copyright 2023, Jan Schmidt
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PSVR2 HMD device
 *
 * @author Jan Schmidt <jan@centricular.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup drv_psvr2
 */

#pragma once

#include "xrt/xrt_prober.h"
#include "xrt/xrt_frame.h"
#include "tracking/t_time_sync.h"
#include <stdbool.h>
#include <stdint.h>

struct xrt_frame_sink;
struct psvr2_tracking_share;

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __APPLE__
/* macOS experiment: install/detach a single producer target. Detach waits for
 * in-flight publication under data_lock; caller owns mapping lifetime. */
bool
psvr2_set_tracking_share(struct xrt_device *xdev, struct psvr2_tracking_share *share);
#endif

/*!
 * @defgroup drv_psvr2 PS VR2 HMD driver
 * @ingroup drv
 *
 * @brief Driver for the Playstation VR2 headset
 *
 */

#define PSVR2_VID 0x054C
#define PSVR2_PID 0x0CDE

/*!
 * Minimal timing snapshot for diagnostics outside the private PSVR2 driver.
 *
 * The *_vts_ns fields are in the headset VTS clock domain. The corresponding
 * *_monotonic_ns fields are mapped into Monado's os_monotonic_get_ns() domain
 * using the driver's current VTS-to-monotonic clock offset.
 */
struct psvr2_slam_timing
{
	bool valid;
	int timestamp_samples;
	int64_t slam_vts_ns;
	int64_t slam_monotonic_ns;
	int64_t imu_vts_ns;
	int64_t imu_monotonic_ns;
	int64_t hw2mono_vts_ns;
};

#define PSVR2_CAMERA_DIAGNOSTIC_HEADER_SIZE 256
#define PSVR2_CAMERA_DIAGNOSTIC_SIZE_SLOTS 8

struct psvr2_camera_packet_size_count
{
	uint32_t size;
	uint64_t count;
};

/*!
 * Snapshot of the raw PS VR2 camera stream for protocol diagnostics.
 *
 * Timestamps are USB completion times in Monado's monotonic clock domain.
 * They are deliberately not presented as camera exposure timestamps.
 */
struct psvr2_camera_diagnostics
{
	bool enabled;
	uint8_t configured_mode;
	uint64_t frame_count;
	uint64_t vi_signature_count;
	uint32_t last_packet_size;
	uint32_t last_vts_us;
	uint32_t last_sequence_id;
	int64_t last_vts_monotonic_ns;
	uint16_t last_camera_set;
	uint16_t last_image_width;
	uint16_t last_image_height;
	int64_t first_arrival_ns;
	int64_t last_arrival_ns;
	int64_t last_interval_ns;
	uint32_t last_header_size;
	uint8_t last_header[PSVR2_CAMERA_DIAGNOSTIC_HEADER_SIZE];
	struct psvr2_camera_packet_size_count packet_sizes[PSVR2_CAMERA_DIAGNOSTIC_SIZE_SLOTS];
};

/*!
 * Create the PS VR2 HMD device
 *
 * @ingroup drv_psvr2
 */
struct xrt_device *
psvr2_hmd_create(struct xrt_prober_device *xpdev);

/*!
 * Snapshot PSVR2 SLAM/IMU clock state for diagnostics.
 *
 * @param xdev A PSVR2 xrt_device.
 * @param out Timing snapshot to fill.
 * @return true when the arguments are valid and a snapshot was taken.
 *         Check out->valid to determine whether timestamp calibration and a
 *         SLAM sample are available yet.
 */
bool
psvr2_get_slam_timing(struct xrt_device *xdev, struct psvr2_slam_timing *out);

/*!
 * Attach optional consumers for the two PS VR2 front-camera passthrough views.
 *
 * With PSVR2_CAMERA_STREAMS=1 the macOS path uses the stock-headset mode 0x10
 * stream validated by GAV: two 1024x1016 BC4 grayscale frames. This follows
 * the existing WMR camera sink model; the caller owns sink lifetime.
 */
xrt_result_t
psvr2_set_passthrough_sinks(struct xrt_device *xdev, struct xrt_frame_sink *left, struct xrt_frame_sink *right);

/*!
 * Snapshot raw camera packet metadata. This never fabricates an exposure time.
 */
bool
psvr2_get_camera_diagnostics(struct xrt_device *xdev, struct psvr2_camera_diagnostics *out);

/*!
 * Return the timing source backed by camera packet VTS timestamps.
 */
struct t_timing_event_source *
psvr2_get_timing_event_source(struct xrt_device *xdev);

/*!
 * Attach four sinks for mode-4 L8 controller-tracking camera images.
 * Camera-set 4 maps to sinks 0/1 and camera-set 5 maps to sinks 2/3. Pass NULL to detach and drain in-flight
 * pushes before freeing sinks. A non-NULL attachment is rejected while another consumer is attached.
 */
bool
psvr2_set_camera_frame_sinks(struct xrt_device *xdev, struct xrt_frame_sink *const sinks[4]);

struct t_blob_sink;

/*!
 * Attach four sinks for the headset's own LED detections (PSVR2_LED_DETECTOR_BLOBS=1), one per mode-4 camera in
 * Monado's camera order, each fed one blob observation per exposure. Same attach/detach rules as
 * @ref psvr2_set_camera_frame_sinks. Fails if the detector stream was not opened.
 */
bool
psvr2_set_led_detector_blob_sinks(struct xrt_device *xdev, struct t_blob_sink *const sinks[4]);

/*!
 * Whether PSVR2_LED_DETECTOR_BLOBS is set: the caller should attach blob sinks with
 * @ref psvr2_set_led_detector_blob_sinks instead of running blob detection on the camera images.
 */
bool
psvr2_led_detector_blobs_requested(void);

/*!
 * Register a function run at the start of the headset's destroy, before anything of the headset is torn down. The
 * headset is destroyed before the other system devices, so the hook can stop a pipeline that uses the headset and the
 * controllers (the Sense optical tracker) while all of them still exist.
 */
bool
psvr2_set_teardown_hook(struct xrt_device *xdev, void (*hook)(void *data), void *data);

/*!
 * Probing function for PlayStation VR2 devices.
 *
 * @ingroup drv_psvr2
 * @see xrt_prober_found_func_t
 */
int
psvr2_found(struct xrt_prober *xp,
            struct xrt_prober_device **devices,
            size_t device_count,
            size_t index,
            cJSON *attached_data,
            struct xrt_device **out_xdevs);

/*!
 * @dir drivers/psvr2
 *
 * @brief @ref drv_psvr2 files.
 */


#ifdef __cplusplus
}
#endif

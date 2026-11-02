// Copyright 2024, Coreforge
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Pimax P2 driver header - inherits xrt_device.
 * @author Coreforge <BC549b@gmail.com>
 * @ingroup drv_pimax_p2
 */

#pragma once
#include "xrt/xrt_device.h"

#include "os/os_threading.h"
#include "util/u_var.h"

#include <hidapi/hidapi.h>


#define PIMAX_P2_MODEL_NAME_LENGTH 32

/*!
 * @defgroup drv_pimax_p2 Pimax P2 HMD driver
 * @ingroup drv
 *
 * @brief Driver for the HID device of Pimax P2 HMDs.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct pimax_p2_device;

/*!
 * Initializes the Pimax HID device, enables the displays, and allocates and
 * populates pdev with data.
 * Should be called before lighthouse init.
 * If no Pimax P2 HID is found or an error occurs, returns false.
 *
 * @ingroup drv_pimax_p2
 */
bool
pimax_p2_init(struct pimax_p2_device **pdev);

/*!
 * Fills the xrt_hmd_parts with data.
 *
 * @ingroup drv_pimax_p2
 */
bool
pimax_p2_setup_hmd(struct pimax_p2_device *pdev, struct xrt_hmd_parts *hmd);

/*!
 * Like xrt_device->get_view_poses, but does not fill in the head relation.
 * Tracking must be handled separately.
 *
 * @ingroup drv_pimax_p2
 */
xrt_result_t
pimax_p2_get_view_poses(struct pimax_p2_device *dev,
                        const struct xrt_vec3 *default_eye_relation,
                        struct xrt_hmd_parts *hmd,
                        uint32_t view_count,
                        struct xrt_fov *out_fovs,
                        struct xrt_pose *out_poses);


/*!
 * Compute distortion, using the mesh loaded by init.
 *
 * @ingroup drv_pimax_p2
 */
xrt_result_t
pimax_p2_compute_distortion_from_mesh(
    struct pimax_p2_device *dev, uint32_t view, float u, float v, struct xrt_uv_triplet *out_result);

/*!
 * Get backlight brightness from 0 to 1.
 *
 * @ingroup drv_pimax_p2
 */
xrt_result_t
pimax_p2_get_brightness(struct pimax_p2_device *dev, float *out_brightness);

/*!
 * Set backlight brightness from 0 to 1.
 *
 * @ingroup drv_pimax_p2
 */
xrt_result_t
pimax_p2_set_brightness(struct pimax_p2_device *dev, float brightness, bool relative);

/*!
 * Clean up a pimax_p2_device.
 *
 * @ingroup drv_pimax_p2
 */
void
pimax_p2_destroy(struct pimax_p2_device *dev);

struct pimax_p2_device;

struct pimax_p2_display_properties
{
	struct xrt_vec2 size_in_meters;
	uint32_t pixels_width;
	uint32_t pixels_height;
	uint32_t refresh_rate;
	float nominal_frame_interval_ns;
	float gap;
};

typedef void (*pimax_p2_display_get_props_func_t)(struct pimax_p2_device *dev,
                                                  struct pimax_p2_display_properties *out_props);
typedef void (*pimax_p2_init_display_func_t)(struct pimax_p2_device *dev);


struct pimax_p2_model_funcs
{
	pimax_p2_display_get_props_func_t get_display_properties;
	pimax_p2_init_display_func_t init_display;
};

struct pimax_p2_model_config
{
	const wchar_t product_name[PIMAX_P2_MODEL_NAME_LENGTH]; // the name in the USB descriptor
	const char display_name[PIMAX_P2_MODEL_NAME_LENGTH];    // the name for the xdev
	const char *default_mesh_name;
	struct pimax_p2_model_funcs funcs;
};

struct pimax_p2_view_mesh
{
	struct xrt_fov fov;
	size_t len;
	float *data;
};

struct pimax_p2_mesh
{
	float ipd;
	struct pimax_p2_view_mesh views[2];
};

struct pimax_p2_mesh_set
{
	size_t mesh_count;
	struct pimax_p2_mesh *meshes;
};

struct pimax_p2_device
{
	struct pimax_p2_model_funcs *model_funcs;
	const char *default_mesh_name;

	char display_name[XRT_DEVICE_NAME_LEN];

	struct pimax_p2_mesh_set *mesh_set;

	hid_device *hid_dev;
	struct os_mutex hid_mutex;
	struct pimax_p2_device_config
	{
		float ipd;
		float separation;
		int brightness;
		bool upscaling;
		struct u_var_draggable_f32 offset_h_0;
		struct u_var_draggable_f32 offset_h_1;
		struct u_var_draggable_f32 offset_v_0;
		struct u_var_draggable_f32 offset_v_1;
	} device_config;

	// as the mutex and thread are direct struct members, they cannot be set to NULL to indicate status
	enum
	{
		PIMAX_P2_INIT_STATE_HID_MUTEX = 1 << 0,
		// hid_dev can just be checked for NULL
		PIMAX_P2_INIT_STATE_HID_THREAD = 1 << 1,
		PIMAX_P2_INIT_STATE_HID_THREAD_RUNNING = 1 << 2,
	} init_state_flags;

	struct os_thread poll_thread;
	bool should_poll;
	uint32_t polls_since_last_keepalive;

	bool always_update_distortion;
};

#ifdef __cplusplus
}
#endif

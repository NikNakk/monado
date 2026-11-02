// Copyright 2024, Coreforge
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Pimax P2 driver implementation.
 * @author Coreforge <BC549b@gmail.com>
 * @ingroup drv_pimax_p2
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_results.h"

#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_time.h"
#include "util/u_device.h"
#include "util/u_var.h"
#include "util/u_file.h"

#include "math/m_vec2.h"
#include "math/m_api.h"

#include "os/os_time.h"

#include "pimax_p2.h"

#include <cjson/cJSON.h>


#define OFFSETS_MIN -0.15
#define OFFSETS_MAX 0.15
#define OFFSETS_STEP 0.00005

#define PIMAX_P2_VID 0x0483
#define PIMAX_P2_PID 0x0101

#define PIMAX_P2_POLL_TIMEOUT 5
#define PIMAX_P2_POLL_WAIT 10
#define PIMAX_P2_POLL_KEEPALIVE_WAIT_COUNT 50 // every x polls, send keepalive packets


DEBUG_GET_ONCE_FLOAT_OPTION(pimax_ipd_offs_v0, "PIMAX_IPD_V0", 0.0)
DEBUG_GET_ONCE_FLOAT_OPTION(pimax_ipd_offs_v1, "PIMAX_IPD_V1", 0.0)
DEBUG_GET_ONCE_FLOAT_OPTION(pimax_ipd_offs_h0, "PIMAX_IPD_H0", 0.0)
DEBUG_GET_ONCE_FLOAT_OPTION(pimax_ipd_offs_h1, "PIMAX_IPD_H1", 0.0)
// required, as the resolution needs to be known
DEBUG_GET_ONCE_NUM_OPTION(pimax_desired_mode, "XRT_COMPOSITOR_DESIRED_MODE", -1)

DEBUG_GET_ONCE_NUM_OPTION(pimax_hid_retry, "PIMAX_HID_RETRY_COUNT", 3)
DEBUG_GET_ONCE_NUM_OPTION(pimax_hid_delay, "PIMAX_HID_RETRY_DELAY_MS", 1000)

// ugly workaround for the 5K XR until monado has proper subpixel shading (it's not as noticeable on other headsets)
DEBUG_GET_ONCE_FLOAT_OPTION(pimax_img_offs_x_r, "PIMAX_OFFS_X_R", 0.0)
DEBUG_GET_ONCE_FLOAT_OPTION(pimax_img_offs_x_g, "PIMAX_OFFS_X_G", 0.0)
DEBUG_GET_ONCE_FLOAT_OPTION(pimax_img_offs_x_b, "PIMAX_OFFS_X_B", 0.0)

DEBUG_GET_ONCE_BOOL_OPTION(pimax_check_init, "PIMAX_CHECK_INIT", false)
DEBUG_GET_ONCE_BOOL_OPTION(pimax_reboot, "PIMAX_REBOOT", false)
DEBUG_GET_ONCE_NUM_OPTION(pimax_reboot_delay, "PIMAX_REBOOT_DELAY_MS", 5000)

// for loading meshes from json files instead of having them compiled in
#define PIMAX_MESHES_DEFAULT_PATH ".config/pimax/meshes"
#define PIMAX_MESHES_PATH_ENV_VAR "PIMAX_MESHES_PATH" // folder to look in for meshes
#define PIMAX_MESH_NAME_ENV_VAR "PIMAX_MESH_NAME"     // file name of the mesh json

// forward declarations, these aren't needed anywhere else, so no need to put them into the header
static void
pimax_8kx_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props);
static void
pimax_5ks_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props);
static void
pimax_p2d_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props);
static void
pimax_p2c_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props);
static void
pimax_p2b_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props);
static void
pimax_p2ea_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props);
static void
pimax_p2z_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props);
static void
pimax_p2y_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props);

static void
init_display_8kx(struct pimax_p2_device *dev);
static void
init_display_p2c(struct pimax_p2_device *dev);

struct pimax_p2_model_config model_configs[] = {
    {L"Pimax P2EA", "Pimax 8K Plus", "p2ea.json", {pimax_p2ea_get_display_props, init_display_8kx}},
    {L"Pimax P2E", "Pimax 8K Plus", "p2e.json", {pimax_p2ea_get_display_props, init_display_p2c}},
    {L"Pimax P2A", "Pimax 5K Super", "p2a.json", {pimax_5ks_get_display_props, init_display_8kx}},
    {L"Pimax P2C", "Pimax 5K Super", "p2c.json", {pimax_p2c_get_display_props, init_display_p2c}},
    {L"Pimax P2N", "Pimax 8KX", "p2n.json", {pimax_8kx_get_display_props, init_display_8kx}},
    {L"Pimax P2D", "Pimax 5K+", "p2d.json", {pimax_p2d_get_display_props, init_display_8kx}},
    {L"Pimax P2B", "Pimax 5K XR", "p2b.json", {pimax_p2b_get_display_props, init_display_8kx}},
    {L"Pimax P2Z", "Pimax 5K Super", "p2z.json", {pimax_p2z_get_display_props, init_display_8kx}},
    {L"Pimax P2F", "Pimax 5K Super", "p2f.json", {pimax_p2z_get_display_props, init_display_8kx}},
    {L"Pimax P2Y", "Pimax Artisan", "p2y.json", {pimax_p2y_get_display_props, init_display_8kx}},
};


uint8_t pimax_packet_parallel_projections_off[64] = {
    0xF0, 0x00, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

uint8_t pimax_init2[64] = {0xF0, 0x00, 0x00, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0xf6, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                           0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

uint8_t pimax_keepalive[64] = {0xF0, 0x00, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

uint8_t pimax_hmd_power[64] = {0xF0, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

uint8_t pimax_reboot[64] = {0xF0, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

uint8_t pimax_poll_freq[6] = {0x11, 0x00, 0x00, 0x0b, 0x10, 0x27};


/*
 *  About the display properties specified here (and in the distortion files):
 *
 *  The resolution specified is per eye, and rotated. This makes it easier to populate the views.
 *
 *  Example:
 *      If a headset reports a resolution of 2880x2160 and has the usual layout of the P2 series
 *      (two portrait mode panels side by side, I'm not aware of headsets with a different layout),
 *      The height of the screen is the width of the view, and half the width of the screen is the
 *      height of the view.
 *      [headset] 2880x2160 -> [view, display props, json] 2160x1440
 *
 **/

static void
pimax_8kx_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props)
{
	out_props->pixels_width = dev->device_config.upscaling ? 2160 : 3168;
	out_props->pixels_height = dev->device_config.upscaling ? 1440 : 2160;

	// out_props->nominal_frame_interval_ns = 1000.*1000.*1000./(dev->device_config.upscaling ? 110. : 90.);
	out_props->refresh_rate = dev->device_config.upscaling ? 110 : 90;
}

static void
pimax_5ks_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props)
{
	out_props->pixels_width = 2160;
	out_props->pixels_height = 1440;

	out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 110.;
	out_props->refresh_rate = 110;
}

static void
pimax_p2d_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props)
{
	switch (debug_get_num_option_pimax_desired_mode()) {
	case 0:
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 65.;
		out_props->refresh_rate = 65;
		break;
	case 2:
		out_props->pixels_width = 1600;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 120.;
		out_props->refresh_rate = 120;
		break;
	case 3:
		out_props->pixels_width = 1600;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 144.;
		out_props->refresh_rate = 144;
		break;
	default:
	case 1:
		out_props->pixels_width = 2038;
		out_props->pixels_height = 1440;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 90.;
		out_props->refresh_rate = 90;
		break;
	}
}

static void
pimax_p2c_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props)
{
	switch (debug_get_num_option_pimax_desired_mode()) {
	case 1:
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 72.;
		out_props->refresh_rate = 72;
		break;
	case 3:
		out_props->pixels_width = 1600;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 120.;
		out_props->refresh_rate = 120;
		break;
	default:
	case 2:
		out_props->pixels_width = 2560;
		out_props->pixels_height = 1440;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 90.;
		out_props->refresh_rate = 90;
		break;
	}
}
static void
pimax_p2ea_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props)
{
	switch (debug_get_num_option_pimax_desired_mode()) {
	case 0:
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 72.;
		out_props->refresh_rate = 72;
		break;
	case 2:
		out_props->pixels_width = 2160;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 110.;
		out_props->refresh_rate = 110;
		break;
	default:
	case 1:
		out_props->pixels_width = 2560;
		out_props->pixels_height = 1440;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 90.;
		out_props->refresh_rate = 90;
		break;
	}
}

static void
pimax_p2b_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props)
{
	switch (debug_get_num_option_pimax_desired_mode()) {
	case 0:
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 64.;
		out_props->refresh_rate = 64;
		break;
	case 1:
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 72.;
		out_props->refresh_rate = 72;
		break;
	default:
	case 2:
		out_props->pixels_width = 2560;
		out_props->pixels_height = 1440;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 82.;
		out_props->refresh_rate = 82;
		break;
	}
}

static void
pimax_p2z_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props)
{
	switch (debug_get_num_option_pimax_desired_mode()) {
	case 2:
		out_props->pixels_width = 1700;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 120.;
		out_props->refresh_rate = 120;
		break;
	case 3:
		out_props->pixels_width = 1600;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 144.;
		out_props->refresh_rate = 144;
		break;
	case 4:
		out_props->pixels_width = 1436;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 160.;
		out_props->refresh_rate = 160;
		break;
	case 5:
		out_props->pixels_width = 1328;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 180.;
		out_props->refresh_rate = 180;
		break;
	default:
	case 1:
		out_props->pixels_width = 2464;
		out_props->pixels_height = 1440;
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 90.;
		out_props->refresh_rate = 90;
		break;
	}
}

static void
pimax_p2y_get_display_props(struct pimax_p2_device *dev, struct pimax_p2_display_properties *out_props)
{
	out_props->pixels_width = 1600;
	out_props->pixels_height = 1440;

	switch (debug_get_num_option_pimax_desired_mode()) {
	case 1:
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 72.;
		out_props->refresh_rate = 72;
		break;
	case 3:
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 120.;
		out_props->refresh_rate = 120;
		break;
	default:
	case 2:
		out_props->nominal_frame_interval_ns = 1000. * 1000. * 1000. / 90.;
		out_props->refresh_rate = 90;
		break;
	}
}

/*
 *
 * Helper functions.
 *
 */

static float
pimax_p2_8kx_lens_separation_from_raw(int16_t raw)
{
	// U_LOG_D("Raw: %d", raw);
	return (raw < 0 ? (((raw & 0x7fff) * -0.168f) + 4960) : raw) / 50000.f;
}

static float
pimax_p2_8kx_ipd_from_raw(uint16_t raw)
{
	return pimax_p2_8kx_lens_separation_from_raw(raw) - 0.026f;
}

/*
 *
 * Non-exported HID functions
 *
 */

static void
pimax_p2_8kx_read_config(struct pimax_p2_device *dev)
{
	if (!dev->hid_dev) {
		U_LOG_E("Pimax HID device not available");
		return;
	}
	uint8_t buf[64];
	buf[0] = 240;
	if (hid_get_feature_report(dev->hid_dev, buf, 64) == -1) {
		U_LOG_E("Failed to read feature report: %ls", hid_error(dev->hid_dev));
		return;
	}
	dev->device_config.upscaling = buf[35] & 1;
	dev->device_config.ipd = pimax_p2_8kx_ipd_from_raw(*(uint16_t *)(&buf[36]));
	dev->device_config.separation = pimax_p2_8kx_lens_separation_from_raw(*(uint16_t *)(&buf[36]));
	dev->device_config.brightness = buf[30];
	U_LOG_D("IPD Set to %f", dev->device_config.ipd);
	U_LOG_D("Separation Set to %f", dev->device_config.separation);
}

static bool
pimax_p2_get_power_status(hid_device *hid)
{
	if (hid_send_feature_report(hid, pimax_poll_freq, sizeof(pimax_poll_freq)) == -1) {
		U_LOG_E("Failed to send polling frequency report: %ls", hid_error(hid));
		return false; // without this report sent, no data can be polled, assume no init
	}

	uint8_t buf[64] = {
	    240,
	};
	if (hid_get_feature_report(hid, buf, 64) == -1) {
		U_LOG_E("Failed to read status report: %ls", hid_error(hid));
		return false;
	}

	return buf[0x37] == 1; // check exactly for 1, just as a safeguard against potentially different structs
}

static void
pimax_p2_8kx_poll_locked(struct pimax_p2_device *dev)
{
	if (!dev->hid_dev) {
		U_LOG_E("Pimax HID device not available");
		return;
	}
	if (hid_send_feature_report(dev->hid_dev, pimax_poll_freq, sizeof(pimax_poll_freq)) == -1) {
		U_LOG_E("Failed to send polling frequency report: %ls", hid_error(dev->hid_dev));
		return; // without this report sent, no data can be polled
	}
	uint8_t buf[64];
	if (hid_read_timeout(dev->hid_dev, buf, 64, PIMAX_P2_POLL_TIMEOUT) == -1) {
		U_LOG_E("Failed to read input report: %ls", hid_error(dev->hid_dev));
		return;
	}

	uint8_t report_type = buf[0];

	if (report_type == 238) {
		// IPD Changed
		dev->device_config.ipd = pimax_p2_8kx_ipd_from_raw(*(uint16_t *)(&buf[4]));
		dev->device_config.separation = pimax_p2_8kx_lens_separation_from_raw(*(uint16_t *)(&buf[4]));
		U_LOG_D("IPD Set to %f", dev->device_config.ipd);
		U_LOG_D("Separation Set to %f", dev->device_config.separation);
	}

	if (dev->polls_since_last_keepalive > PIMAX_P2_POLL_KEEPALIVE_WAIT_COUNT) {
		// send keepalive
		dev->polls_since_last_keepalive = 0;
		hid_send_feature_report(dev->hid_dev, pimax_keepalive, sizeof(pimax_keepalive));
	}

	dev->polls_since_last_keepalive++;
}

// used for most HMDs, but mostly based on packets recorded from a P2N
static void
init_display_8kx(struct pimax_p2_device *dev)
{
	hid_send_feature_report(dev->hid_dev, pimax_init2, sizeof(pimax_init2));
	hid_send_feature_report(dev->hid_dev, pimax_packet_parallel_projections_off,
	                        sizeof(pimax_packet_parallel_projections_off));
	hid_send_feature_report(dev->hid_dev, pimax_hmd_power, sizeof(pimax_hmd_power));
	hid_send_feature_report(dev->hid_dev, pimax_keepalive, sizeof(pimax_keepalive));
}

static void
init_display_p2c(struct pimax_p2_device *dev)
{
	hid_send_feature_report(dev->hid_dev, pimax_packet_parallel_projections_off,
	                        sizeof(pimax_packet_parallel_projections_off));
	hid_send_feature_report(dev->hid_dev, pimax_keepalive, sizeof(pimax_keepalive));
	hid_send_feature_report(dev->hid_dev, pimax_init2, sizeof(pimax_init2));
}

static void
pimax_p2_open_hid(hid_device **dev)
{
	int attempts = 0;
	do {
		if (attempts) {
			U_LOG_W("Failed to open Pimax HID device, retrying");
			os_nanosleep(debug_get_num_option_pimax_hid_delay() * U_TIME_1MS_IN_NS);
		}
		*dev = hid_open(PIMAX_P2_VID, PIMAX_P2_PID, NULL);
		attempts++;
	} while (!*dev && attempts < debug_get_num_option_pimax_hid_retry());
}

/*
 *
 * Other member functions
 *
 */

static void
pimax_p2_8kx_poll(struct pimax_p2_device *dev)
{
	os_mutex_lock(&dev->hid_mutex);
	pimax_p2_8kx_poll_locked(dev);
	os_mutex_unlock(&dev->hid_mutex);
}

static void *
pimax_p2_poll_thread_func(void *ptr)
{
	struct pimax_p2_device *dev = (struct pimax_p2_device *)ptr;
	while (dev->should_poll) {
		pimax_p2_8kx_poll((struct pimax_p2_device *)ptr);
		os_nanosleep(PIMAX_P2_POLL_WAIT * U_TIME_1MS_IN_NS);
	}
	return NULL;
}

static void
pimax_p2_update_fovs_from_mesh(struct pimax_p2_device *dev, struct xrt_hmd_parts *hmd)
{
	for (int i = 0; i < 2; i++) {
		if (!dev->mesh_set) {
			// there's nothing really useful that can be done here
			return;
		}
		struct xrt_fov fov_lower, fov_upper;
		float separation = dev->device_config.ipd;
		if (i == 0) {
			separation += dev->device_config.offset_h_0.val * 2;
		} else {
			separation += dev->device_config.offset_h_1.val * 2;
		}

		int lower_mesh_idx = -1;
		int upper_mesh_idx = -1;
		// this assumes the entries are sorted with ascending lens separation
		for (int i = dev->mesh_set->mesh_count - 1; i >= 0; i--) {
			if (dev->mesh_set->meshes[i].ipd < separation) {
				upper_mesh_idx = i;
				if (i == 0) {
					if (dev->mesh_set->mesh_count > 1)
						upper_mesh_idx = 1;
					lower_mesh_idx = 0;
				} else {
					lower_mesh_idx = i - 1;
				}
				break;
			}
		}
		if (lower_mesh_idx == -1 || upper_mesh_idx == -1) {
			lower_mesh_idx = upper_mesh_idx = dev->mesh_set->mesh_count - 1;
			if (dev->mesh_set->mesh_count > 1)
				lower_mesh_idx--;
		}
		float range = dev->mesh_set->meshes[upper_mesh_idx].ipd - dev->mesh_set->meshes[lower_mesh_idx].ipd;
		float amount = (separation - dev->mesh_set->meshes[upper_mesh_idx].ipd) / range;

		fov_lower = dev->mesh_set->meshes[lower_mesh_idx].views[i].fov;
		fov_upper = dev->mesh_set->meshes[upper_mesh_idx].views[i].fov;
		hmd->distortion.fov[i].angle_up = math_lerp(fov_lower.angle_up, fov_upper.angle_up, amount);
		hmd->distortion.fov[i].angle_down = math_lerp(fov_lower.angle_down, fov_upper.angle_down, amount);
		hmd->distortion.fov[i].angle_left = math_lerp(fov_lower.angle_left, fov_upper.angle_left, amount);
		hmd->distortion.fov[i].angle_right = math_lerp(fov_lower.angle_right, fov_upper.angle_right, amount);
	}
}

static void
pimax_p2_delete_mesh_set(struct pimax_p2_device *dev)
{
	for (size_t mesh = 0; mesh < dev->mesh_set->mesh_count; mesh++) {
		free(dev->mesh_set->meshes[mesh].views[0].data);
		free(dev->mesh_set->meshes[mesh].views[1].data);
	}
	free(dev->mesh_set->meshes);
	free(dev->mesh_set);
	dev->mesh_set = NULL;
}

static size_t
pimax_p2_get_mesh_path(struct pimax_p2_device *dev, size_t max_len, char *out_path)
{
	size_t outlen = 0;
	// directory
	if (getenv(PIMAX_MESHES_PATH_ENV_VAR) != NULL) {
		strncpy(out_path, getenv(PIMAX_MESHES_PATH_ENV_VAR), max_len);
		outlen += strlen(getenv(PIMAX_MESHES_PATH_ENV_VAR));
		if (max_len > outlen) {
			out_path[outlen] = '/';
		}
		outlen++;
	} else {
		strncpy(out_path, getenv("HOME"), max_len);
		outlen += strlen(getenv("HOME"));
		if (max_len > outlen) {
			out_path[outlen] = '/';
		}
		outlen++;
		if (max_len > outlen) {
			strncpy(out_path + outlen, PIMAX_MESHES_DEFAULT_PATH,
			        (strlen(PIMAX_MESHES_DEFAULT_PATH) > (max_len - outlen)) ? 0 : (max_len - outlen));
		}
		outlen += strlen(PIMAX_MESHES_DEFAULT_PATH);
		if (max_len > outlen) {
			out_path[outlen] = '/';
		}
		outlen++;
	}

	// file
	if (getenv(PIMAX_MESH_NAME_ENV_VAR)) {
		if (max_len > outlen) {
			size_t remaining =
			    (strlen(getenv(PIMAX_MESH_NAME_ENV_VAR)) > (max_len - outlen)) ? 0 : (max_len - outlen);
			strncpy(out_path + outlen, getenv(PIMAX_MESH_NAME_ENV_VAR), remaining);
		}
		outlen += strlen(getenv(PIMAX_MESH_NAME_ENV_VAR));
	} else {
		if (max_len > outlen) {
			size_t remaining =
			    (strlen(dev->default_mesh_name) > (max_len - outlen)) ? 0 : (max_len - outlen);
			strncpy(out_path + outlen, dev->default_mesh_name, remaining);
		}
		outlen += strlen(dev->default_mesh_name);
	}

	if (max_len > outlen) {
		out_path[outlen] = 0;
	}
	outlen++;
	if (max_len > 0) {
		out_path[max_len - 1] = 0;
	}
	return outlen;
}

static void
pimax_p2_load_meshes_from_file(struct pimax_p2_device *dev, const char *path)
{
	dev->mesh_set = NULL;
	char *data = u_file_read_content_from_path(path, NULL);
	if (!data) {
		U_LOG_E(
		    "Error reading mesh file %s! Check if the path is correct and if you have permissions to read the "
		    "file!",
		    path);
		return;
	}
	cJSON *json = cJSON_Parse(data);
	if (!json) {
		U_LOG_E("Failed to parse json from %s!", path);
		return;
	}

	struct pimax_p2_display_properties display_props;
	dev->model_funcs->get_display_properties(dev, &display_props);
	if (!cJSON_IsArray(json)) {
		U_LOG_E("Mesh json has wrong structure!");
		return;
	}
	cJSON *currentMeshSet;
	bool mesh_matches = false;
	cJSON_ArrayForEach(currentMeshSet, json)
	{
		// check resolutions first
		cJSON *currentResolution;
		bool res_match = false;
		bool refrate_match = false;
		cJSON_ArrayForEach(currentResolution, cJSON_GetObjectItem(currentMeshSet, "resolutions"))
		{
			uint32_t width = cJSON_GetNumberValue(cJSON_GetObjectItem(currentResolution, "width"));
			uint32_t height = cJSON_GetNumberValue(cJSON_GetObjectItem(currentResolution, "height"));
			if (width == display_props.pixels_width && height == display_props.pixels_height) {
				res_match = true;
				break;
			}
		}
		// also check the refresh rates at some point
		cJSON_ArrayForEach(currentResolution, cJSON_GetObjectItem(currentMeshSet, "refresh_rates"))
		{
			uint32_t refrate = cJSON_GetNumberValue(currentResolution);
			if (refrate == display_props.refresh_rate) {
				refrate_match = true;
				break;
			}
		}

		if (res_match && refrate_match) {
			mesh_matches = true;
			break;
		}
	}
	if (!mesh_matches) {
		U_LOG_E("Could not find a matching mesh for the current display mode!");
		return;
	}
	U_LOG_D("Found mesh!");

	// now create the mesh structures in the pimax_device
	cJSON *meshes = cJSON_GetObjectItem(currentMeshSet, "meshes");
	int mesh_count = cJSON_GetArraySize(meshes);
	dev->mesh_set = U_TYPED_CALLOC(struct pimax_p2_mesh_set);
	dev->mesh_set->mesh_count = mesh_count;
	dev->mesh_set->meshes = U_TYPED_ARRAY_CALLOC(struct pimax_p2_mesh, mesh_count);

	cJSON *currentMesh;
	size_t meshIdx = 0;
	cJSON_ArrayForEach(currentMesh, meshes)
	{
		struct pimax_p2_mesh *currentPMesh = dev->mesh_set->meshes + meshIdx;
		currentPMesh->ipd = atof(currentMesh->string);
		for (size_t view = 0; view < 2; view++) {
			cJSON *view_json = cJSON_GetArrayItem(currentMesh, view);
			currentPMesh->views[view].fov = (struct xrt_fov){
			    cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(view_json, "fov"), 0)),
			    cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(view_json, "fov"), 1)),
			    cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(view_json, "fov"), 2)),
			    cJSON_GetNumberValue(cJSON_GetArrayItem(cJSON_GetObjectItem(view_json, "fov"), 3)),
			};
			cJSON *points = cJSON_GetObjectItem(view_json, "points");
			currentPMesh->views[view].len = cJSON_GetArraySize(points);
			currentPMesh->views[view].data = U_TYPED_ARRAY_CALLOC(float, currentPMesh->views[view].len);
			size_t idx = 0;
			cJSON *pval;
			cJSON_ArrayForEach(pval, points)
			{
				currentPMesh->views[view].data[idx] = cJSON_GetNumberValue(pval);
				idx++;
			}
		}
		meshIdx++;
	}
}

static time_duration_ns
init_p2_pimax8kx(struct hid_device_info *devinfo, struct pimax_p2_device **pdev)
{
	if (devinfo->interface_number)
		return 0;

	struct pimax_p2_device *dev = U_TYPED_CALLOC(struct pimax_p2_device);
	if (dev == NULL) {
		return 0;
	}

	if (os_mutex_init(&dev->hid_mutex)) {
		pimax_p2_destroy(dev);
		return 0;
	}
	dev->init_state_flags |= PIMAX_P2_INIT_STATE_HID_MUTEX;

	U_LOG_D("Pimax 8KX init\n");
	os_mutex_lock(&dev->hid_mutex);
	dev->hid_dev = NULL;
	pimax_p2_open_hid(&dev->hid_dev);
	if (!dev->hid_dev) {
		os_mutex_unlock(&dev->hid_mutex);
		U_LOG_E("Failed to open Pimax 8KX HID device");
		pimax_p2_destroy(dev);
		return 0;
	}

	if (debug_get_bool_option_pimax_reboot()) {
		U_LOG_I("Rebooting Pimax HMD");
		hid_send_feature_report(dev->hid_dev, pimax_reboot, sizeof(pimax_reboot));
		hid_close(dev->hid_dev);
		dev->hid_dev = NULL;
		os_nanosleep(debug_get_num_option_pimax_reboot_delay() * U_TIME_1MS_IN_NS);
		pimax_p2_open_hid(&dev->hid_dev);

		if (!dev->hid_dev) {
			os_mutex_unlock(&dev->hid_mutex);
			U_LOG_E("Failed to open Pimax HID device after rebooting");
			pimax_p2_destroy(dev);
			return 0;
		}
	}

	wchar_t buf[PIMAX_P2_MODEL_NAME_LENGTH];
	if (hid_get_product_string(dev->hid_dev, buf, ARRAY_SIZE(buf)) == -1) {
		os_mutex_unlock(&dev->hid_mutex);
		U_LOG_E("Failed to retrieve HID model string!");
		pimax_p2_destroy(dev);
		return 0;
	}
	bool found_match = false;
	// retrieve the model specific functions
	for (size_t i = 0; i < ARRAY_SIZE(model_configs); i++) {
		if (wcsncmp(buf, model_configs[i].product_name, PIMAX_P2_MODEL_NAME_LENGTH) == 0) {
			dev->model_funcs = &model_configs[i].funcs;
			strncpy(dev->display_name, model_configs[i].display_name,
			        (PIMAX_P2_MODEL_NAME_LENGTH < XRT_DEVICE_NAME_LEN ? PIMAX_P2_MODEL_NAME_LENGTH
			                                                          : XRT_DEVICE_NAME_LEN) -
			            1);
			found_match = true;
			dev->default_mesh_name = model_configs[i].default_mesh_name;
			break;
		}
	}
	if (!found_match) {
		U_LOG_W(
		    "Could not find a matching model specific configuration for \"%ls\". Falling back to the first one "
		    "in the list (please report this)",
		    buf);
	}


	if (!debug_get_bool_option_pimax_check_init() || !pimax_p2_get_power_status(dev->hid_dev)) {
		// doing init again apparently causes issues on some models
		// a full reboot of the HMD would be an alternative (pimax does this), but this is cleaner imo
		U_LOG_D("Sending initialization packets to HMD");
		dev->model_funcs->init_display(dev);
	}
	pimax_p2_8kx_read_config(dev);
	os_mutex_unlock(&dev->hid_mutex);

	*pdev = dev;


	/*
	 * Distortion mesh stuff
	 */

	size_t mesh_path_len = pimax_p2_get_mesh_path(dev, 0, NULL);
	char *meshpath = U_TYPED_ARRAY_CALLOC(char, mesh_path_len);
	pimax_p2_get_mesh_path(dev, mesh_path_len, meshpath);
	U_LOG_I("Using distortion meshes from: %s", meshpath);
	pimax_p2_load_meshes_from_file(dev, meshpath);
	free(meshpath);

	/*
	 * Start the polling thread. Currently, this only handles IPD adjustments
	 */
	dev->should_poll = true;
	dev->polls_since_last_keepalive = 0;
	if (os_thread_init(&dev->poll_thread)) {
		pimax_p2_destroy(dev);
		U_LOG_E("Failed to create polling thread for pimax_p2!");
		return 0;
	}
	dev->init_state_flags |= PIMAX_P2_INIT_STATE_HID_THREAD;
	if (os_thread_start(&dev->poll_thread, pimax_p2_poll_thread_func, dev)) {
		pimax_p2_destroy(dev);
		U_LOG_E("Failed to start polling thread for pimax_p2!");
		return 0;
	}
	dev->init_state_flags |= PIMAX_P2_INIT_STATE_HID_THREAD_RUNNING;


	dev->device_config.offset_h_0.min = OFFSETS_MIN;
	dev->device_config.offset_h_0.max = OFFSETS_MAX;
	dev->device_config.offset_h_0.step = OFFSETS_STEP;
	dev->device_config.offset_h_0.val = debug_get_float_option_pimax_ipd_offs_h0();

	dev->device_config.offset_h_1.min = OFFSETS_MIN;
	dev->device_config.offset_h_1.max = OFFSETS_MAX;
	dev->device_config.offset_h_1.step = OFFSETS_STEP;
	dev->device_config.offset_h_1.val = debug_get_float_option_pimax_ipd_offs_h1();

	dev->device_config.offset_v_0.min = OFFSETS_MIN;
	dev->device_config.offset_v_0.max = OFFSETS_MAX;
	dev->device_config.offset_v_0.step = OFFSETS_STEP;
	dev->device_config.offset_v_0.val = debug_get_float_option_pimax_ipd_offs_v0();

	dev->device_config.offset_v_1.min = OFFSETS_MIN;
	dev->device_config.offset_v_1.max = OFFSETS_MAX;
	dev->device_config.offset_v_1.step = OFFSETS_STEP;
	dev->device_config.offset_v_1.val = debug_get_float_option_pimax_ipd_offs_v1();

	return 3L * (time_duration_ns)U_TIME_1S_IN_NS;
}


/*
 *
 * 'Exported' functions.
 *
 */

// mostly copied from the vive driver
xrt_result_t
pimax_p2_get_view_poses(struct pimax_p2_device *dev,
                        const struct xrt_vec3 *default_eye_relation,
                        struct xrt_hmd_parts *hmd,
                        uint32_t view_count,
                        struct xrt_fov *out_fovs,
                        struct xrt_pose *out_poses)
{
	struct xrt_vec3 newEyeRelation = *default_eye_relation;
	newEyeRelation.x = dev->device_config.ipd;

	for (uint32_t i = 0; i < view_count && i < ARRAY_SIZE(hmd->views); i++) {
		out_fovs[i] = hmd->distortion.fov[i];
	}

	for (uint32_t view_index = 0; view_index < view_count; view_index++) {
		struct xrt_pose pose = XRT_POSE_IDENTITY;
		bool adjust = view_index == 0;

		float v_adjust = 0.0;
		float h_adjust = 0.0;
		if (view_index == 0) {
			h_adjust = dev->device_config.offset_h_0.val;
			v_adjust = dev->device_config.offset_v_0.val;
		} else {
			h_adjust = dev->device_config.offset_h_1.val;
			v_adjust = dev->device_config.offset_v_1.val;
		}

		pose.position.x = (newEyeRelation.x / 2.0f) + h_adjust;
		pose.position.y = (newEyeRelation.y / 2.0f) + v_adjust;
		pose.position.z = newEyeRelation.z / 2.0f;

		// Adjust for left/right while also making sure there aren't any -0.f.
		if (pose.position.x != 0.0f && adjust) {
			pose.position.x = -pose.position.x;
		}
		if (pose.position.y != 0.0f && adjust) {
			pose.position.y = -pose.position.y;
		}
		if (pose.position.z != 0.0f && adjust) {
			pose.position.z = -pose.position.z;
		}

		out_poses[view_index] = pose;
	}

	//  canted displays (10°), based on PiTools output
	//  ONLY WORKS IF LIGHTHOUSES ARE ON!!!

	out_poses[0].orientation = (struct xrt_quat){0, 0.0871557, 0, 0.996195};
	out_poses[1].orientation = (struct xrt_quat){0, -0.0871557, 0, 0.996195};

	return XRT_SUCCESS;
}

// width and height here refer to how the displays are located in the hmd
static void
pimax_p2_fill_display(struct xrt_hmd_parts *hmd, struct pimax_p2_display_properties *display_props)
{
	hmd->screens[0].w_pixels = 2 * display_props->pixels_height;
	hmd->screens[0].h_pixels = display_props->pixels_width;
	for (int i = 0; i < 2; i++) {
		hmd->views[i].viewport.w_pixels = display_props->pixels_height;
		hmd->views[i].viewport.h_pixels = display_props->pixels_width;
		hmd->views[i].viewport.x_pixels = i * display_props->pixels_height;
		hmd->views[i].viewport.y_pixels = 0;
		hmd->views[i].display.w_pixels = display_props->pixels_width;
		hmd->views[i].display.h_pixels = display_props->pixels_height;
		hmd->views[i].rot.v[0] = 0;
		hmd->views[i].rot.v[1] = i ? 1 : -1;
		hmd->views[i].rot.v[2] = i ? -1 : 1;
		hmd->views[i].rot.v[3] = 0;
	}
}

xrt_result_t
pimax_p2_get_brightness(struct pimax_p2_device *dev, float *out_brightness)
{
	os_mutex_lock(&dev->hid_mutex);
	pimax_p2_8kx_read_config(dev);
	os_mutex_unlock(&dev->hid_mutex);
	*out_brightness = (float)dev->device_config.brightness / 100;
	return XRT_SUCCESS;
}

xrt_result_t
pimax_p2_set_brightness(struct pimax_p2_device *dev, float brightness, bool relative)
{
	os_mutex_lock(&dev->hid_mutex);
	if (relative) {
		pimax_p2_8kx_read_config(dev);
		brightness += dev->device_config.brightness;
	}
	uint8_t cmd[64] = {0xf0, 0x00, 0x00, 0x08};
	uint8_t bright_int = (uint8_t)roundf(brightness * 100);
	bright_int = CLAMP(bright_int, 0, 100);
	cmd[30] = bright_int;
	int ret = hid_send_feature_report(dev->hid_dev, cmd, sizeof(cmd));
	os_mutex_unlock(&dev->hid_mutex);

	return ret ? XRT_ERROR_USB_COMMAND_FAILED : XRT_SUCCESS;
}

xrt_result_t
pimax_p2_compute_distortion_from_mesh(
    struct pimax_p2_device *dev, uint32_t view, float u, float v, struct xrt_uv_triplet *out_result)
{
	// if no mesh got loaded, do no distortion
	if (!dev->mesh_set) {
		out_result->r.x = u;
		out_result->r.y = v;
		out_result->g.x = u;
		out_result->g.y = v;
		out_result->b.x = u;
		out_result->b.y = v;
		return true;
	}

	float separation = dev->device_config.ipd;
	if (view == 0) {
		separation += dev->device_config.offset_h_0.val * 2;
	} else {
		separation += dev->device_config.offset_h_1.val * 2;
	}

	// first, find the two closest meshes, could also be done in the poll function (probably better)
	int lower_mesh_idx = -1;
	int upper_mesh_idx = -1;
	// this assumes the entries are sorted with ascending lens separation
	for (int i = dev->mesh_set->mesh_count - 1; i >= 0; i--) {
		if (dev->mesh_set->meshes[i].ipd < separation) {
			lower_mesh_idx = i;
			if (i == (int)dev->mesh_set->mesh_count - 1) {
				if (dev->mesh_set->mesh_count > 1)
					upper_mesh_idx = dev->mesh_set->mesh_count - 1;
				lower_mesh_idx = i - 1;
			} else {
				upper_mesh_idx = i + 1;
			}
			break;
		}
	}
	if (lower_mesh_idx == -1 || upper_mesh_idx == -1) {
		lower_mesh_idx = upper_mesh_idx = 0;
		if (dev->mesh_set->mesh_count > 1)
			upper_mesh_idx = 1;
	}

	float range = dev->mesh_set->meshes[upper_mesh_idx].ipd - dev->mesh_set->meshes[lower_mesh_idx].ipd;
	struct xrt_vec2 pos = {u, v};

	int mesh_steps_u = 65;
	int mesh_steps_v = 65;
	int stride_in_floats = 8;

	int u_lower = floorf(u * (mesh_steps_u - 1));
	int u_upper = ceilf(u * (mesh_steps_u - 1));
	int v_lower = floorf(v * (mesh_steps_v - 1));
	int v_upper = ceilf(v * (mesh_steps_v - 1));

	int idx_ulvl = v_lower * mesh_steps_u + u_lower;
	int idx_uuvl = v_lower * mesh_steps_u + u_upper;
	int idx_ulvu = v_upper * mesh_steps_u + u_lower;
	int idx_uuvu = v_upper * mesh_steps_u + u_upper;
	struct xrt_uv_triplet triplets[2];
	for (int i = 0; i < 2; i++) {

		int mesh_idx = i ? upper_mesh_idx : lower_mesh_idx;

		struct xrt_vec2 vec_ulvl = *(
		    struct xrt_vec2 *)(&dev->mesh_set->meshes[mesh_idx].views[view].data[idx_ulvl * stride_in_floats]);
		struct xrt_vec2 vec_uuvl = *(
		    struct xrt_vec2 *)(&dev->mesh_set->meshes[mesh_idx].views[view].data[idx_uuvl * stride_in_floats]);
		// struct xrt_vec2 vec_ulvu = *(struct
		// xrt_vec2*)(&dev->mesh_set->meshes[mesh_idx].views[view].data[idx_ulvu * stride_in_floats]);
		struct xrt_vec2 vec_uuvu = *(
		    struct xrt_vec2 *)(&dev->mesh_set->meshes[mesh_idx].views[view].data[idx_uuvu * stride_in_floats]);

		struct xrt_uv_triplet triplet_uvl;
		struct xrt_uv_triplet triplet_uvu;
		if (u_lower == u_upper) {
			triplet_uvl = *(struct xrt_uv_triplet *)(&dev->mesh_set->meshes[mesh_idx]
			                                              .views[view]
			                                              .data[idx_uuvl * stride_in_floats + 2]);
			triplet_uvu = *(struct xrt_uv_triplet *)(&dev->mesh_set->meshes[mesh_idx]
			                                              .views[view]
			                                              .data[idx_uuvu * stride_in_floats + 2]);
		} else {

			struct xrt_uv_triplet triplet_ulvl = *(
			    struct xrt_uv_triplet
			        *)(&dev->mesh_set->meshes[mesh_idx].views[view].data[idx_ulvl * stride_in_floats + 2]);
			struct xrt_uv_triplet triplet_uuvl = *(
			    struct xrt_uv_triplet
			        *)(&dev->mesh_set->meshes[mesh_idx].views[view].data[idx_uuvl * stride_in_floats + 2]);
			struct xrt_uv_triplet triplet_ulvu = *(
			    struct xrt_uv_triplet
			        *)(&dev->mesh_set->meshes[mesh_idx].views[view].data[idx_ulvu * stride_in_floats + 2]);
			struct xrt_uv_triplet triplet_uuvu = *(
			    struct xrt_uv_triplet
			        *)(&dev->mesh_set->meshes[mesh_idx].views[view].data[idx_uuvu * stride_in_floats + 2]);

			// interpolate u first
			float amount = (pos.x - vec_ulvl.x) / (vec_uuvl.x - vec_ulvl.x);
			triplet_uvl = uv_triplet_lerp(triplet_ulvl, triplet_uuvl, amount);
			triplet_uvu = uv_triplet_lerp(triplet_ulvu, triplet_uuvu, amount);
		}
		if (v_upper == v_lower) {
			triplets[i] = triplet_uvl;
		} else {
			// now v
			float amount = (pos.y - vec_ulvl.y) / (vec_uuvu.y - vec_ulvl.y);
			triplets[i] = uv_triplet_lerp(triplet_uvl, triplet_uvu, amount);
		}
	}
	if (upper_mesh_idx == lower_mesh_idx) {
		*out_result = triplets[0];
	} else {
		float amount = (separation - dev->mesh_set->meshes[upper_mesh_idx].ipd) / range;
		*out_result = uv_triplet_lerp(triplets[0], triplets[1], amount);
	}
	float dir = view ? -1.f : 1.f;
	struct pimax_p2_display_properties props;
	dev->model_funcs->get_display_properties(dev, &props);
	out_result->r.x += (debug_get_float_option_pimax_img_offs_x_r() / (float)props.pixels_width) * dir;
	out_result->g.x += (debug_get_float_option_pimax_img_offs_x_g() / (float)props.pixels_width) * dir;
	out_result->b.x += (debug_get_float_option_pimax_img_offs_x_b() / (float)props.pixels_width) * dir;

	return XRT_SUCCESS;
}

bool
pimax_p2_init(struct pimax_p2_device **pdev)
{
	struct hid_device_info *devinfo = hid_enumerate(PIMAX_P2_VID, PIMAX_P2_PID);
	if (!devinfo) {
		return false;
	}
	struct hid_device_info *current = devinfo;
	time_duration_ns delay = 0;
	while (current && *pdev == NULL) {
		delay = init_p2_pimax8kx(current, pdev);
		current = current->next;
	}
	hid_free_enumeration(devinfo);

	os_nanosleep(delay);
	if (*pdev == NULL) {
		return false;
	}

	// setup debug GUI
	u_var_add_root(*pdev, "Pimax HMD", true);
	u_var_add_ro_f32(*pdev, &(*pdev)->device_config.ipd, "Current IPD");
	u_var_add_gui_header(*pdev, NULL, "Horizontal offset: ");
	u_var_add_draggable_f32(*pdev, &(*pdev)->device_config.offset_h_0, "Left H");
	u_var_add_draggable_f32(*pdev, &(*pdev)->device_config.offset_h_1, "Right H");

	u_var_add_gui_header(*pdev, NULL, "Vertical offset: ");
	u_var_add_draggable_f32(*pdev, &(*pdev)->device_config.offset_v_0, "Left V");
	u_var_add_draggable_f32(*pdev, &(*pdev)->device_config.offset_v_1, "Right V");

	return true;
}

void
pimax_p2_destroy(struct pimax_p2_device *dev)
{
	U_LOG_D("Pimax destroy\n");
	dev->should_poll = false;
	if (dev->init_state_flags & PIMAX_P2_INIT_STATE_HID_THREAD_RUNNING) {
		os_thread_join(&dev->poll_thread);
	}
	if (dev->init_state_flags & PIMAX_P2_INIT_STATE_HID_THREAD) {
		os_thread_destroy(&dev->poll_thread);
	}
	if (dev->init_state_flags & PIMAX_P2_INIT_STATE_HID_MUTEX && dev->hid_dev) {
		os_mutex_lock(&dev->hid_mutex);
		hid_close(dev->hid_dev);
		os_mutex_unlock(&dev->hid_mutex);
	}
	if (dev->init_state_flags & PIMAX_P2_INIT_STATE_HID_MUTEX) {
		os_mutex_destroy(&dev->hid_mutex);
	}

	// delete mesh
	if (dev->mesh_set) {
		pimax_p2_delete_mesh_set(dev);
	}

	free(dev);
}

bool
pimax_p2_setup_hmd(struct pimax_p2_device *pdev, struct xrt_hmd_parts *hmd)
{

	struct pimax_p2_display_properties display_props;
	pdev->model_funcs->get_display_properties(pdev, &display_props);
	pimax_p2_fill_display(hmd, &display_props);

	// copy FoV values
	pimax_p2_update_fovs_from_mesh(pdev, hmd);

	return true;
}

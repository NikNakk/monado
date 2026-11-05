// Copyright 2025-2026, Collabora, Ltd.
// Copyright 2026, Finn Zeruhn
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Hid implementation based on hidapi.
 * @author Simon Zeni <simon.zeni@collabora.com>
 * @author Finn Zeruhn <fzeruhn@icloud.com>
 * @ingroup aux_hid
 */

#include "h_hid.h"
#include "os/os_time.h"

#include "util/u_misc.h"
#include "util/u_time.h"

#include <hidapi.h>


/*!
 * @implements h_hid_device
 */
struct hid_hidapi
{
	struct h_hid_device base;

	hid_device *handle;
};

static int
h_hidapi_read(struct h_hid_device *ohdev, uint8_t *data, size_t length, int milliseconds)
{
	struct hid_hidapi *hadev = (struct hid_hidapi *)ohdev;

	// Any negative timeout blocks, hidapi on macOS only blocks for -1.
	return hid_read_timeout(hadev->handle, data, length, milliseconds < 0 ? -1 : milliseconds);
}

static int
h_hidapi_write(struct h_hid_device *ohdev, const uint8_t *data, size_t length)
{
	struct hid_hidapi *hadev = (struct hid_hidapi *)ohdev;

	return hid_write(hadev->handle, data, length);
}

static int
h_hidapi_get_feature(struct h_hid_device *ohdev, uint8_t report_num, uint8_t *data, size_t length)
{
	struct hid_hidapi *hadev = (struct hid_hidapi *)ohdev;

	// Like hidraw, hidapi expects the report number in the first byte of the buffer.
	data[0] = report_num;
	return hid_get_feature_report(hadev->handle, data, length);
}

static int
h_hidapi_get_physical_address(struct h_hid_device *ohdev, uint8_t *data, size_t length)
{
	// hidapi has nothing like HIDIOCGRAWPHYS.
	return -1;
}

static int
h_hidapi_get_feature_timeout(struct h_hid_device *ohdev, void *data, size_t length, uint32_t timeout)
{
	struct hid_hidapi *hadev = (struct hid_hidapi *)ohdev;
	int64_t end_ns = os_monotonic_get_ns() + (int64_t)timeout * U_TIME_1MS_IN_NS;

	// Like hidraw, retry until the report is ready or the timeout (in ms) is up. hidapi
	// doesn't say why a call failed, so unlike hidraw this retries on any error.
	int ret;
	do {
		ret = hid_get_feature_report(hadev->handle, data, length);
		if (ret >= 0) {
			break;
		}

		if (os_monotonic_get_ns() >= end_ns) {
			break;
		}

		os_nanosleep(U_TIME_1MS_IN_NS);
	} while (true);

	return ret;
}

static int
h_hidapi_set_feature(struct h_hid_device *ohdev, const uint8_t *data, size_t length)
{
	struct hid_hidapi *hadev = (struct hid_hidapi *)ohdev;

	return hid_send_feature_report(hadev->handle, data, length);
}

static void
h_hidapi_destroy(struct h_hid_device *ohdev)
{
	struct hid_hidapi *hadev = (struct hid_hidapi *)ohdev;

	hid_close(hadev->handle);
	free(hadev);
}

int
h_hid_open_hidapi(const char *path, struct h_hid_device **out_hid)
{
	hid_device *handle = hid_open_path(path);
	if (handle == NULL) {
		return -1;
	}

	struct hid_hidapi *hadev = U_TYPED_CALLOC(struct hid_hidapi);

	hadev->base.read = h_hidapi_read;
	hadev->base.write = h_hidapi_write;
	hadev->base.get_feature = h_hidapi_get_feature;
	hadev->base.get_feature_timeout = h_hidapi_get_feature_timeout;
	hadev->base.set_feature = h_hidapi_set_feature;
	hadev->base.get_physical_address = h_hidapi_get_physical_address;
	hadev->base.destroy = h_hidapi_destroy;
	hadev->handle = handle;

	*out_hid = &hadev->base;

	return 0;
}

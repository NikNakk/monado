// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Wrapper around OS native hid functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 *
 * @ingroup aux_hid
 */

#pragma once

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_config_have.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * @defgroup aux_hid HID
 * @ingroup aux
 *
 * @brief Wrapper around HID devices.
 */

/*!
 * @dir auxiliary/hid
 *
 * @brief @ref aux_hid files.
 */

/*!
 * @interface h_hid_device
 *
 * Representing a single hid interface on a device.
 */
struct h_hid_device
{
	int (*read)(struct h_hid_device *hid_dev, uint8_t *data, size_t size, int milliseconds);

	//! Optional OS receipt timestamp; NULL on backends that cannot provide it.
	int (*read_with_timestamp)(
	    struct h_hid_device *hid_dev, uint8_t *data, size_t size, int milliseconds, int64_t *out_timestamp_ns);

	int (*write)(struct h_hid_device *hid_dev, const uint8_t *data, size_t size);

	int (*get_feature)(struct h_hid_device *hid_dev, uint8_t report_num, uint8_t *data, size_t size);

	int (*get_feature_timeout)(struct h_hid_device *hid_dev, void *data, size_t size, uint32_t timeout);

	int (*set_feature)(struct h_hid_device *hid_dev, const uint8_t *data, size_t size);

	int (*get_physical_address)(struct h_hid_device *hid_dev, uint8_t *data, size_t size);

	void (*destroy)(struct h_hid_device *hid_dev);
};

/*!
 * Read the next input report, if any, from the given hid device.
 *
 * If milliseconds are negative, this call blocks indefinitely, 0 polls,
 * and positive will block for that amount of milliseconds.
 *
 * @public @memberof h_hid_device
 */
static inline int
h_hid_read(struct h_hid_device *hid_dev, uint8_t *data, size_t size, int milliseconds)
{
	return hid_dev->read(hid_dev, data, size, milliseconds);
}

static inline int
h_hid_read_with_timestamp(
    struct h_hid_device *hid_dev, uint8_t *data, size_t size, int milliseconds, int64_t *out_timestamp_ns)
{
	if (out_timestamp_ns != NULL) {
		*out_timestamp_ns = 0;
	}
	if (hid_dev->read_with_timestamp != NULL) {
		return hid_dev->read_with_timestamp(hid_dev, data, size, milliseconds, out_timestamp_ns);
	}
	return hid_dev->read(hid_dev, data, size, milliseconds);
}

/*!
 * Write an output report to the given device.
 *
 * @public @memberof h_hid_device
 */
static inline int
h_hid_write(struct h_hid_device *hid_dev, const uint8_t *data, size_t size)
{
	return hid_dev->write(hid_dev, data, size);
}

/*!
 * Get a numbered feature report.
 *
 * If the device doesn't have more than one feature report, request report 0.
 *
 * @public @memberof h_hid_device
 */
static inline int
h_hid_get_feature(struct h_hid_device *hid_dev, uint8_t report_num, uint8_t *data, size_t size)
{
	return hid_dev->get_feature(hid_dev, report_num, data, size);
}

/*!
 * Get a feature report with a timeout.
 *
 * @public @memberof h_hid_device
 */
static inline int
h_hid_get_feature_timeout(struct h_hid_device *hid_dev, void *data, size_t size, uint32_t timeout)
{
	return hid_dev->get_feature_timeout(hid_dev, data, size, timeout);
}

/*!
 * Set a feature report.
 *
 * The first byte of the buffer is the report number, to be followed by
 * the data of the report.
 *
 * @public @memberof h_hid_device
 */
static inline int
h_hid_set_feature(struct h_hid_device *hid_dev, const uint8_t *data, size_t size)
{
	return hid_dev->set_feature(hid_dev, data, size);
}

/*!
 * Get the physical address.
 *
 * For USB devices, the string contains the physical path to the device (the
 * USB controller, hubs, ports, etc).  For Bluetooth *devices, the string
 * contains the hardware (MAC) address of the device.
 *
 * @public @memberof h_hid_device
 */
static inline int
h_hid_get_physical_address(struct h_hid_device *hid_dev, uint8_t *data, size_t size)
{
	return hid_dev->get_physical_address(hid_dev, data, size);
}

/*!
 * Close and free the given device.
 *
 * @public @memberof h_hid_device
 */
static inline void
h_hid_destroy(struct h_hid_device *hid_dev)
{
	hid_dev->destroy(hid_dev);
}

#ifdef XRT_OS_LINUX
/*!
 * Open the given path as a hidraw device.
 *
 * @see hid_hidraw
 * @public @memberof h_hid_device
 */
int
h_hid_open_hidraw(const char *path, struct h_hid_device **out_hid);
#endif

#if defined(XRT_HAVE_HIDAPI) && !defined(XRT_OS_LINUX)
/*!
 * Open the given path, as listed by hidapi's hid_enumerate, as a hidapi device.
 *
 * @see hid_hidapi
 * @public @memberof h_hid_device
 */
int
h_hid_open_hidapi(const char *path, struct h_hid_device **out_hid);
#endif

#ifdef XRT_OS_OSX
/*!
 * Wrap an IOHIDDeviceRef as an h_hid_device.
 *
 * The native device is retained by this function. The returned h_hid_device
 * owns that retain until h_hid_destroy() is called.
 *
 * void * is used here so IOKit types do not leak into the platform-neutral
 * h_hid.h interface.
 */
int
h_hid_open_iokit(void *native_device, struct h_hid_device **out_hid);

/*!
 * Find and open a Bluetooth HID device by vendor and product ID, without a prober: used to attach a device that
 * connects after start-up. @p out_product (optional) receives its product name.
 *
 * @return 0 on success, negative if no such device is connected or it could not be opened.
 */
int
h_hid_open_iokit_bluetooth(
    uint16_t vendor_id, uint16_t product_id, struct h_hid_device **out_hid, char *out_product, size_t product_size);
#endif

#ifdef __cplusplus
} // extern "C"
#endif

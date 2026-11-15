// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Hid implementation based on hidraw.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup aux_hid
 */

#include "h_hid.h"

#ifdef XRT_OS_LINUX

#include "os/os_fd.h"
#include "util/u_misc.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#include <sys/ioctl.h>

#include <linux/hidraw.h>


#include <stdio.h>

/*!
 * @implements h_hid_device
 */
struct hid_hidraw
{
	struct h_hid_device base;

	int fd;
};

static int
h_hidraw_read(struct h_hid_device *ohdev, uint8_t *data, size_t length, int milliseconds)
{
	struct hid_hidraw *hrdev = (struct hid_hidraw *)ohdev;

	return (int)os_fd_read(hrdev->fd, data, length, milliseconds);
}

static int
h_hidraw_write(struct h_hid_device *ohdev, const uint8_t *data, size_t length)
{
	struct hid_hidraw *hrdev = (struct hid_hidraw *)ohdev;

	return write(hrdev->fd, data, length);
}

static int
h_hidraw_get_feature(struct h_hid_device *ohdev, uint8_t report_num, uint8_t *data, size_t length)
{
	struct hid_hidraw *hrdev = (struct hid_hidraw *)ohdev;
	// The ioctl expects the report number in the first byte of the buffer,
	// but it will overwrite it.
	data[0] = report_num;
	return ioctl(hrdev->fd, HIDIOCGFEATURE(length), data);
}

static int
h_hidraw_get_physical_address(struct h_hid_device *ohdev, uint8_t *data, size_t length)
{
	struct hid_hidraw *hrdev = (struct hid_hidraw *)ohdev;
	return ioctl(hrdev->fd, HIDIOCGRAWPHYS(length), data);
}

static int
h_hidraw_get_feature_timeout(struct h_hid_device *ohdev, void *data, size_t length, uint32_t timeout)
{
	struct hid_hidraw *hrdev = (struct hid_hidraw *)ohdev;

	struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
	unsigned int i;
	int ret = 0;

	for (i = 0; i < timeout; i++) {
		ret = ioctl(hrdev->fd, HIDIOCGFEATURE(length), data);
		if (ret != -1 || errno != EPIPE)
			break;

		ts.tv_sec = 0;
		ts.tv_nsec = 1000000;
		nanosleep(&ts, NULL);
	}

	return ret;
}

static int
h_hidraw_set_feature(struct h_hid_device *ohdev, const uint8_t *data, size_t length)
{
	struct hid_hidraw *hrdev = (struct hid_hidraw *)ohdev;

	return ioctl(hrdev->fd, HIDIOCSFEATURE(length), data);
}

static void
h_hidraw_destroy(struct h_hid_device *ohdev)
{
	struct hid_hidraw *hrdev = (struct hid_hidraw *)ohdev;

	close(hrdev->fd);
	free(hrdev);
}

int
h_hid_open_hidraw(const char *path, struct h_hid_device **out_hid)
{
	struct hid_hidraw *hrdev = U_TYPED_CALLOC(struct hid_hidraw);

	hrdev->base.read = h_hidraw_read;
	hrdev->base.write = h_hidraw_write;
	hrdev->base.get_feature = h_hidraw_get_feature;
	hrdev->base.get_feature_timeout = h_hidraw_get_feature_timeout;
	hrdev->base.set_feature = h_hidraw_set_feature;
	hrdev->base.get_physical_address = h_hidraw_get_physical_address;
	hrdev->base.destroy = h_hidraw_destroy;
	hrdev->fd = open(path, O_RDWR);
	if (hrdev->fd < 0) {
		free(hrdev);
		return -errno;
	}

	*out_hid = &hrdev->base;

	return 0;
}

#endif

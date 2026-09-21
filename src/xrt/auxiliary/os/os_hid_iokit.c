// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief HID implementation based on macOS IOKit.
 * @ingroup aux_os
 */

#include "os_hid.h"

#ifdef XRT_OS_OSX

#include "util/u_misc.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IOKIT_HID_MAX_QUEUED_REPORTS 128
#define IOKIT_HID_FALLBACK_MAX_REPORT_SIZE 1024

struct iokit_input_report
{
	uint8_t *data;
	size_t length;
	struct iokit_input_report *next;
};

/*!
 * @implements os_hid_device
 */
struct hid_iokit
{
	struct os_hid_device base;

	IOHIDDeviceRef device;
	uint8_t *input_report_buffer;
	CFIndex max_input_report_length;

	pthread_t thread;
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	bool thread_started;
	bool thread_ready;
	bool running;
	bool disconnected;
	CFRunLoopRef run_loop;

	struct iokit_input_report *reports_head;
	struct iokit_input_report *reports_tail;
	size_t report_count;
};

static CFIndex
iokit_get_int_property(IOHIDDeviceRef device, CFStringRef key)
{
	CFTypeRef value = IOHIDDeviceGetProperty(device, key);
	if (value == NULL || CFGetTypeID(value) != CFNumberGetTypeID()) {
		return 0;
	}

	CFIndex result = 0;
	if (!CFNumberGetValue((CFNumberRef)value, kCFNumberCFIndexType, &result)) {
		return 0;
	}

	return result;
}

static void
iokit_free_report(struct iokit_input_report *report)
{
	if (report == NULL) {
		return;
	}

	free(report->data);
	free(report);
}

static void
iokit_drop_oldest_report_locked(struct hid_iokit *hid)
{
	struct iokit_input_report *report = hid->reports_head;
	if (report == NULL) {
		return;
	}

	hid->reports_head = report->next;
	if (hid->reports_head == NULL) {
		hid->reports_tail = NULL;
	}
	if (hid->report_count > 0) {
		hid->report_count--;
	}

	iokit_free_report(report);
}

static void
iokit_input_report_callback(void *context,
                            IOReturn result,
                            void *sender,
                            IOHIDReportType report_type,
                            uint32_t report_id,
                            uint8_t *report,
                            CFIndex report_length)
{
	(void)sender;
	(void)report_type;
	(void)report_id;

	struct hid_iokit *hid = (struct hid_iokit *)context;
	if (result != kIOReturnSuccess || report == NULL || report_length <= 0) {
		return;
	}

	struct iokit_input_report *queued = U_TYPED_CALLOC(struct iokit_input_report);
	if (queued == NULL) {
		return;
	}

	queued->data = U_TYPED_ARRAY_CALLOC(uint8_t, (size_t)report_length);
	if (queued->data == NULL) {
		free(queued);
		return;
	}
	memcpy(queued->data, report, (size_t)report_length);
	queued->length = (size_t)report_length;

	pthread_mutex_lock(&hid->mutex);
	if (!hid->running || hid->disconnected) {
		pthread_mutex_unlock(&hid->mutex);
		iokit_free_report(queued);
		return;
	}

	while (hid->report_count >= IOKIT_HID_MAX_QUEUED_REPORTS) {
		iokit_drop_oldest_report_locked(hid);
	}

	if (hid->reports_tail != NULL) {
		hid->reports_tail->next = queued;
	} else {
		hid->reports_head = queued;
	}
	hid->reports_tail = queued;
	hid->report_count++;
	pthread_cond_signal(&hid->condition);
	pthread_mutex_unlock(&hid->mutex);
}

static void
iokit_removal_callback(void *context, IOReturn result, void *sender)
{
	(void)result;
	(void)sender;

	struct hid_iokit *hid = (struct hid_iokit *)context;
	CFRunLoopRef run_loop = NULL;

	pthread_mutex_lock(&hid->mutex);
	hid->disconnected = true;
	if (hid->run_loop != NULL) {
		run_loop = hid->run_loop;
		CFRetain(run_loop);
	}
	pthread_cond_broadcast(&hid->condition);
	pthread_mutex_unlock(&hid->mutex);

	if (run_loop != NULL) {
		CFRunLoopStop(run_loop);
		CFRelease(run_loop);
	}
}

static void *
iokit_read_thread(void *ptr)
{
	struct hid_iokit *hid = (struct hid_iokit *)ptr;
	CFRunLoopRef run_loop = CFRunLoopGetCurrent();
	CFRetain(run_loop);

	IOHIDDeviceScheduleWithRunLoop(hid->device, run_loop, kCFRunLoopDefaultMode);

	pthread_mutex_lock(&hid->mutex);
	hid->run_loop = run_loop;
	hid->thread_ready = true;
	pthread_cond_broadcast(&hid->condition);
	pthread_mutex_unlock(&hid->mutex);

	for (;;) {
		pthread_mutex_lock(&hid->mutex);
		bool keep_running = hid->running && !hid->disconnected;
		pthread_mutex_unlock(&hid->mutex);
		if (!keep_running) {
			break;
		}

		SInt32 code = CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
		if (code == kCFRunLoopRunFinished || code == kCFRunLoopRunStopped) {
			pthread_mutex_lock(&hid->mutex);
			bool requested_stop = !hid->running || hid->disconnected;
			pthread_mutex_unlock(&hid->mutex);
			if (requested_stop) {
				break;
			}
		}
	}

	IOHIDDeviceUnscheduleFromRunLoop(hid->device, run_loop, kCFRunLoopDefaultMode);

	pthread_mutex_lock(&hid->mutex);
	hid->run_loop = NULL;
	pthread_cond_broadcast(&hid->condition);
	pthread_mutex_unlock(&hid->mutex);

	CFRelease(run_loop);
	return NULL;
}

static int
iokit_wait_for_report_locked(struct hid_iokit *hid, int milliseconds)
{
	if (milliseconds == 0) {
		return 0;
	}

	if (milliseconds < 0) {
		while (hid->reports_head == NULL && hid->running && !hid->disconnected) {
			int ret = pthread_cond_wait(&hid->condition, &hid->mutex);
			if (ret != 0) {
				return -1;
			}
		}
		return 0;
	}

	struct timespec deadline = {0};
	if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
		return -1;
	}
	deadline.tv_sec += milliseconds / 1000;
	deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	while (hid->reports_head == NULL && hid->running && !hid->disconnected) {
		int ret = pthread_cond_timedwait(&hid->condition, &hid->mutex, &deadline);
		if (ret == ETIMEDOUT) {
			return 0;
		}
		if (ret != 0) {
			return -1;
		}
	}

	return 0;
}

static int
iokit_read(struct os_hid_device *ohdev, uint8_t *data, size_t length, int milliseconds)
{
	struct hid_iokit *hid = (struct hid_iokit *)ohdev;
	if (data == NULL || length == 0) {
		return -1;
	}

	pthread_mutex_lock(&hid->mutex);
	if (hid->reports_head == NULL) {
		int ret = iokit_wait_for_report_locked(hid, milliseconds);
		if (ret != 0) {
			pthread_mutex_unlock(&hid->mutex);
			return -1;
		}
	}

	if (hid->reports_head == NULL) {
		bool disconnected = hid->disconnected;
		pthread_mutex_unlock(&hid->mutex);
		return disconnected ? -1 : 0;
	}

	struct iokit_input_report *report = hid->reports_head;
	hid->reports_head = report->next;
	if (hid->reports_head == NULL) {
		hid->reports_tail = NULL;
	}
	if (hid->report_count > 0) {
		hid->report_count--;
	}
	pthread_mutex_unlock(&hid->mutex);

	size_t copy_length = report->length < length ? report->length : length;
	memcpy(data, report->data, copy_length);
	iokit_free_report(report);

	return (int)copy_length;
}

static int
iokit_set_report(struct hid_iokit *hid, IOHIDReportType type, const uint8_t *data, size_t length)
{
	if (data == NULL || length == 0) {
		return -1;
	}

	pthread_mutex_lock(&hid->mutex);
	bool disconnected = hid->disconnected;
	pthread_mutex_unlock(&hid->mutex);
	if (disconnected) {
		return -1;
	}

	uint8_t report_id = data[0];
	const uint8_t *report = data;
	CFIndex report_length = (CFIndex)length;
	if (report_id == 0) {
		report = data + 1;
		report_length--;
	}

	IOReturn ret = IOHIDDeviceSetReport(hid->device, type, report_id, report, report_length);
	return ret == kIOReturnSuccess ? (int)length : -1;
}

static int
iokit_write(struct os_hid_device *ohdev, const uint8_t *data, size_t length)
{
	return iokit_set_report((struct hid_iokit *)ohdev, kIOHIDReportTypeOutput, data, length);
}

static int
iokit_get_feature(struct os_hid_device *ohdev, uint8_t report_num, uint8_t *data, size_t length)
{
	struct hid_iokit *hid = (struct hid_iokit *)ohdev;
	if (data == NULL || length == 0) {
		return -1;
	}

	pthread_mutex_lock(&hid->mutex);
	bool disconnected = hid->disconnected;
	pthread_mutex_unlock(&hid->mutex);
	if (disconnected) {
		return -1;
	}

	data[0] = report_num;
	uint8_t *report = data;
	CFIndex report_length = (CFIndex)length;
	if (report_num == 0) {
		report = data + 1;
		report_length--;
	}

	IOReturn ret = IOHIDDeviceGetReport(hid->device, kIOHIDReportTypeFeature, report_num, report, &report_length);
	if (ret != kIOReturnSuccess) {
		return -1;
	}

	if (report_num == 0) {
		report_length++;
	}

	return (int)report_length;
}

static int
iokit_get_feature_timeout(struct os_hid_device *ohdev, void *data, size_t length, uint32_t timeout)
{
	(void)timeout;
	if (data == NULL || length == 0) {
		return -1;
	}

	uint8_t *bytes = (uint8_t *)data;
	return iokit_get_feature(ohdev, bytes[0], bytes, length);
}

static int
iokit_set_feature(struct os_hid_device *ohdev, const uint8_t *data, size_t length)
{
	return iokit_set_report((struct hid_iokit *)ohdev, kIOHIDReportTypeFeature, data, length);
}

static int
iokit_get_physical_address(struct os_hid_device *ohdev, uint8_t *data, size_t length)
{
	struct hid_iokit *hid = (struct hid_iokit *)ohdev;
	if (data == NULL || length == 0) {
		return -1;
	}

	CFTypeRef value = IOHIDDeviceGetProperty(hid->device, CFSTR(kIOHIDSerialNumberKey));
	if (value == NULL || CFGetTypeID(value) != CFStringGetTypeID()) {
		return -1;
	}

	if (!CFStringGetCString((CFStringRef)value, (char *)data, (CFIndex)length, kCFStringEncodingUTF8)) {
		return -1;
	}

	return (int)strlen((const char *)data);
}

static void
iokit_destroy(struct os_hid_device *ohdev)
{
	struct hid_iokit *hid = (struct hid_iokit *)ohdev;
	CFRunLoopRef run_loop = NULL;

	pthread_mutex_lock(&hid->mutex);
	hid->running = false;
	if (hid->run_loop != NULL) {
		run_loop = hid->run_loop;
		CFRetain(run_loop);
	}
	pthread_cond_broadcast(&hid->condition);
	pthread_mutex_unlock(&hid->mutex);

	if (run_loop != NULL) {
		CFRunLoopStop(run_loop);
		CFRelease(run_loop);
	}

	if (hid->thread_started) {
		pthread_join(hid->thread, NULL);
	}

	pthread_mutex_lock(&hid->mutex);
	while (hid->reports_head != NULL) {
		iokit_drop_oldest_report_locked(hid);
	}
	pthread_mutex_unlock(&hid->mutex);

	IOHIDDeviceClose(hid->device, kIOHIDOptionsTypeNone);
	CFRelease(hid->device);

	free(hid->input_report_buffer);
	pthread_cond_destroy(&hid->condition);
	pthread_mutex_destroy(&hid->mutex);
	free(hid);
}

int
os_hid_open_iokit(void *native_device, struct os_hid_device **out_hid)
{
	if (native_device == NULL || out_hid == NULL) {
		return -1;
	}

	struct hid_iokit *hid = U_TYPED_CALLOC(struct hid_iokit);
	if (hid == NULL) {
		return -1;
	}

	if (pthread_mutex_init(&hid->mutex, NULL) != 0) {
		free(hid);
		return -1;
	}
	if (pthread_cond_init(&hid->condition, NULL) != 0) {
		pthread_mutex_destroy(&hid->mutex);
		free(hid);
		return -1;
	}

	hid->device = (IOHIDDeviceRef)native_device;
	CFRetain(hid->device);

	IOReturn open_ret = IOHIDDeviceOpen(hid->device, kIOHIDOptionsTypeNone);
	if (open_ret != kIOReturnSuccess) {
		CFRelease(hid->device);
		pthread_cond_destroy(&hid->condition);
		pthread_mutex_destroy(&hid->mutex);
		free(hid);
		return -1;
	}

	hid->max_input_report_length = iokit_get_int_property(hid->device, CFSTR(kIOHIDMaxInputReportSizeKey));
	if (hid->max_input_report_length <= 0) {
		hid->max_input_report_length = IOKIT_HID_FALLBACK_MAX_REPORT_SIZE;
	}
	hid->input_report_buffer = U_TYPED_ARRAY_CALLOC(uint8_t, (size_t)hid->max_input_report_length);
	if (hid->input_report_buffer == NULL) {
		IOHIDDeviceClose(hid->device, kIOHIDOptionsTypeNone);
		CFRelease(hid->device);
		pthread_cond_destroy(&hid->condition);
		pthread_mutex_destroy(&hid->mutex);
		free(hid);
		return -1;
	}

	hid->base.read = iokit_read;
	hid->base.write = iokit_write;
	hid->base.get_feature = iokit_get_feature;
	hid->base.get_feature_timeout = iokit_get_feature_timeout;
	hid->base.set_feature = iokit_set_feature;
	hid->base.get_physical_address = iokit_get_physical_address;
	hid->base.destroy = iokit_destroy;

	hid->running = true;
	IOHIDDeviceRegisterInputReportCallback(hid->device, hid->input_report_buffer, hid->max_input_report_length,
	                                       iokit_input_report_callback, hid);
	IOHIDDeviceRegisterRemovalCallback(hid->device, iokit_removal_callback, hid);

	int thread_ret = pthread_create(&hid->thread, NULL, iokit_read_thread, hid);
	if (thread_ret != 0) {
		hid->running = false;
		IOHIDDeviceClose(hid->device, kIOHIDOptionsTypeNone);
		CFRelease(hid->device);
		free(hid->input_report_buffer);
		pthread_cond_destroy(&hid->condition);
		pthread_mutex_destroy(&hid->mutex);
		free(hid);
		return -thread_ret;
	}
	hid->thread_started = true;

	pthread_mutex_lock(&hid->mutex);
	while (!hid->thread_ready) {
		pthread_cond_wait(&hid->condition, &hid->mutex);
	}
	pthread_mutex_unlock(&hid->mutex);

	*out_hid = &hid->base;
	return 0;
}

#endif // XRT_OS_OSX

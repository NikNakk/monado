// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief HID implementation based on macOS IOKit.
 * @ingroup aux_os
 */

#include "os_hid.h"
#include "os_time.h"

#ifdef XRT_OS_OSX

#include "util/u_misc.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IOKIT_HID_MAX_QUEUED_REPORTS 128
#define IOKIT_HID_FALLBACK_MAX_REPORT_SIZE 1024

/*
 * PS VR2 Sense diagnostic constants.
 *
 * This deliberately lives in the macOS HID backend as a temporary diagnostic
 * override: with PSSENSE_FORCE_IR=1, normal Sense Bluetooth output reports are
 * left intact except for the tracking-LED fields and CRC. This lets us prove
 * the controller output/camera path without changing the normal LED-sync
 * algorithm in the pssense driver.
 */
#define PSSENSE_VID 0x054c
#define PSSENSE_PID_LEFT 0x0e45
#define PSSENSE_PID_RIGHT 0x0e46
#define PSSENSE_BT_REPORT_ID 0x31
#define PSSENSE_BT_REPORT_LENGTH 78
#define PSSENSE_HOST_TIMESTAMP_OFFSET 18
#define PSSENSE_DEVICE_TIMESTAMP_OFFSET 49
#define PSSENSE_LED_SETTINGS_OFFSET 22
#define PSSENSE_PACKET_CRC_OFFSET 74
#define PSSENSE_OUTPUT_CRC_SEED 0xa2
#define PSSENSE_LED_PHASE_PRESCAN 1
#define PSSENSE_LED_PERIOD_ID 42
#define PSSENSE_FORCE_IR_CYCLE_NS 2000000ULL
#define PSSENSE_FORCE_IR_LEAD_NS 50000000ULL
#define PSSENSE_PERIOD_ID_UNIT_NS 50000ULL

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

	bool logged_output_success;

	bool is_pssense;
	char pssense_side;
	bool force_pssense_ir;
	bool force_pssense_ir_programmed;
	bool force_pssense_ir_wait_logged;
	uint8_t force_pssense_ir_led_sequence;
	uint32_t force_pssense_ir_cycle_position;
	bool have_pssense_device_timestamp;
	uint32_t pssense_device_timestamp_ticks;
	uint64_t pssense_device_timestamp_host_ns;

	bool pssense_timing_diag;
	bool pssense_timing_diag_have_last;
	uint8_t pssense_timing_diag_last_phase;
	uint8_t pssense_timing_diag_last_sequence;
	uint8_t pssense_timing_diag_last_period_id;
	uint32_t pssense_timing_diag_last_cycle_position;
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

static bool
iokit_env_enabled(const char *name)
{
	const char *value = getenv(name);
	return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0 && strcmp(value, "false") != 0 &&
	       strcmp(value, "FALSE") != 0;
}

static uint32_t
iokit_read_le32(const uint8_t *data)
{
	return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void
iokit_write_le32(uint8_t *data, uint32_t value)
{
	data[0] = (uint8_t)(value & 0xff);
	data[1] = (uint8_t)((value >> 8) & 0xff);
	data[2] = (uint8_t)((value >> 16) & 0xff);
	data[3] = (uint8_t)((value >> 24) & 0xff);
}

static uint32_t
iokit_crc32_le(uint32_t crc, const uint8_t *data, size_t length)
{
	crc ^= 0xffffffff;
	while (length-- > 0) {
		crc ^= *data++;
		for (int i = 0; i < 8; i++) {
			crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320U : 0U);
		}
	}
	return crc ^ 0xffffffff;
}

static uint32_t
iokit_pssense_crc(const uint8_t *report)
{
	uint32_t crc = iokit_crc32_le(0, (const uint8_t[]){PSSENSE_OUTPUT_CRC_SEED}, 1);
	return iokit_crc32_le(crc, report, PSSENSE_PACKET_CRC_OFFSET);
}

static bool
iokit_is_pssense_device(IOHIDDeviceRef device)
{
	CFIndex vendor = iokit_get_int_property(device, CFSTR(kIOHIDVendorIDKey));
	CFIndex product = iokit_get_int_property(device, CFSTR(kIOHIDProductIDKey));
	return vendor == PSSENSE_VID && (product == PSSENSE_PID_LEFT || product == PSSENSE_PID_RIGHT);
}

static char
iokit_pssense_side(IOHIDDeviceRef device)
{
	CFIndex product = iokit_get_int_property(device, CFSTR(kIOHIDProductIDKey));
	if (product == PSSENSE_PID_LEFT) {
		return 'L';
	}
	if (product == PSSENSE_PID_RIGHT) {
		return 'R';
	}
	return '?';
}

static void
iokit_update_pssense_clock_locked(struct hid_iokit *hid, const uint8_t *report, size_t report_length)
{
	if (!hid->is_pssense || report_length < PSSENSE_DEVICE_TIMESTAMP_OFFSET + sizeof(uint32_t) ||
	    report[0] != PSSENSE_BT_REPORT_ID) {
		return;
	}

	hid->pssense_device_timestamp_ticks = iokit_read_le32(report + PSSENSE_DEVICE_TIMESTAMP_OFFSET);
	hid->pssense_device_timestamp_host_ns = os_monotonic_get_ns();
	hid->have_pssense_device_timestamp = true;
}

static void
iokit_log_pssense_timing_locked(struct hid_iokit *hid,
                                 const uint8_t *report,
                                 size_t report_length,
                                 bool force_ir_overridden)
{
	if (!hid->pssense_timing_diag || !hid->is_pssense || report_length != PSSENSE_BT_REPORT_LENGTH ||
	    report[0] != PSSENSE_BT_REPORT_ID) {
		return;
	}

	uint8_t phase = report[PSSENSE_LED_SETTINGS_OFFSET];
	uint8_t sequence = report[PSSENSE_LED_SETTINGS_OFFSET + 1];
	uint8_t period_id = report[PSSENSE_LED_SETTINGS_OFFSET + 2];
	uint32_t cycle_position = iokit_read_le32(report + PSSENSE_LED_SETTINGS_OFFSET + 3);
	uint32_t cycle_length_thirds_ns = iokit_read_le32(report + PSSENSE_LED_SETTINGS_OFFSET + 7);

	if (hid->pssense_timing_diag_have_last && phase == hid->pssense_timing_diag_last_phase &&
	    sequence == hid->pssense_timing_diag_last_sequence && period_id == hid->pssense_timing_diag_last_period_id &&
	    cycle_position == hid->pssense_timing_diag_last_cycle_position) {
		return;
	}

	hid->pssense_timing_diag_have_last = true;
	hid->pssense_timing_diag_last_phase = phase;
	hid->pssense_timing_diag_last_sequence = sequence;
	hid->pssense_timing_diag_last_period_id = period_id;
	hid->pssense_timing_diag_last_cycle_position = cycle_position;

	uint64_t now_ns = os_monotonic_get_ns();
	uint32_t host_timestamp_us = iokit_read_le32(report + PSSENSE_HOST_TIMESTAMP_OFFSET);
	uint64_t pulse_ns = (uint64_t)period_id * PSSENSE_PERIOD_ID_UNIT_NS;
	uint64_t cycle_ns = (uint64_t)cycle_length_thirds_ns / 3ULL;

	uint32_t estimated_device_ticks = 0;
	uint64_t clock_age_ns = 0;
	int64_t blink_delta_ns = 0;
	int64_t blink_host_est_ns = 0;
	bool clock_valid = hid->have_pssense_device_timestamp;
	if (clock_valid) {
		clock_age_ns = now_ns - hid->pssense_device_timestamp_host_ns;
		uint64_t elapsed_ticks = (clock_age_ns * 3ULL) / 1000ULL;
		estimated_device_ticks = hid->pssense_device_timestamp_ticks + (uint32_t)elapsed_ticks;
		int32_t blink_delta_ticks = (int32_t)(cycle_position - estimated_device_ticks);
		blink_delta_ns = ((int64_t)blink_delta_ticks * 1000LL) / 3LL;
		blink_host_est_ns = (int64_t)now_ns + blink_delta_ns;
	}

	fprintf(stderr,
	        "os_hid_iokit: PSSENSE_TIMING side=%c force_ir=%u host_now_ns=%llu host_report_us=%u phase=%u "
	        "led_seq=%u period_id=%u pulse_ns=%llu cycle_position=%u cycle_ns=%llu clock_valid=%u "
	        "device_ticks_est=%u clock_age_ns=%llu blink_delta_ns=%lld blink_host_est_ns=%lld masks=%02x%02x%02x%02x\n",
	        hid->pssense_side, force_ir_overridden ? 1U : 0U, (unsigned long long)now_ns, host_timestamp_us, phase,
	        sequence, period_id, (unsigned long long)pulse_ns, cycle_position, (unsigned long long)cycle_ns,
	        clock_valid ? 1U : 0U, estimated_device_ticks, (unsigned long long)clock_age_ns, (long long)blink_delta_ns,
	        (long long)blink_host_est_ns, report[PSSENSE_LED_SETTINGS_OFFSET + 11],
	        report[PSSENSE_LED_SETTINGS_OFFSET + 12], report[PSSENSE_LED_SETTINGS_OFFSET + 13],
	        report[PSSENSE_LED_SETTINGS_OFFSET + 14]);
}

static bool
iokit_force_pssense_ir_locked(struct hid_iokit *hid, uint8_t *report, size_t report_length)
{
	if (!hid->force_pssense_ir || !hid->is_pssense || report_length != PSSENSE_BT_REPORT_LENGTH ||
	    report[0] != PSSENSE_BT_REPORT_ID) {
		return false;
	}

	if (!hid->have_pssense_device_timestamp) {
		if (!hid->force_pssense_ir_wait_logged) {
			fprintf(stderr, "os_hid_iokit: PSSENSE_FORCE_IR waiting for controller device clock\n");
			hid->force_pssense_ir_wait_logged = true;
		}
		return false;
	}

	if (!hid->force_pssense_ir_programmed) {
		uint64_t now_ns = os_monotonic_get_ns();
		uint64_t elapsed_ns = now_ns - hid->pssense_device_timestamp_host_ns;
		uint64_t elapsed_ticks = (elapsed_ns * 3ULL) / 1000ULL;
		uint64_t lead_ticks = (PSSENSE_FORCE_IR_LEAD_NS * 3ULL) / 1000ULL;
		hid->force_pssense_ir_cycle_position =
		    hid->pssense_device_timestamp_ticks + (uint32_t)elapsed_ticks + (uint32_t)lead_ticks;
		hid->force_pssense_ir_led_sequence++;
		hid->force_pssense_ir_programmed = true;
		fprintf(stderr,
		        "os_hid_iokit: PSSENSE_FORCE_IR programmed PRESCAN: period_id=%u cycle=%.3fms lead=%.1fms "
		        "cycle_position=%u\n",
		        PSSENSE_LED_PERIOD_ID, (double)PSSENSE_FORCE_IR_CYCLE_NS / 1000000.0,
		        (double)PSSENSE_FORCE_IR_LEAD_NS / 1000000.0, hid->force_pssense_ir_cycle_position);
	}

	/*
	 * Optically verified continuous-equivalent PRESCAN pattern:
	 *  - period ID 42: ~2.1ms pulse
	 *  - 2.0ms repeating cycle, giving slight pulse overlap
	 *  - all four LED masks enabled
	 *
	 * cycle_length is encoded in thirds of a nanosecond; cycle_position is
	 * encoded in controller IMU ticks (one third of a microsecond).
	 */
	report[PSSENSE_LED_SETTINGS_OFFSET] = PSSENSE_LED_PHASE_PRESCAN;
	report[PSSENSE_LED_SETTINGS_OFFSET + 1] = hid->force_pssense_ir_led_sequence;
	report[PSSENSE_LED_SETTINGS_OFFSET + 2] = PSSENSE_LED_PERIOD_ID;
	iokit_write_le32(report + PSSENSE_LED_SETTINGS_OFFSET + 3, hid->force_pssense_ir_cycle_position);
	iokit_write_le32(report + PSSENSE_LED_SETTINGS_OFFSET + 7, (uint32_t)(PSSENSE_FORCE_IR_CYCLE_NS * 3ULL));
	memset(report + PSSENSE_LED_SETTINGS_OFFSET + 11, 0xff, 4);

	uint32_t crc = iokit_pssense_crc(report);
	iokit_write_le32(report + PSSENSE_PACKET_CRC_OFFSET, crc);
	return true;
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

	iokit_update_pssense_clock_locked(hid, report, (size_t)report_length);

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

static const char *
iokit_report_type_name(IOHIDReportType type)
{
	switch (type) {
	case kIOHIDReportTypeInput: return "input";
	case kIOHIDReportTypeOutput: return "output";
	case kIOHIDReportTypeFeature: return "feature";
	default: return "unknown";
	}
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
		fprintf(stderr, "os_hid_iokit: refusing %s report write after device disconnect\n", iokit_report_type_name(type));
		return -1;
	}

	uint8_t stack_report[PSSENSE_BT_REPORT_LENGTH];
	const uint8_t *send_data = data;
	bool overridden = false;
	if (type == kIOHIDReportTypeOutput && length == sizeof(stack_report)) {
		memcpy(stack_report, data, sizeof(stack_report));
		pthread_mutex_lock(&hid->mutex);
		overridden = iokit_force_pssense_ir_locked(hid, stack_report, sizeof(stack_report));
		pthread_mutex_unlock(&hid->mutex);
		if (overridden) {
			send_data = stack_report;
		}
	}

	if (type == kIOHIDReportTypeOutput) {
		pthread_mutex_lock(&hid->mutex);
		iokit_log_pssense_timing_locked(hid, send_data, length, overridden);
		pthread_mutex_unlock(&hid->mutex);
	}

	uint8_t report_id = send_data[0];
	const uint8_t *report = send_data;
	CFIndex report_length = (CFIndex)length;
	if (report_id == 0) {
		report = send_data + 1;
		report_length--;
	}

	IOReturn ret = IOHIDDeviceSetReport(hid->device, type, report_id, report, report_length);
	if (ret != kIOReturnSuccess) {
		fprintf(stderr,
		        "os_hid_iokit: IOHIDDeviceSetReport failed: type=%s id=0x%02x app_length=%zu iokit_length=%ld "
		        "IOReturn=0x%08x\n",
		        iokit_report_type_name(type), report_id, length, (long)report_length, (unsigned int)ret);
		return -1;
	}

	if (type == kIOHIDReportTypeOutput) {
		pthread_mutex_lock(&hid->mutex);
		if (!hid->logged_output_success) {
			hid->logged_output_success = true;
			fprintf(stderr,
			        "os_hid_iokit: IOHIDDeviceSetReport output OK: id=0x%02x app_length=%zu iokit_length=%ld\n",
			        report_id, length, (long)report_length);
		}
		pthread_mutex_unlock(&hid->mutex);
	}

	return (int)length;
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
	hid->is_pssense = iokit_is_pssense_device(hid->device);
	hid->pssense_side = iokit_pssense_side(hid->device);
	hid->force_pssense_ir = hid->is_pssense && iokit_env_enabled("PSSENSE_FORCE_IR");
	hid->pssense_timing_diag = hid->is_pssense && iokit_env_enabled("PSSENSE_TIMING_DIAG");
	if (hid->force_pssense_ir) {
		fprintf(stderr,
		        "os_hid_iokit: PSSENSE_FORCE_IR=1 enabled for PS VR2 Sense controller; overriding tracking LED "
		        "fields only\n");
	}
	if (hid->pssense_timing_diag) {
		fprintf(stderr, "os_hid_iokit: PSSENSE_TIMING_DIAG=1 enabled for Sense %c\n", hid->pssense_side);
	}

	IOReturn open_ret = IOHIDDeviceOpen(hid->device, kIOHIDOptionsTypeNone);
	if (open_ret != kIOReturnSuccess) {
		fprintf(stderr, "os_hid_iokit: IOHIDDeviceOpen failed: IOReturn=0x%08x\n", (unsigned int)open_ret);
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

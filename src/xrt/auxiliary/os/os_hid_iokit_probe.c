// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Small macOS-only PS Sense HID transport probe.
 */

#include "os_hid.h"

#ifdef XRT_OS_OSX

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSSENSE_VID 0x054c
#define PSSENSE_PID_LEFT 0x0e45
#define PSSENSE_PID_RIGHT 0x0e46
#define PSSENSE_CALIBRATION_REPORT_ID 0x05
#define PSSENSE_CALIBRATION_REPORT_LENGTH 64
#define PSSENSE_INPUT_REPORT_LENGTH 78
#define PSSENSE_INPUT_REPORT_ID 0x31

static int32_t
get_int_property(IOHIDDeviceRef device, CFStringRef key)
{
	CFTypeRef value = IOHIDDeviceGetProperty(device, key);
	if (value == NULL || CFGetTypeID(value) != CFNumberGetTypeID()) {
		return 0;
	}

	int32_t result = 0;
	if (!CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, &result)) {
		return 0;
	}

	return result;
}

static void
get_string_property(IOHIDDeviceRef device, CFStringRef key, char *out, size_t out_size)
{
	if (out_size == 0) {
		return;
	}

	out[0] = '\0';
	CFTypeRef value = IOHIDDeviceGetProperty(device, key);
	if (value == NULL || CFGetTypeID(value) != CFStringGetTypeID()) {
		return;
	}

	(void)CFStringGetCString((CFStringRef)value, out, (CFIndex)out_size, kCFStringEncodingUTF8);
}

static const char *
hand_name(uint16_t product_id)
{
	return product_id == PSSENSE_PID_LEFT ? "left" : "right";
}

static void
print_prefix(const uint8_t *data, size_t length)
{
	size_t n = length < 16 ? length : 16;
	for (size_t i = 0; i < n; i++) {
		printf("%02x%s", data[i], i + 1 == n ? "" : " ");
	}
}

static int
probe_controller(IOHIDDeviceRef device, uint16_t product_id, int read_count)
{
	char product[128] = {0};
	char serial[128] = {0};
	char transport[64] = {0};
	get_string_property(device, CFSTR(kIOHIDProductKey), product, sizeof(product));
	get_string_property(device, CFSTR(kIOHIDSerialNumberKey), serial, sizeof(serial));
	get_string_property(device, CFSTR(kIOHIDTransportKey), transport, sizeof(transport));

	printf("Found %s Sense controller: product='%s' serial='%s' transport='%s'\n", hand_name(product_id), product,
	       serial, transport);

	struct os_hid_device *hid = NULL;
	int ret = os_hid_open_iokit(device, &hid);
	if (ret != 0 || hid == NULL) {
		fprintf(stderr, "  open failed: %d\n", ret);
		return 1;
	}
	printf("  open: OK\n");

	for (int part = 0; part < 2; part++) {
		uint8_t calibration[PSSENSE_CALIBRATION_REPORT_LENGTH] = {0};
		ret = os_hid_get_feature(hid, PSSENSE_CALIBRATION_REPORT_ID, calibration, sizeof(calibration));
		if (ret < 0) {
			fprintf(stderr, "  calibration feature report %d: FAILED\n", part + 1);
			os_hid_destroy(hid);
			return 1;
		}

		printf("  calibration feature report %d: %d bytes, id=0x%02x part=0x%02x prefix=", part + 1, ret,
		       calibration[0], ret > 1 ? calibration[1] : 0);
		print_prefix(calibration, (size_t)ret);
		printf("\n");
	}

	printf("  waiting for %d full input report%s...\n", read_count, read_count == 1 ? "" : "s");
	int received = 0;
	for (int i = 0; i < read_count; i++) {
		uint8_t report[PSSENSE_INPUT_REPORT_LENGTH] = {0};
		ret = os_hid_read(hid, report, sizeof(report), 2000);
		if (ret == 0) {
			printf("  input %d: timeout\n", i + 1);
			continue;
		}
		if (ret < 0) {
			printf("  input %d: read failed/disconnected\n", i + 1);
			break;
		}

		printf("  input %d: %d bytes, id=0x%02x%s prefix=", i + 1, ret, report[0],
		       report[0] == PSSENSE_INPUT_REPORT_ID ? " (full mode)" : "");
		print_prefix(report, (size_t)ret);
		printf("\n");
		if (ret == PSSENSE_INPUT_REPORT_LENGTH && report[0] == PSSENSE_INPUT_REPORT_ID) {
			received++;
		}
	}

	os_hid_destroy(hid);
	printf("  result: %d/%d expected full input reports received\n", received, read_count);
	return received > 0 ? 0 : 1;
}

int
main(int argc, char **argv)
{
	int read_count = 5;
	if (argc == 2) {
		read_count = atoi(argv[1]);
		if (read_count < 1 || read_count > 1000) {
			fprintf(stderr, "Usage: %s [input-report-count: 1..1000]\n", argv[0]);
			return 2;
		}
	} else if (argc > 2) {
		fprintf(stderr, "Usage: %s [input-report-count: 1..1000]\n", argv[0]);
		return 2;
	}

	IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	if (manager == NULL) {
		fprintf(stderr, "Failed to create IOHIDManager\n");
		return 1;
	}

	IOHIDManagerSetDeviceMatching(manager, NULL);
	IOReturn manager_ret = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
	if (manager_ret != kIOReturnSuccess) {
		fprintf(stderr, "Failed to open IOHIDManager: 0x%08x\n", manager_ret);
		CFRelease(manager);
		return 1;
	}

	CFSetRef device_set = IOHIDManagerCopyDevices(manager);
	if (device_set == NULL) {
		printf("No HID devices found. Pair a PS VR2 Sense controller in macOS Bluetooth settings first.\n");
		IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
		CFRelease(manager);
		return 1;
	}

	CFIndex count = CFSetGetCount(device_set);
	const void **devices = calloc((size_t)count, sizeof(void *));
	if (devices == NULL && count != 0) {
		CFRelease(device_set);
		IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
		CFRelease(manager);
		return 1;
	}
	CFSetGetValues(device_set, devices);

	int found = 0;
	int failed = 0;
	for (CFIndex i = 0; i < count; i++) {
		IOHIDDeviceRef device = (IOHIDDeviceRef)devices[i];
		int32_t vendor_id = get_int_property(device, CFSTR(kIOHIDVendorIDKey));
		int32_t product_id = get_int_property(device, CFSTR(kIOHIDProductIDKey));
		if (vendor_id != PSSENSE_VID || (product_id != PSSENSE_PID_LEFT && product_id != PSSENSE_PID_RIGHT)) {
			continue;
		}

		found++;
		if (probe_controller(device, (uint16_t)product_id, read_count) != 0) {
			failed++;
		}
	}

	free(devices);
	CFRelease(device_set);
	IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
	CFRelease(manager);

	if (found == 0) {
		printf("No paired PS VR2 Sense controllers found (Sony 054c:0e45 / 054c:0e46).\n");
		return 1;
	}

	printf("Probed %d Sense controller%s: %d succeeded, %d failed.\n", found, found == 1 ? "" : "s", found - failed,
	       failed);
	return failed == 0 ? 0 : 1;
}

#else
int
main(void)
{
	return 1;
}
#endif

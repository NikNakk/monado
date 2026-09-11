// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Small macOS-only PS Sense HID transport probe.
 */

#include "os_hid.h"
#include "os_time.h"

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
#include <time.h>

#define PSSENSE_VID 0x054c
#define PSSENSE_PID_LEFT 0x0e45
#define PSSENSE_PID_RIGHT 0x0e46
#define PSSENSE_CALIBRATION_REPORT_ID 0x05
#define PSSENSE_CALIBRATION_REPORT_LENGTH 64
#define PSSENSE_INPUT_REPORT_LENGTH 78
#define PSSENSE_INPUT_REPORT_ID 0x31
#define PSSENSE_OUTPUT_REPORT_LENGTH 78
#define PSSENSE_OUTPUT_REPORT_ID 0x31
#define PSSENSE_OUTPUT_REPORT_TAG 0x10
#define PSSENSE_OUTPUT_COUNTER_OFFSET 41
#define PSSENSE_LED_MASK_OFFSET 33
#define PSSENSE_OUTPUT_PERIOD_NS 10000000ULL

struct led_output_state
{
	uint8_t sequence;
	uint8_t counter;
};

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

static CFMutableDictionaryRef
create_sense_matching_dictionary(uint16_t product_id)
{
	CFMutableDictionaryRef matching = CFDictionaryCreateMutable(
	    kCFAllocatorDefault, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (matching == NULL) {
		return NULL;
	}

	int32_t vendor_value = PSSENSE_VID;
	int32_t product_value = product_id;
	CFNumberRef vendor = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &vendor_value);
	CFNumberRef product = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &product_value);
	if (vendor == NULL || product == NULL) {
		if (vendor != NULL) {
			CFRelease(vendor);
		}
		if (product != NULL) {
			CFRelease(product);
		}
		CFRelease(matching);
		return NULL;
	}

	CFDictionarySetValue(matching, CFSTR(kIOHIDVendorIDKey), vendor);
	CFDictionarySetValue(matching, CFSTR(kIOHIDProductIDKey), product);
	CFRelease(vendor);
	CFRelease(product);
	return matching;
}

static bool
set_sense_device_matching(IOHIDManagerRef manager)
{
	CFMutableDictionaryRef left = create_sense_matching_dictionary(PSSENSE_PID_LEFT);
	CFMutableDictionaryRef right = create_sense_matching_dictionary(PSSENSE_PID_RIGHT);
	if (left == NULL || right == NULL) {
		if (left != NULL) {
			CFRelease(left);
		}
		if (right != NULL) {
			CFRelease(right);
		}
		return false;
	}

	const void *values[] = {left, right};
	CFArrayRef matching = CFArrayCreate(kCFAllocatorDefault, values, 2, &kCFTypeArrayCallBacks);
	CFRelease(left);
	CFRelease(right);
	if (matching == NULL) {
		return false;
	}

	IOHIDManagerSetDeviceMatchingMultiple(manager, matching);
	CFRelease(matching);
	return true;
}

static int
write_tracking_led_report(struct os_hid_device *hid, struct led_output_state *state, uint32_t mask)
{
	uint8_t report[PSSENSE_OUTPUT_REPORT_LENGTH] = {0};
	report[0] = PSSENSE_OUTPUT_REPORT_ID;
	report[1] = (uint8_t)((state->sequence++ & 0x0f) << 4);
	report[2] = PSSENSE_OUTPUT_REPORT_TAG;
	report[PSSENSE_OUTPUT_COUNTER_OFFSET] = state->counter++;
	report[PSSENSE_LED_MASK_OFFSET + 0] = (uint8_t)(mask >> 0);
	report[PSSENSE_LED_MASK_OFFSET + 1] = (uint8_t)(mask >> 8);
	report[PSSENSE_LED_MASK_OFFSET + 2] = (uint8_t)(mask >> 16);
	report[PSSENSE_LED_MASK_OFFSET + 3] = (uint8_t)(mask >> 24);

	/* The macOS backend inserts PRESCAN timing and fixes the Bluetooth CRC. */
	int written = os_hid_write(hid, report, sizeof(report));
	if (written != (int)sizeof(report)) {
		fprintf(stderr, "  tracking LED output write failed: %d\n", written);
		return 1;
	}
	return 0;
}

static int
hold_tracking_led_mask(struct os_hid_device *hid,
                       struct led_output_state *state,
                       uint32_t mask,
                       time_duration_ns duration_ns)
{
	timepoint_ns end_ns = os_monotonic_get_ns() + duration_ns;
	const struct timespec interval = {.tv_sec = 0, .tv_nsec = (long)PSSENSE_OUTPUT_PERIOD_NS};
	while (os_monotonic_get_ns() < end_ns) {
		if (write_tracking_led_report(hid, state, mask) != 0) {
			return 1;
		}
		(void)nanosleep(&interval, NULL);
	}
	return 0;
}

static int
hold_tracking_leds_on(struct os_hid_device *hid, int seconds)
{
	struct led_output_state state = {0};

	printf("  holding tracking LEDs on for %d second%s...\n", seconds, seconds == 1 ? "" : "s");
	if (hold_tracking_led_mask(hid, &state, UINT32_MAX, (time_duration_ns)seconds * 1000000000LL) != 0) {
		return 1;
	}

	printf("  tracking LED hold complete; power the controller off when capture is finished\n");
	return 0;
}

static int
scan_tracking_led_masks(struct os_hid_device *hid, const char *manifest_path, int segment_ms)
{
	FILE *manifest = fopen(manifest_path, "w");
	if (manifest == NULL) {
		perror("Could not open LED mask manifest");
		return 1;
	}

	struct led_output_state state = {0};
	fprintf(manifest, "segment_index,label,mask_hex,start_monotonic_ns,end_monotonic_ns\n");
	printf("  scanning Sense LED masks: %d ms per segment, manifest=%s\n", segment_ms, manifest_path);

	const int segment_count = 21;
	for (int segment = 0; segment < segment_count; segment++) {
		uint32_t mask = 0;
		char label[32] = {0};
		if (segment == 0 || segment == segment_count - 1) {
			strcpy(label, "all_off");
		} else if (segment == 1 || segment == segment_count - 2) {
			strcpy(label, "all_on");
			mask = UINT32_MAX;
		} else {
			int bit = segment - 2;
			snprintf(label, sizeof(label), "bit_%02d", bit);
			mask = UINT32_C(1) << bit;
		}

		timepoint_ns start_ns = os_monotonic_get_ns();
		printf("  segment %02d/%02d %-8s mask=%08" PRIx32 "\n", segment + 1, segment_count, label, mask);
		fflush(stdout);
		if (hold_tracking_led_mask(hid, &state, mask, (time_duration_ns)segment_ms * 1000000LL) != 0) {
			fclose(manifest);
			return 1;
		}
		timepoint_ns end_ns = os_monotonic_get_ns();
		fprintf(manifest, "%d,%s,%08" PRIx32 ",%" PRIi64 ",%" PRIi64 "\n", segment, label, mask,
		        start_ns, end_ns);
		fflush(manifest);
	}

	fclose(manifest);
	printf("  LED mask scan complete\n");
	return 0;
}

static int
probe_controller(IOHIDDeviceRef device,
                 uint16_t product_id,
                 int read_count,
                 int force_ir_seconds,
                 const char *mask_scan_manifest,
                 int mask_segment_ms)
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

	if (force_ir_seconds > 0 && received > 0 && hold_tracking_leds_on(hid, force_ir_seconds) != 0) {
		os_hid_destroy(hid);
		return 1;
	}
	if (mask_scan_manifest != NULL && received > 0 &&
	    scan_tracking_led_masks(hid, mask_scan_manifest, mask_segment_ms) != 0) {
		os_hid_destroy(hid);
		return 1;
	}

	os_hid_destroy(hid);
	printf("  result: %d/%d expected full input reports received\n", received, read_count);
	return received > 0 ? 0 : 1;
}

int
main(int argc, char **argv)
{
	int read_count = 5;
	int force_ir_seconds = 0;
	int mask_segment_ms = 500;
	const char *mask_scan_manifest = NULL;
	uint16_t requested_product_id = 0;
	bool have_read_count = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--force-ir-seconds") == 0) {
			if (++i >= argc) {
				fprintf(stderr, "--force-ir-seconds requires a value\n");
				return 2;
			}
			force_ir_seconds = atoi(argv[i]);
			if (force_ir_seconds < 1 || force_ir_seconds > 600) {
				fprintf(stderr, "--force-ir-seconds must be between 1 and 600\n");
				return 2;
			}
		} else if (strcmp(argv[i], "--force-ir-mask-scan") == 0) {
			if (++i >= argc) {
				fprintf(stderr, "--force-ir-mask-scan requires an output CSV path\n");
				return 2;
			}
			mask_scan_manifest = argv[i];
		} else if (strcmp(argv[i], "--mask-segment-ms") == 0) {
			if (++i >= argc) {
				fprintf(stderr, "--mask-segment-ms requires a value\n");
				return 2;
			}
			mask_segment_ms = atoi(argv[i]);
			if (mask_segment_ms < 200 || mask_segment_ms > 5000) {
				fprintf(stderr, "--mask-segment-ms must be between 200 and 5000\n");
				return 2;
			}
		} else if (strcmp(argv[i], "--hand") == 0) {
			if (++i >= argc) {
				fprintf(stderr, "--hand requires left or right\n");
				return 2;
			}
			if (strcmp(argv[i], "left") == 0) {
				requested_product_id = PSSENSE_PID_LEFT;
			} else if (strcmp(argv[i], "right") == 0) {
				requested_product_id = PSSENSE_PID_RIGHT;
			} else {
				fprintf(stderr, "--hand requires left or right\n");
				return 2;
			}
		} else if (!have_read_count) {
			read_count = atoi(argv[i]);
			have_read_count = true;
			if (read_count < 1 || read_count > 1000) {
				fprintf(stderr, "input-report-count must be between 1 and 1000\n");
				return 2;
			}
		} else {
			fprintf(stderr,
			        "Usage: %s [--hand left|right] [--force-ir-seconds 1..600 | "
			        "--force-ir-mask-scan output.csv [--mask-segment-ms 200..5000]] "
			        "[input-report-count: 1..1000]\n",
			        argv[0]);
			return 2;
		}
	}

	if (force_ir_seconds > 0 && mask_scan_manifest != NULL) {
		fprintf(stderr, "--force-ir-seconds and --force-ir-mask-scan are mutually exclusive\n");
		return 2;
	}
	if (mask_scan_manifest != NULL && requested_product_id == 0) {
		fprintf(stderr, "--force-ir-mask-scan requires --hand left or --hand right\n");
		return 2;
	}
	if (force_ir_seconds > 0 || mask_scan_manifest != NULL) {
		if (setenv("PSSENSE_FORCE_IR", "1", 1) != 0) {
			fprintf(stderr, "Failed to enable PSSENSE_FORCE_IR\n");
			return 1;
		}
	}

	IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	if (manager == NULL) {
		fprintf(stderr, "Failed to create IOHIDManager\n");
		return 1;
	}

	if (!set_sense_device_matching(manager)) {
		fprintf(stderr, "Failed to create Sense HID matching criteria\n");
		CFRelease(manager);
		return 1;
	}

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
		if (requested_product_id != 0 && product_id != requested_product_id) {
			continue;
		}

		found++;
		if (probe_controller(device, (uint16_t)product_id, read_count, force_ir_seconds, mask_scan_manifest,
		                     mask_segment_ms) != 0) {
			failed++;
		}
	}

	free(devices);
	CFRelease(device_set);
	IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
	CFRelease(manager);

	if (found == 0) {
		if (requested_product_id != 0) {
			printf("No active %s PS VR2 Sense controller found. Pair and wake it first.\n",
			       hand_name(requested_product_id));
		} else {
			printf("No paired PS VR2 Sense controllers found (Sony 054c:0e45 / 054c:0e46).\n");
		}
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

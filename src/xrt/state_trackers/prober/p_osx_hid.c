// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS IOKit HID probing for PS VR2 Sense controllers.
 * @ingroup st_prober
 */

#include "p_prober.h"

#ifdef XRT_OS_OSX

#include "os/os_hid.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDLib.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSSENSE_VID 0x054c
#define PSSENSE_PID_LEFT 0x0e45
#define PSSENSE_PID_RIGHT 0x0e46

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

static bool
get_string_property(IOHIDDeviceRef device, CFStringRef key, char *out, size_t out_size)
{
	if (out == NULL || out_size == 0) {
		return false;
	}

	out[0] = '\0';
	CFTypeRef value = IOHIDDeviceGetProperty(device, key);
	if (value == NULL || CFGetTypeID(value) != CFStringGetTypeID()) {
		return false;
	}

	return CFStringGetCString((CFStringRef)value, out, (CFIndex)out_size, kCFStringEncodingUTF8);
}

static CFMutableDictionaryRef
create_matching_dictionary(uint16_t product_id)
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
set_sense_matching(IOHIDManagerRef manager)
{
	CFMutableDictionaryRef left = create_matching_dictionary(PSSENSE_PID_LEFT);
	CFMutableDictionaryRef right = create_matching_dictionary(PSSENSE_PID_RIGHT);
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

static bool
is_bluetooth(IOHIDDeviceRef device)
{
	char transport[64] = {0};
	if (!get_string_property(device, CFSTR(kIOHIDTransportKey), transport, sizeof(transport))) {
		return false;
	}

	return strcmp(transport, "Bluetooth") == 0;
}

static bool
parse_bluetooth_address(const char *serial, uint64_t *out_id)
{
	if (serial == NULL || out_id == NULL) {
		return false;
	}

	unsigned int bytes[6] = {0};
	if (sscanf(serial, "%2x:%2x:%2x:%2x:%2x:%2x", &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4],
	           &bytes[5]) != 6) {
		return false;
	}

	uint64_t id = 0;
	for (size_t i = 0; i < 6; i++) {
		if (bytes[i] > 0xff) {
			return false;
		}
		id = (id << 8) | (uint64_t)bytes[i];
	}

	*out_id = id;
	return true;
}

static uint64_t
get_registry_id(IOHIDDeviceRef device)
{
	io_service_t service = IOHIDDeviceGetService(device);
	if (service == IO_OBJECT_NULL) {
		return 0;
	}

	uint64_t entry_id = 0;
	return IORegistryEntryGetRegistryEntryID(service, &entry_id) == KERN_SUCCESS ? entry_id : 0;
}

static int
p_osx_hid_open_interface(struct xrt_prober *xp,
                         struct xrt_prober_device *xpdev,
                         int hid_iface,
                         struct os_hid_device **out_hid_dev)
{
	(void)xp;
	struct prober_device *pdev = (struct prober_device *)xpdev;

	// Sense exposes a single HID interface to the Monado driver.
	if (hid_iface != 0 || pdev->base.bus != XRT_BUS_TYPE_BLUETOOTH || pdev->osx_hid_device == NULL) {
		return -1;
	}

	return os_hid_open_iokit(pdev->osx_hid_device, out_hid_dev);
}

static bool
p_osx_hid_can_open(struct xrt_prober *xp, struct xrt_prober_device *xpdev)
{
	struct prober *p = (struct prober *)xp;
	struct prober_device *pdev = (struct prober_device *)xpdev;

	if (pdev->base.bus == XRT_BUS_TYPE_BLUETOOTH && pdev->osx_hid_device != NULL) {
		return true;
	}

#ifdef XRT_HAVE_LIBUSB
	if (pdev->base.bus == XRT_BUS_TYPE_USB && pdev->usb.dev != NULL) {
		return p_libusb_can_open(p, pdev);
	}
#endif

	return false;
}

void
p_osx_hid_teardown(struct prober *p)
{
	if (p->osx_hid_device_set != NULL) {
		CFRelease((CFSetRef)p->osx_hid_device_set);
		p->osx_hid_device_set = NULL;
	}

	if (p->osx_hid_manager != NULL) {
		IOHIDManagerClose((IOHIDManagerRef)p->osx_hid_manager, kIOHIDOptionsTypeNone);
		CFRelease((IOHIDManagerRef)p->osx_hid_manager);
		p->osx_hid_manager = NULL;
	}
}

int
p_osx_hid_probe(struct prober *p)
{
	p_osx_hid_teardown(p);

	IOHIDManagerRef manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
	if (manager == NULL) {
		P_ERROR(p, "Failed to create IOHIDManager");
		return -1;
	}

	if (!set_sense_matching(manager)) {
		P_ERROR(p, "Failed to create PS Sense IOKit matching criteria");
		CFRelease(manager);
		return -1;
	}

	IOReturn ret = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
	if (ret != kIOReturnSuccess) {
		P_ERROR(p, "Failed to open IOHIDManager: 0x%08x", ret);
		CFRelease(manager);
		return -1;
	}

	CFSetRef device_set = IOHIDManagerCopyDevices(manager);
	p->osx_hid_manager = manager;
	p->osx_hid_device_set = device_set;
	if (device_set == NULL) {
		return 0;
	}

	CFIndex count = CFSetGetCount(device_set);
	const void **devices = calloc((size_t)count, sizeof(void *));
	if (devices == NULL && count > 0) {
		P_ERROR(p, "Failed to allocate macOS HID enumeration array");
		return -1;
	}
	CFSetGetValues(device_set, devices);

	for (CFIndex i = 0; i < count; i++) {
		IOHIDDeviceRef device = (IOHIDDeviceRef)devices[i];
		if (!is_bluetooth(device)) {
			continue;
		}

		int32_t vendor = get_int_property(device, CFSTR(kIOHIDVendorIDKey));
		int32_t product = get_int_property(device, CFSTR(kIOHIDProductIDKey));
		if (vendor != PSSENSE_VID || (product != PSSENSE_PID_LEFT && product != PSSENSE_PID_RIGHT)) {
			continue;
		}

		char product_name[P_PROBER_BLUETOOTH_PRODUCT_COUNT] = {0};
		if (!get_string_property(device, CFSTR(kIOHIDProductKey), product_name, sizeof(product_name))) {
			snprintf(product_name, sizeof(product_name), "PS VR2 Sense Controller");
		}

		char serial[64] = {0};
		(void)get_string_property(device, CFSTR(kIOHIDSerialNumberKey), serial, sizeof(serial));
		uint64_t bluetooth_id = 0;
		if (!parse_bluetooth_address(serial, &bluetooth_id)) {
			bluetooth_id = get_registry_id(device);
		}
		if (bluetooth_id == 0) {
			P_WARN(p, "Skipping PS Sense controller without stable Bluetooth/registry identifier");
			continue;
		}

		struct prober_device *pdev = NULL;
		if (p_dev_get_bluetooth_dev(p, bluetooth_id, (uint16_t)vendor, (uint16_t)product, product_name, &pdev) != 0 ||
		    pdev == NULL) {
			P_WARN(p, "Failed to add PS Sense controller %04x:%04x", (uint16_t)vendor, (uint16_t)product);
			continue;
		}

		pdev->osx_hid_device = device;
		P_INFO(p, "Found macOS PS Sense HID %04x:%04x '%s' (%s)", (uint16_t)vendor, (uint16_t)product,
		       product_name, serial);
	}

	free(devices);

	// Replace the generic macOS stubs now that a real native HID backend exists.
	p->base.open_hid_interface = p_osx_hid_open_interface;
	p->base.can_open = p_osx_hid_can_open;
	return 0;
}

#endif // XRT_OS_OSX

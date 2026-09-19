// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Create global IOSurfaces in a separate process for the external-import probe.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>
#include <mach/kern_return.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PROBE_WIDTH 64
#define PROBE_HEIGHT 64
#define PROBE_IMAGE_COUNT 3
#define PROBE_BGRA_FOURCC UINT32_C(0x42475241)

static void
dict_set_u32(CFMutableDictionaryRef dict, CFStringRef key, uint32_t value)
{
	CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
	CFDictionarySetValue(dict, key, number);
	CFRelease(number);
}

static IOSurfaceRef
create_surface(uint32_t image_index)
{
	const uint32_t bytes_per_element = 4;
	const uint32_t bytes_per_row = PROBE_WIDTH * bytes_per_element;
	const uint32_t alloc_size = bytes_per_row * PROBE_HEIGHT;

	CFMutableDictionaryRef properties =
	    CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (properties == NULL) {
		return NULL;
	}

	dict_set_u32(properties, kIOSurfaceWidth, PROBE_WIDTH);
	dict_set_u32(properties, kIOSurfaceHeight, PROBE_HEIGHT);
	dict_set_u32(properties, kIOSurfaceBytesPerElement, bytes_per_element);
	dict_set_u32(properties, kIOSurfaceBytesPerRow, bytes_per_row);
	dict_set_u32(properties, kIOSurfaceAllocSize, alloc_size);
	dict_set_u32(properties, kIOSurfacePixelFormat, PROBE_BGRA_FOURCC);
	CFDictionarySetValue(properties, kIOSurfaceIsGlobal, kCFBooleanTrue);

	IOSurfaceRef surface = IOSurfaceCreate(properties);
	CFRelease(properties);
	if (surface == NULL) {
		return NULL;
	}

	if (IOSurfaceLock(surface, 0, NULL) != KERN_SUCCESS) {
		CFRelease(surface);
		return NULL;
	}

	uint8_t *base = IOSurfaceGetBaseAddress(surface);
	const size_t stride = IOSurfaceGetBytesPerRow(surface);
	if (base == NULL) {
		IOSurfaceUnlock(surface, 0, NULL);
		CFRelease(surface);
		return NULL;
	}

	const uint8_t colors[PROBE_IMAGE_COUNT][4] = {
	    {0x20, 0x20, 0xf0, 0xff},
	    {0x20, 0xf0, 0x20, 0xff},
	    {0xf0, 0x20, 0x20, 0xff},
	};
	for (uint32_t y = 0; y < PROBE_HEIGHT; y++) {
		for (uint32_t x = 0; x < PROBE_WIDTH; x++) {
			memcpy(base + y * stride + x * 4, colors[image_index], 4);
		}
	}
	IOSurfaceUnlock(surface, 0, NULL);
	return surface;
}

int
main(void)
{
	IOSurfaceRef surfaces[PROBE_IMAGE_COUNT] = {0};
	for (uint32_t i = 0; i < PROBE_IMAGE_COUNT; i++) {
		surfaces[i] = create_surface(i);
		if (surfaces[i] == NULL) {
			fprintf(stderr, "Failed to create IOSurface %u\n", i);
			for (uint32_t j = 0; j < i; j++) {
				CFRelease(surfaces[j]);
			}
			return 1;
		}
	}

	printf("%u %u %u\n",
	       (unsigned)IOSurfaceGetID(surfaces[0]),
	       (unsigned)IOSurfaceGetID(surfaces[1]),
	       (unsigned)IOSurfaceGetID(surfaces[2]));
	fflush(stdout);
	fprintf(stderr, "Holding three 64x64 BGRA IOSurfaces alive. Press Enter after the import probe completes.\n");
	(void)getchar();

	for (uint32_t i = 0; i < PROBE_IMAGE_COUNT; i++) {
		CFRelease(surfaces[i]);
	}
	return 0;
}

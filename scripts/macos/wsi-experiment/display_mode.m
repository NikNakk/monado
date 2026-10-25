// SPDX-License-Identifier: BSL-1.0
// Test helper: preserve and restore the selected PS VR2 display mode.
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int
main(int argc, char **argv)
{
	@autoreleasepool {
		for (NSScreen *screen in NSScreen.screens) {
			if (![screen.localizedName isEqualToString:@"PS VR2"])
				continue;
			CGDirectDisplayID display = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
			CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
			if (!current)
				return 1;
			if (argc == 1) {
				printf("%u %.6f %zu %zu\n", CGDisplayModeGetIODisplayModeID(current),
				       CGDisplayModeGetRefreshRate(current), CGDisplayModeGetPixelWidth(current),
				       CGDisplayModeGetPixelHeight(current));
				CGDisplayModeRelease(current);
				return 0;
			}
			if (argc != 3 || (strcmp(argv[1], "--restore") != 0 && strcmp(argv[1], "--refresh") != 0)) {
				CGDisplayModeRelease(current);
				return 2;
			}
			char *end = NULL;
			unsigned long id = strtoul(argv[2], &end, 10);
			if (*end || id > UINT32_MAX) {
				CGDisplayModeRelease(current);
				return 2;
			}
			CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, NULL);
			int result = 1;
			if (modes) {
				for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
					CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
					bool match =
					    strcmp(argv[1], "--restore") == 0
					        ? (uint32_t)CGDisplayModeGetIODisplayModeID(mode) == (uint32_t)id
					        : fabs(CGDisplayModeGetRefreshRate(mode) - (double)id) < 1 &&
					              CGDisplayModeGetPixelWidth(mode) ==
					                  CGDisplayModeGetPixelWidth(current) &&
					              CGDisplayModeGetPixelHeight(mode) ==
					                  CGDisplayModeGetPixelHeight(current);
					if (!match)
						continue;
					result =
					    CGDisplaySetDisplayMode(display, mode, NULL) == kCGErrorSuccess ? 0 : 1;
					break;
				}
				CFRelease(modes);
			}
			CGDisplayModeRelease(current);
			return result;
		}
		fprintf(stderr, "PS VR2 display is not connected\n");
		return 1;
	}
}

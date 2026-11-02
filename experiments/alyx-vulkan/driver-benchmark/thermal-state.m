// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#import <Foundation/Foundation.h>
int
main(void)
{
	@autoreleasepool {
		printf("%ld\n", (long)NSProcessInfo.processInfo.thermalState);
	}
}

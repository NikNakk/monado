// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Diagnostic macOS NSProcessInfo activity assertion.
 * @ingroup ipc_server
 */

#import <Foundation/Foundation.h>

#include "server/ipc_server_macos_activity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSLock *g_process_activity_lock = nil;
static id<NSObject> g_process_activity_token = nil;

static NSLock *
get_process_activity_lock(void)
{
	@synchronized([NSProcessInfo class]) {
		if (g_process_activity_lock == nil) {
			g_process_activity_lock = [[NSLock alloc] init];
		}
	}
	return g_process_activity_lock;
}

static bool
process_activity_options_from_value(const char *value, NSActivityOptions *out_options, const char **out_mode)
{
	if (value == NULL || value[0] == '\0' || strcmp(value, "0") == 0 || strcmp(value, "off") == 0) {
		return false;
	}

	if (strcmp(value, "user-interactive") == 0) {
		*out_options = NSActivityUserInteractive;
		*out_mode = "user-interactive";
		return true;
	}

	if (strcmp(value, "latency-critical") == 0) {
		*out_options = NSActivityUserInteractive | NSActivityLatencyCritical;
		*out_mode = "latency-critical";
		return true;
	}

	return false;
}

void
ipc_server_macos_process_activity_startup(void)
{
	@autoreleasepool {
		const char *value = getenv("XRT_MACOS_PROCESS_ACTIVITY");

		/*
		 * Deliberately bypass Monado's logging layer: this line is the
		 * diagnostic proof that the guaranteed service startup path reached
		 * this code and shows exactly what getenv() sees.
		 */
		fprintf(stderr, "MACOS_PROCESS_ACTIVITY startup raw=%s\n", value != NULL ? value : "<unset>");
		fflush(stderr);

		NSActivityOptions options = 0;
		const char *mode = NULL;
		if (!process_activity_options_from_value(value, &options, &mode)) {
			if (value != NULL && value[0] != '\0' && strcmp(value, "0") != 0 && strcmp(value, "off") != 0) {
				fprintf(stderr,
				        "MACOS_PROCESS_ACTIVITY invalid value=%s expected=user-interactive|latency-critical\n",
				        value);
				fflush(stderr);
			}
			return;
		}

		NSLock *lock = get_process_activity_lock();
		[lock lock];
		if (g_process_activity_token != nil) {
			[lock unlock];
			return;
		}

		id<NSObject> token =
		    [[NSProcessInfo processInfo] beginActivityWithOptions:options reason:@"Monado XR compositor diagnostic"];
		if (token == nil) {
			fprintf(stderr, "MACOS_PROCESS_ACTIVITY begin failed mode=%s\n", mode);
			fflush(stderr);
			[lock unlock];
			return;
		}

		g_process_activity_token = [token retain];
		fprintf(stderr,
		        "MACOS_PROCESS_ACTIVITY began mode=%s options=0x%llx process_lifetime=1\n",
		        mode,
		        (unsigned long long)options);
		fflush(stderr);
		[lock unlock];
	}
}

void
ipc_server_macos_process_activity_shutdown(void)
{
	@autoreleasepool {
		NSLock *lock = get_process_activity_lock();
		[lock lock];
		if (g_process_activity_token != nil) {
			[[NSProcessInfo processInfo] endActivity:g_process_activity_token];
			[g_process_activity_token release];
			g_process_activity_token = nil;
			fprintf(stderr, "MACOS_PROCESS_ACTIVITY ended process_lifetime=1\n");
			fflush(stderr);
		}
		[lock unlock];
	}
}

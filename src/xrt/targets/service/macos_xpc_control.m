// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Development launchd registration for on-demand monado-service.
 * @ingroup ipc
 */

#import <Foundation/Foundation.h>

#include "shared/ipc_metal_xpc.h"

#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MONADO_XPC_LAUNCHD_LABEL "org.freedesktop.monado.service"

extern char **environ;

static bool
get_executable_path(char out_path[PATH_MAX])
{
	uint32_t size = PATH_MAX;
	char raw_path[PATH_MAX] = {0};
	if (_NSGetExecutablePath(raw_path, &size) != 0) {
		return false;
	}
	return realpath(raw_path, out_path) != NULL;
}

static bool
get_service_executable(char out_path[PATH_MAX])
{
	char control_path[PATH_MAX] = {0};
	if (!get_executable_path(control_path)) {
		return false;
	}

	char *slash = strrchr(control_path, '/');
	if (slash == NULL) {
		return false;
	}
	*slash = '\0';

	if (snprintf(out_path, PATH_MAX, "%s/monado-service", control_path) >= PATH_MAX) {
		return false;
	}

	return access(out_path, X_OK) == 0;
}

static void
get_domain(char out_domain[64])
{
	snprintf(out_domain, 64, "gui/%u", (unsigned)getuid());
}

static void
get_direct_target(char out_target[256])
{
	snprintf(out_target, 256, "gui/%u/%s", (unsigned)getuid(), MONADO_XPC_LAUNCHD_LABEL);
}

static void
get_legacy_broker_target(char out_target[256])
{
	snprintf(out_target, 256, "gui/%u/%s", (unsigned)getuid(), IPC_METAL_XPC_SERVICE_NAME);
}

static void
get_plist_path(char out_path[PATH_MAX])
{
	snprintf(out_path, PATH_MAX, "/tmp/%s.%u.plist", MONADO_XPC_LAUNCHD_LABEL, (unsigned)getuid());
}

static void
get_legacy_plist_path(char out_path[PATH_MAX])
{
	snprintf(out_path, PATH_MAX, "/tmp/%s.%u.plist", IPC_METAL_XPC_SERVICE_NAME, (unsigned)getuid());
}

static void
get_log_paths(char out_stdout[PATH_MAX], char out_stderr[PATH_MAX])
{
	snprintf(out_stdout, PATH_MAX, "/tmp/monado-service-launchd.%u.out.log", (unsigned)getuid());
	snprintf(out_stderr, PATH_MAX, "/tmp/monado-service-launchd.%u.err.log", (unsigned)getuid());
}

static int
run_launchctl(const char *verb, const char *arg1, const char *arg2)
{
	char *argv[5] = {(char *)"/bin/launchctl", (char *)verb, (char *)arg1, NULL, NULL};
	if (arg2 != NULL) {
		argv[3] = (char *)arg2;
	}

	pid_t pid = 0;
	int ret = posix_spawn(&pid, "/bin/launchctl", NULL, NULL, argv, environ);
	if (ret != 0) {
		fprintf(stderr, "launchctl %s spawn failed: %s\n", verb, strerror(ret));
		return ret;
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			return errno;
		}
	}

	if (!WIFEXITED(status)) {
		return 128;
	}
	return WEXITSTATUS(status);
}

static bool
should_forward_environment_key(NSString *key)
{
	if (key == nil) {
		return false;
	}

	static NSArray<NSString *> *prefixes = nil;
	static NSSet<NSString *> *exact_keys = nil;
	static dispatch_once_t once_token;
	dispatch_once(&once_token, ^{
		prefixes = [[NSArray alloc] initWithObjects:@"XRT_",
		                                              @"PSVR2_",
		                                              @"IPC_",
		                                              @"VK_",
		                                              @"MVK_",
		                                              @"MOLTENVK_",
		                                              @"METAL_",
		                                              @"MTL_",
		                                              nil];
		exact_keys = [[NSSet alloc] initWithObjects:@"PATH", @"DYLD_LIBRARY_PATH", @"DYLD_FRAMEWORK_PATH", nil];
	});

	if ([exact_keys containsObject:key]) {
		return true;
	}

	for (NSString *prefix in prefixes) {
		if ([key hasPrefix:prefix]) {
			return true;
		}
	}

	return false;
}

static void
set_default_environment_value(NSMutableDictionary *environment, NSString *key, NSString *value)
{
	if ([environment objectForKey:key] == nil) {
		[environment setObject:value forKey:key];
	}
}

static NSDictionary *
make_launch_environment(void)
{
	NSDictionary *current = [[NSProcessInfo processInfo] environment];
	NSMutableDictionary *filtered = [NSMutableDictionary dictionary];

	for (NSString *key in current) {
		if (!should_forward_environment_key(key)) {
			continue;
		}

		NSString *value = [current objectForKey:key];
		if (value != nil) {
			[filtered setObject:value forKey:key];
		}
	}

	/* A launchd service must never treat its inherited stdin as a quit trigger. */
	[filtered setObject:@"1" forKey:@"XRT_NO_STDIN"];

	/*
	 * launchd keeps the registration around after the process exits, so the
	 * service itself should be disposable. These are launchd-only defaults:
	 * explicit values exported by the developer/user are preserved.
	 */
	set_default_environment_value(filtered, @"IPC_EXIT_WHEN_IDLE", @"1");
	set_default_environment_value(filtered, @"IPC_EXIT_WHEN_IDLE_DELAY_MS", @"5000");
	set_default_environment_value(filtered, @"XRT_MACOS_EXIT_ON_DISPLAY_LOSS", @"1");
	set_default_environment_value(filtered, @"XRT_MACOS_DISPLAY_LOSS_DELAY_MS", @"3000");

	return filtered;
}

static bool
write_launch_agent_plist(const char *path, const char *service_executable)
{
	@autoreleasepool {
		NSString *exe = [NSString stringWithUTF8String:service_executable];
		NSString *plist_path = [NSString stringWithUTF8String:path];
		NSString *label = [NSString stringWithUTF8String:MONADO_XPC_LAUNCHD_LABEL];
		NSString *mach_service = [NSString stringWithUTF8String:IPC_METAL_XPC_SERVICE_NAME];

		char stdout_path[PATH_MAX] = {0};
		char stderr_path[PATH_MAX] = {0};
		get_log_paths(stdout_path, stderr_path);
		NSString *stdout_string = [NSString stringWithUTF8String:stdout_path];
		NSString *stderr_string = [NSString stringWithUTF8String:stderr_path];
		NSDictionary *launch_environment = make_launch_environment();

		if (exe == nil || plist_path == nil || label == nil || mach_service == nil || stdout_string == nil ||
		    stderr_string == nil || launch_environment == nil) {
			return false;
		}

		NSDictionary *plist = @{
			@"Label" : label,
			@"ProgramArguments" : @[ exe ],
			@"MachServices" : @{ mach_service : @YES },
			@"RunAtLoad" : @NO,
			@"ProcessType" : @"Interactive",
			@"EnvironmentVariables" : launch_environment,
			@"StandardOutPath" : stdout_string,
			@"StandardErrorPath" : stderr_string,
		};

		NSError *error = nil;
		NSData *data = [NSPropertyListSerialization dataWithPropertyList:plist
		                                                        format:NSPropertyListXMLFormat_v1_0
		                                                       options:0
		                                                         error:&error];
		if (data == nil || ![data writeToFile:plist_path options:NSDataWritingAtomic error:&error]) {
			const char *message = error != nil ? error.localizedDescription.UTF8String : "unknown error";
			fprintf(stderr, "Could not write monado-service LaunchAgent plist: %s\n",
			        message != NULL ? message : "unknown");
			return false;
		}

		return true;
	}
}

static int
bootstrap_service(void)
{
	char service_executable[PATH_MAX] = {0};
	if (!get_service_executable(service_executable)) {
		fprintf(stderr, "Could not find executable sibling 'monado-service' next to this control tool\n");
		return 1;
	}

	char domain[64] = {0};
	char direct_target[256] = {0};
	char legacy_target[256] = {0};
	char plist_path[PATH_MAX] = {0};
	char legacy_plist_path[PATH_MAX] = {0};
	get_domain(domain);
	get_direct_target(direct_target);
	get_legacy_broker_target(legacy_target);
	get_plist_path(plist_path);
	get_legacy_plist_path(legacy_plist_path);

	/* The old broker and the service intentionally advertise the same Mach name. */
	(void)run_launchctl("bootout", legacy_target, NULL);
	(void)run_launchctl("bootout", direct_target, NULL);
	unlink(legacy_plist_path);
	unlink(plist_path);

	if (!write_launch_agent_plist(plist_path, service_executable)) {
		return 2;
	}

	int ret = run_launchctl("bootstrap", domain, plist_path);
	if (ret != 0) {
		fprintf(stderr, "launchctl bootstrap failed with status %d\n", ret);
		return 3;
	}

	char stdout_path[PATH_MAX] = {0};
	char stderr_path[PATH_MAX] = {0};
	get_log_paths(stdout_path, stderr_path);

	printf("monado-service registered for on-demand XPC activation\n");
	printf("LaunchAgent label: %s\n", MONADO_XPC_LAUNCHD_LABEL);
	printf("Mach service: %s\n", IPC_METAL_XPC_SERVICE_NAME);
	printf("Executable: %s\n", service_executable);
	printf("LaunchAgent plist: %s\n", plist_path);
	printf("Relevant XRT/PSVR2/Vulkan environment captured from this shell\n");
	printf("Lifecycle defaults: idle exit after 5000 ms; display-loss exit after 3000 ms (explicit environment overrides preserved)\n");
	printf("stdout: %s\n", stdout_path);
	printf("stderr: %s\n", stderr_path);
	return 0;
}

static int
bootout_service(void)
{
	char direct_target[256] = {0};
	char plist_path[PATH_MAX] = {0};
	get_direct_target(direct_target);
	get_plist_path(plist_path);

	int ret = run_launchctl("bootout", direct_target, NULL);
	if (ret != 0) {
		fprintf(stderr, "launchctl bootout returned %d\n", ret);
	}
	unlink(plist_path);
	return ret == 0 ? 0 : 1;
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "bootstrap") == 0) {
		return bootstrap_service();
	}
	if (argc == 2 && strcmp(argv[1], "bootout") == 0) {
		return bootout_service();
	}

	fprintf(stderr, "usage: %s {bootstrap|bootout}\n", argv[0]);
	return 64;
}

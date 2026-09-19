// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief launchd registration for on-demand monado-service on macOS.
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

static bool
copy_ns_path(NSString *path, char out_path[PATH_MAX])
{
	if (path == nil) {
		return false;
	}
	const char *filesystem_path = path.fileSystemRepresentation;
	if (filesystem_path == NULL) {
		return false;
	}
	int written = snprintf(out_path, PATH_MAX, "%s", filesystem_path);
	return written >= 0 && written < PATH_MAX;
}

static bool
get_persistent_plist_path(char out_path[PATH_MAX])
{
	@autoreleasepool {
		NSString *path = [[NSHomeDirectory() stringByAppendingPathComponent:@"Library/LaunchAgents"]
		    stringByAppendingPathComponent:@MONADO_XPC_LAUNCHD_LABEL ".plist"];
		return copy_ns_path(path, out_path);
	}
}

static bool
get_persistent_log_paths(char out_stdout[PATH_MAX], char out_stderr[PATH_MAX])
{
	@autoreleasepool {
		NSString *dir = [[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs"]
		    stringByAppendingPathComponent:@"Monado"];
		NSString *stdout_path = [dir stringByAppendingPathComponent:@"monado-service.out.log"];
		NSString *stderr_path = [dir stringByAppendingPathComponent:@"monado-service.err.log"];
		return copy_ns_path(stdout_path, out_stdout) && copy_ns_path(stderr_path, out_stderr);
	}
}

static bool
ensure_persistent_directories(void)
{
	@autoreleasepool {
		NSFileManager *fm = [NSFileManager defaultManager];
		NSArray<NSString *> *directories = @[
			[NSHomeDirectory() stringByAppendingPathComponent:@"Library/LaunchAgents"],
			[[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs"]
			    stringByAppendingPathComponent:@"Monado"],
		];
		for (NSString *directory in directories) {
			NSError *error = nil;
			if (![fm createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:&error]) {
				const char *message = error != nil ? error.localizedDescription.UTF8String : "unknown error";
				fprintf(stderr, "Could not create '%s': %s\n", directory.fileSystemRepresentation,
				        message != NULL ? message : "unknown error");
				return false;
			}
		}
		return true;
	}
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

static int
run_launchctl_bootstrap_with_retry(const char *domain, const char *plist_path)
{
	int ret = 0;
	for (int attempt = 0; attempt < 5; attempt++) {
		ret = run_launchctl("bootstrap", domain, plist_path);
		if (ret == 0) {
			return 0;
		}
		/*
		 * launchd can briefly retain the old Mach-service registration after
		 * bootout. Status 5 (EIO) is commonly returned during that teardown
		 * window, so give it a short bounded chance to settle.
		 */
		if (attempt < 4) {
			usleep((useconds_t)(200000 * (attempt + 1)));
		}
	}
	return ret;
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
make_launch_environment(bool capture_current_environment)
{
	NSMutableDictionary *filtered = [NSMutableDictionary dictionary];
	if (capture_current_environment) {
		NSDictionary *current = [[NSProcessInfo processInfo] environment];
		for (NSString *key in current) {
			if (!should_forward_environment_key(key)) {
				continue;
			}

			NSString *value = [current objectForKey:key];
			if (value != nil) {
				[filtered setObject:value forKey:key];
			}
		}
	}

	/* A launchd service must never treat its inherited stdin as a quit trigger. */
	[filtered setObject:@"1" forKey:@"XRT_NO_STDIN"];

	/*
	 * launchd keeps the registration around after the process exits, so the
	 * service itself should be disposable. These are launchd-only defaults:
	 * explicit values exported by the developer/user are preserved in the
	 * development bootstrap mode.
	 */
	set_default_environment_value(filtered, @"IPC_EXIT_WHEN_IDLE", @"1");
	set_default_environment_value(filtered, @"IPC_EXIT_WHEN_IDLE_DELAY_MS", @"5000");
	set_default_environment_value(filtered, @"XRT_MACOS_EXIT_ON_DISPLAY_LOSS", @"1");
	set_default_environment_value(filtered, @"XRT_MACOS_DISPLAY_LOSS_DELAY_MS", @"3000");
	set_default_environment_value(filtered, @"XRT_MACOS_DISPLAY_LOSS_SHUTDOWN_WATCHDOG_MS", @"5000");

	return filtered;
}

static bool
write_launch_agent_plist(const char *path, const char *service_executable, bool capture_current_environment,
                         bool persistent_logs)
{
	@autoreleasepool {
		NSString *exe = [NSString stringWithUTF8String:service_executable];
		NSString *plist_path = [NSString stringWithUTF8String:path];
		NSString *label = [NSString stringWithUTF8String:MONADO_XPC_LAUNCHD_LABEL];
		NSString *mach_service = [NSString stringWithUTF8String:IPC_METAL_XPC_SERVICE_NAME];

		char stdout_path[PATH_MAX] = {0};
		char stderr_path[PATH_MAX] = {0};
		bool have_log_paths = persistent_logs ? get_persistent_log_paths(stdout_path, stderr_path)
		                                      : (get_log_paths(stdout_path, stderr_path), true);
		NSString *stdout_string = have_log_paths ? [NSString stringWithUTF8String:stdout_path] : nil;
		NSString *stderr_string = have_log_paths ? [NSString stringWithUTF8String:stderr_path] : nil;
		NSDictionary *launch_environment = make_launch_environment(capture_current_environment);

		if (exe == nil || plist_path == nil || label == nil || mach_service == nil || stdout_string == nil ||
		    stderr_string == nil || launch_environment == nil) {
			return false;
		}

		NSDictionary *plist = @{
			@"Label" : label,
			@"ProgramArguments" : @[ exe ],
			@"MachServices" : @{ mach_service : @YES },
			@"RunAtLoad" : @NO,
			@"ProcessType" : @"Adaptive",
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

static void
remove_development_registrations(const char *legacy_target, const char *direct_target, const char *legacy_plist_path,
                                 const char *plist_path)
{
	/* The old broker and the service intentionally advertise the same Mach name. */
	(void)run_launchctl("bootout", legacy_target, NULL);
	(void)run_launchctl("bootout", direct_target, NULL);
	unlink(legacy_plist_path);
	unlink(plist_path);
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

	remove_development_registrations(legacy_target, direct_target, legacy_plist_path, plist_path);

	if (!write_launch_agent_plist(plist_path, service_executable, true, false)) {
		return 2;
	}

	int ret = run_launchctl_bootstrap_with_retry(domain, plist_path);
	if (ret != 0) {
		fprintf(stderr, "launchctl bootstrap failed with status %d\n", ret);
		return 3;
	}

	char stdout_path[PATH_MAX] = {0};
	char stderr_path[PATH_MAX] = {0};
	get_log_paths(stdout_path, stderr_path);

	printf("monado-service registered for on-demand XPC activation (development mode)\n");
	printf("LaunchAgent label: %s\n", MONADO_XPC_LAUNCHD_LABEL);
	printf("Mach service: %s\n", IPC_METAL_XPC_SERVICE_NAME);
	printf("ProcessType: Adaptive (foreground XPC provenance enabled when the client requests an importance lease)\n");
	printf("Executable: %s\n", service_executable);
	printf("LaunchAgent plist: %s\n", plist_path);
	printf("Relevant XRT/PSVR2/Vulkan environment captured from this shell\n");
	printf("Lifecycle defaults: idle exit after 5000 ms; display-loss exit after 3000 ms; forced-exit watchdog after a further 5000 ms (explicit environment overrides preserved)\n");
	printf("stdout: %s\n", stdout_path);
	printf("stderr: %s\n", stderr_path);
	return 0;
}

static int
install_service(void)
{
	char service_executable[PATH_MAX] = {0};
	if (!get_service_executable(service_executable)) {
		fprintf(stderr, "Could not find executable sibling 'monado-service' next to this control tool\n");
		return 1;
	}
	if (!ensure_persistent_directories()) {
		return 2;
	}

	char domain[64] = {0};
	char direct_target[256] = {0};
	char legacy_target[256] = {0};
	char plist_path[PATH_MAX] = {0};
	char legacy_plist_path[PATH_MAX] = {0};
	char persistent_plist_path[PATH_MAX] = {0};
	get_domain(domain);
	get_direct_target(direct_target);
	get_legacy_broker_target(legacy_target);
	get_plist_path(plist_path);
	get_legacy_plist_path(legacy_plist_path);
	if (!get_persistent_plist_path(persistent_plist_path)) {
		fprintf(stderr, "Could not determine persistent LaunchAgent path\n");
		return 3;
	}

	remove_development_registrations(legacy_target, direct_target, legacy_plist_path, plist_path);
	unlink(persistent_plist_path);

	/*
	 * Persistent installs intentionally do not snapshot the invoking shell's
	 * tuning/debug environment. Proven runtime behaviour belongs in source
	 * defaults; the LaunchAgent only carries lifecycle settings.
	 */
	if (!write_launch_agent_plist(persistent_plist_path, service_executable, false, true)) {
		return 4;
	}

	int ret = run_launchctl_bootstrap_with_retry(domain, persistent_plist_path);
	if (ret != 0) {
		fprintf(stderr, "launchctl bootstrap failed with status %d\n", ret);
		return 5;
	}

	char stdout_path[PATH_MAX] = {0};
	char stderr_path[PATH_MAX] = {0};
	if (!get_persistent_log_paths(stdout_path, stderr_path)) {
		return 6;
	}

	printf("monado-service installed as a persistent per-user LaunchAgent\n");
	printf("LaunchAgent label: %s\n", MONADO_XPC_LAUNCHD_LABEL);
	printf("Mach service: %s\n", IPC_METAL_XPC_SERVICE_NAME);
	printf("Executable: %s\n", service_executable);
	printf("LaunchAgent plist: %s\n", persistent_plist_path);
	printf("The LaunchAgent will be available again after logout/login or reboot and starts on demand via XPC\n");
	printf("No development XRT/PSVR2/Vulkan environment was captured\n");
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

static int
uninstall_service(void)
{
	char direct_target[256] = {0};
	char persistent_plist_path[PATH_MAX] = {0};
	get_direct_target(direct_target);
	if (!get_persistent_plist_path(persistent_plist_path)) {
		fprintf(stderr, "Could not determine persistent LaunchAgent path\n");
		return 1;
	}

	/* It is fine if the service is not currently loaded. */
	(void)run_launchctl("bootout", direct_target, NULL);
	if (unlink(persistent_plist_path) != 0 && errno != ENOENT) {
		fprintf(stderr, "Could not remove '%s': %s\n", persistent_plist_path, strerror(errno));
		return 2;
	}

	printf("Removed persistent monado-service LaunchAgent: %s\n", persistent_plist_path);
	return 0;
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
	if (argc == 2 && strcmp(argv[1], "install") == 0) {
		return install_service();
	}
	if (argc == 2 && strcmp(argv[1], "uninstall") == 0) {
		return uninstall_service();
	}

	fprintf(stderr, "usage: %s {bootstrap|bootout|install|uninstall}\n", argv[0]);
	fprintf(stderr, "  bootstrap/bootout are development registration controls using /tmp\n");
	fprintf(stderr, "  install/uninstall manage the persistent per-user LaunchAgent\n");
	return 64;
}

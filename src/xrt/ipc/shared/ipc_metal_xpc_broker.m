// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief launchd-managed XPC broker for Metal shared handles.
 * @ingroup ipc_shared
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "shared/ipc_metal_xpc.h"
#include "shared/ipc_metal_xpc_service_internal.h"
#include "os/os_time.h"

#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

@interface IPCMetalXPCBrokerListenerDelegate : NSObject <NSXPCListenerDelegate> {
	IPCMetalXPCServiceObject *_service;
}
@end

@implementation IPCMetalXPCBrokerListenerDelegate

- (instancetype)init
{
	self = [super init];
	if (self != nil) {
		_service = [[IPCMetalXPCServiceObject alloc] init];
	}
	return self;
}

- (void)dealloc
{
	[_service release];
	[super dealloc];
}

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)newConnection
{
	(void)listener;
	if (newConnection.effectiveUserIdentifier != getuid() || newConnection.processIdentifier <= 0) {
		return NO;
	}
	newConnection.exportedInterface = ipc_metal_xpc_create_interface();
	newConnection.exportedObject = _service;
	[newConnection resume];
	return YES;
}

- (void)expirePendingTokens:(NSTimer *)timer
{
	(void)timer;
	uint64_t now = os_monotonic_get_ns();
	if (now > IPC_METAL_XPC_TOKEN_LIFETIME_NS) {
		[_service expireTokensBefore:now - IPC_METAL_XPC_TOKEN_LIFETIME_NS];
	}
}

@end

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

static void
get_domain(char out_domain[64])
{
	snprintf(out_domain, 64, "gui/%u", (unsigned)getuid());
}

static void
get_target(char out_target[256])
{
	snprintf(out_target, 256, "gui/%u/%s", (unsigned)getuid(), IPC_METAL_XPC_SERVICE_NAME);
}

static void
get_plist_path(char out_path[PATH_MAX])
{
	snprintf(out_path, PATH_MAX, "/tmp/%s.%u.plist", IPC_METAL_XPC_SERVICE_NAME, (unsigned)getuid());
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
write_launch_agent_plist(const char *path, const char *executable)
{
	@autoreleasepool {
		NSString *exe = [NSString stringWithUTF8String:executable];
		NSString *plist_path = [NSString stringWithUTF8String:path];
		NSString *service = [NSString stringWithUTF8String:IPC_METAL_XPC_SERVICE_NAME];
		if (exe == nil || plist_path == nil || service == nil) {
			return false;
		}

		NSDictionary *plist = @{
			@"Label" : service,
			@"ProgramArguments" : @[ exe, @"--service" ],
			@"MachServices" : @{service : @YES},
			@"RunAtLoad" : @NO,
			@"ProcessType" : @"Interactive",
		};

		NSError *error = nil;
		NSData *data = [NSPropertyListSerialization dataWithPropertyList:plist
		                                                          format:NSPropertyListXMLFormat_v1_0
		                                                         options:0
		                                                           error:&error];
		if (data == nil || ![data writeToFile:plist_path options:NSDataWritingAtomic error:&error]) {
			const char *message = error != nil ? error.localizedDescription.UTF8String : "unknown error";
			fprintf(stderr, "Could not write Metal XPC broker LaunchAgent plist: %s\n",
			        message != NULL ? message : "unknown");
			return false;
		}

		return true;
	}
}

static int
bootstrap_broker(void)
{
	char executable[PATH_MAX] = {0};
	if (!get_executable_path(executable)) {
		fprintf(stderr, "Could not determine broker executable path\n");
		return 1;
	}

	char domain[64] = {0};
	char target[256] = {0};
	char plist_path[PATH_MAX] = {0};
	get_domain(domain);
	get_target(target);
	get_plist_path(plist_path);

	(void)run_launchctl("bootout", target, NULL);
	unlink(plist_path);

	if (!write_launch_agent_plist(plist_path, executable)) {
		return 2;
	}

	int ret = run_launchctl("bootstrap", domain, plist_path);
	if (ret != 0) {
		fprintf(stderr, "launchctl bootstrap failed with status %d\n", ret);
		return 3;
	}

	printf("Metal XPC broker registered as %s\n", IPC_METAL_XPC_SERVICE_NAME);
	printf("LaunchAgent plist: %s\n", plist_path);
	return 0;
}

static int
bootout_broker(void)
{
	char target[256] = {0};
	char plist_path[PATH_MAX] = {0};
	get_target(target);
	get_plist_path(plist_path);

	int ret = run_launchctl("bootout", target, NULL);
	if (ret != 0) {
		fprintf(stderr, "launchctl bootout returned %d\n", ret);
	}
	unlink(plist_path);
	return ret == 0 ? 0 : 1;
}

static int
run_service(void)
{
	@autoreleasepool {
		IPCMetalXPCBrokerListenerDelegate *delegate = [[IPCMetalXPCBrokerListenerDelegate alloc] init];
		NSXPCListener *listener = [[NSXPCListener alloc]
		    initWithMachServiceName:[NSString stringWithUTF8String:IPC_METAL_XPC_SERVICE_NAME]];
		listener.delegate = delegate;
		[listener resume];

		NSTimer *timer = [NSTimer scheduledTimerWithTimeInterval:1.0
		                                                  target:delegate
		                                                selector:@selector(expirePendingTokens:)
		                                                userInfo:nil
		                                                 repeats:YES];
		[[NSRunLoop currentRunLoop] run];
		[timer invalidate];

		[listener release];
		[delegate release];
		return 0;
	}
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--service") == 0) {
		return run_service();
	}
	if (argc == 2 && strcmp(argv[1], "bootstrap") == 0) {
		return bootstrap_broker();
	}
	if (argc == 2 && strcmp(argv[1], "bootout") == 0) {
		return bootout_broker();
	}

	fprintf(stderr, "usage: %s {bootstrap|bootout|--service}\n", argv[0]);
	return 64;
}

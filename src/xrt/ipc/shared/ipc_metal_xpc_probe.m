// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Cross-process XPC probe for Metal shared texture/event handles.
 * @ingroup ipc_shared
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <dispatch/dispatch.h>
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

@protocol IPCMetalXPCProbeProtocol
- (void)testTextureHandle:(MTLSharedTextureHandle *)handle reply:(void (^)(BOOL success))reply;
- (void)testEventHandle:(MTLSharedEventHandle *)handle reply:(void (^)(BOOL success))reply;
@end

@interface IPCMetalXPCProbeService : NSObject <IPCMetalXPCProbeProtocol>
@end

@implementation IPCMetalXPCProbeService

- (void)testTextureHandle:(MTLSharedTextureHandle *)handle reply:(void (^)(BOOL success))reply
{
	@autoreleasepool {
		id<MTLDevice> device = handle.device;
		id<MTLTexture> texture = device != nil ? [device newSharedTextureWithHandle:handle] : nil;
		BOOL valid = texture != nil && texture.textureType == MTLTextureType2DArray && texture.arrayLength == 2 &&
		             texture.width == 8 && texture.height == 8 && texture.pixelFormat == MTLPixelFormatRGBA8Unorm;
		if (texture != nil) {
			[texture release];
		}
		reply(valid);
	}
}

- (void)testEventHandle:(MTLSharedEventHandle *)handle reply:(void (^)(BOOL success))reply
{
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		id<MTLSharedEvent> event = device != nil ? [device newSharedEventWithHandle:handle] : nil;
		BOOL valid = event != nil && event.signaledValue == 37;
		if (valid) {
			event.signaledValue = 41;
		}
		if (event != nil) {
			[event release];
		}
		reply(valid);
	}
}

@end

@interface IPCMetalXPCProbeListenerDelegate : NSObject <NSXPCListenerDelegate>
{
	IPCMetalXPCProbeService *_service;
}
@end

@implementation IPCMetalXPCProbeListenerDelegate

- (instancetype)init
{
	self = [super init];
	if (self != nil) {
		_service = [[IPCMetalXPCProbeService alloc] init];
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
	newConnection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(IPCMetalXPCProbeProtocol)];
	newConnection.exportedObject = _service;
	[newConnection resume];
	return YES;
}

@end

static int
run_launchctl(const char *verb, const char *domain, const char *plist_path)
{
	char *const argv[] = {(char *)"/bin/launchctl", (char *)verb, (char *)domain, (char *)plist_path, NULL};
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
write_launch_agent_plist(const char *path, const char *executable, const char *service_name)
{
	@autoreleasepool {
		NSString *exe = [NSString stringWithUTF8String:executable];
		NSString *service = [NSString stringWithUTF8String:service_name];
		NSString *plist_path = [NSString stringWithUTF8String:path];
		if (exe == nil || service == nil || plist_path == nil) {
			return false;
		}

		NSDictionary *plist = @{
			@"Label" : service,
			@"ProgramArguments" : @[ exe, @"--xpc-service", service ],
			@"MachServices" : @{ service : @YES },
			@"RunAtLoad" : @NO,
		};

		NSError *error = nil;
		NSData *data = [NSPropertyListSerialization dataWithPropertyList:plist
		                                                        format:NSPropertyListXMLFormat_v1_0
		                                                       options:0
		                                                         error:&error];
		if (data == nil || ![data writeToFile:plist_path options:NSDataWritingAtomic error:&error]) {
			const char *message = error != nil ? error.localizedDescription.UTF8String : "unknown error";
			fprintf(stderr, "Could not write temporary launchd plist: %s\n", message != NULL ? message : "unknown");
			return false;
		}
		return true;
	}
}

static int
run_xpc_service(const char *service_name)
{
	@autoreleasepool {
		NSString *name = [NSString stringWithUTF8String:service_name];
		if (name == nil) {
			return 64;
		}

		IPCMetalXPCProbeListenerDelegate *delegate = [[IPCMetalXPCProbeListenerDelegate alloc] init];
		NSXPCListener *listener = [[NSXPCListener alloc] initWithMachServiceName:name];
		listener.delegate = delegate;
		[listener resume];
		[[NSRunLoop currentRunLoop] run];

		[listener release];
		[delegate release];
		return 0;
	}
}

static bool
call_texture_probe(NSXPCConnection *connection, MTLSharedTextureHandle *handle)
{
	__block BOOL success = NO;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCProbeProtocol> proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
		fprintf(stderr, "XPC texture call failed: %s\n", error.localizedDescription.UTF8String);
		dispatch_semaphore_signal(semaphore);
	}];
	[proxy testTextureHandle:handle reply:^(BOOL remote_success) {
		success = remote_success;
		replied = YES;
		dispatch_semaphore_signal(semaphore);
	}];

	long wait_result = dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC));
	return wait_result == 0 && replied && success;
}

static bool
call_event_probe(NSXPCConnection *connection, MTLSharedEventHandle *handle)
{
	__block BOOL success = NO;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCProbeProtocol> proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
		fprintf(stderr, "XPC event call failed: %s\n", error.localizedDescription.UTF8String);
		dispatch_semaphore_signal(semaphore);
	}];
	[proxy testEventHandle:handle reply:^(BOOL remote_success) {
		success = remote_success;
		replied = YES;
		dispatch_semaphore_signal(semaphore);
	}];

	long wait_result = dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC));
	return wait_result == 0 && replied && success;
}

static int
run_parent_probe(void)
{
	@autoreleasepool {
		char executable[PATH_MAX] = {0};
		if (!get_executable_path(executable)) {
			fprintf(stderr, "Could not determine probe executable path\n");
			return 1;
		}

		char service_name[192];
		snprintf(service_name, sizeof(service_name), "org.freedesktop.monado.metal-xpc-probe.%d", (int)getpid());

		char plist_path[PATH_MAX];
		snprintf(plist_path, sizeof(plist_path), "/tmp/%s.plist", service_name);
		if (!write_launch_agent_plist(plist_path, executable, service_name)) {
			return 2;
		}

		char domain[64];
		snprintf(domain, sizeof(domain), "gui/%u", (unsigned)getuid());
		int launch_ret = run_launchctl("bootstrap", domain, plist_path);
		if (launch_ret != 0) {
			fprintf(stderr, "launchctl bootstrap failed with status %d\n", launch_ret);
			unlink(plist_path);
			return 3;
		}

		int result = 4;
		NSString *service = [NSString stringWithUTF8String:service_name];
		NSXPCConnection *connection = [[NSXPCConnection alloc] initWithMachServiceName:service options:0];
		connection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(IPCMetalXPCProbeProtocol)];
		[connection resume];

		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil) {
			fprintf(stderr, "No Metal device available\n");
			goto cleanup;
		}

		MTLTextureDescriptor *descriptor = [[MTLTextureDescriptor alloc] init];
		descriptor.textureType = MTLTextureType2DArray;
		descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
		descriptor.width = 8;
		descriptor.height = 8;
		descriptor.depth = 1;
		descriptor.mipmapLevelCount = 1;
		descriptor.sampleCount = 1;
		descriptor.arrayLength = 2;
		descriptor.storageMode = MTLStorageModePrivate;
		descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

		id<MTLTexture> texture = [device newSharedTextureWithDescriptor:descriptor];
		[descriptor release];
		if (texture == nil) {
			fprintf(stderr, "Could not create shared 2D-array texture\n");
			goto cleanup;
		}

		MTLSharedTextureHandle *texture_handle = [texture newSharedTextureHandle];
		if (texture_handle == nil) {
			[texture release];
			fprintf(stderr, "Could not create MTLSharedTextureHandle\n");
			goto cleanup;
		}

		if (!call_texture_probe(connection, texture_handle)) {
			fprintf(stderr, "XPC shared-texture handle probe failed\n");
			[texture_handle release];
			[texture release];
			goto cleanup;
		}
		printf("texture: XPC child recreated shared 2D-array texture successfully\n");
		[texture_handle release];
		[texture release];

		id<MTLSharedEvent> event = [device newSharedEvent];
		if (event == nil) {
			fprintf(stderr, "Could not create MTLSharedEvent\n");
			goto cleanup;
		}
		event.signaledValue = 37;
		MTLSharedEventHandle *event_handle = [event newSharedEventHandle];
		if (event_handle == nil) {
			[event release];
			fprintf(stderr, "Could not create MTLSharedEventHandle\n");
			goto cleanup;
		}

		if (!call_event_probe(connection, event_handle) || event.signaledValue != 41) {
			fprintf(stderr,
			        "XPC shared-event handle probe failed (parent value=%llu)\n",
			        (unsigned long long)event.signaledValue);
			[event_handle release];
			[event release];
			goto cleanup;
		}
		printf("event: XPC child recreated and signaled shared event successfully\n");
		[event_handle release];
		[event release];

		printf("PASS: Metal shared texture and event handles survived a real XPC process boundary\n");
		result = 0;

	cleanup:
		[connection invalidate];
		[connection release];
		int bootout_ret = run_launchctl("bootout", domain, plist_path);
		if (bootout_ret != 0) {
			fprintf(stderr, "warning: launchctl bootout returned %d\n", bootout_ret);
		}
		unlink(plist_path);
		return result;
	}
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--xpc-service") == 0) {
		return run_xpc_service(argv[2]);
	}
	if (argc != 1) {
		fprintf(stderr, "usage: %s\n", argv[0]);
		return 64;
	}
	return run_parent_probe();
}

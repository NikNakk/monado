#pragma once
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Common native transport and resources for the Metal and Wine GPU probes.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <spawn.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
extern char **environ;
static unsigned
texture_width()
{
	const char *value = getenv("GPU_SYNC_TEXTURE_SIZE");
	if (value == nullptr)
		return 64;
	char *end = nullptr;
	unsigned long width = strtoul(value, &end, 10);
	if (end == value || *end != 0 || width < 16 || width > 2048) {
		// Fail here, not later as a zero-sized IOSurface or texture.
		fprintf(stderr, "FAIL GPU_SYNC_TEXTURE_SIZE must be a whole number from 16 to 2048, got '%s'\n",
		        value);
		exit(2);
	}
	return (unsigned)width;
}
static const unsigned W = texture_width(), BYTES = W * W * 4;
static constexpr unsigned N = 3;
static uint64_t
ns()
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}
static void
check(bool good, const char *msg)
{
	if (!good)
		throw std::runtime_error(msg);
}

static void
io(int fd, void *data, size_t size, bool output, int timeout_ms = 5000)
{
	auto *p = static_cast<char *>(data);
	while (size) {
		if (!output) {
			pollfd f{fd, POLLIN, 0};
			check(poll(&f, 1, timeout_ms) > 0, "transport timeout");
		}
		ssize_t count = output ? write(fd, p, size) : read(fd, p, size);
		check(count > 0, "transport closed");
		p += count;
		size -= count;
	}
}
#include "macos_gpu_sync_protocol.h"
static void
pattern(const Command &c, unsigned x, unsigned y, uint8_t p[4])
{
	gpu_sync_pattern(&c, x, y, p);
}
static IOSurfaceRef
surface()
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	NSDictionary *d = @{
		(id)kIOSurfaceWidth : @(W),
		(id)kIOSurfaceHeight : @(W),
		(id)kIOSurfaceBytesPerElement : @4,
		(id)kIOSurfaceBytesPerRow : @(W * 4),
		(id)kIOSurfaceAllocSize : @(BYTES),
		(id)kIOSurfacePixelFormat : @(0x42475241),
		(id)kIOSurfaceIsGlobal : @YES
	};
#pragma clang diagnostic pop
	return IOSurfaceCreate((CFDictionaryRef)d);
}
@protocol GPUSyncEventProtocol
- (void)eventHandle:(void (^)(MTLSharedEventHandle *))reply;
- (void)gateHandle:(void (^)(MTLSharedEventHandle *))reply;
@end
@interface GPUSyncEventService : NSObject <GPUSyncEventProtocol, NSXPCListenerDelegate>
@property(retain) id<MTLSharedEvent> event;
@property(retain) id<MTLSharedEvent> gate;
@end
@implementation GPUSyncEventService
- (void)eventHandle:(void (^)(MTLSharedEventHandle *))reply
{
	reply([[self.event newSharedEventHandle] autorelease]);
}
- (void)gateHandle:(void (^)(MTLSharedEventHandle *))reply
{
	reply([[self.gate newSharedEventHandle] autorelease]);
}
- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	(void)listener;
	connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(GPUSyncEventProtocol)];
	connection.exportedObject = self;
	[connection resume];
	return YES;
}
@end
static int
launchctl(const char *verb, const char *domain, const char *plist)
{
	char *args[] = {const_cast<char *>("/bin/launchctl"), const_cast<char *>(verb), const_cast<char *>(domain),
	                const_cast<char *>(plist), nullptr};
	pid_t pid;
	check(posix_spawn(&pid, "/bin/launchctl", nullptr, nullptr, args, environ) == 0, "launchctl spawn");
	int status;
	check(waitpid(pid, &status, 0) == pid, "launchctl wait");
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
static void
metal_complete(id<MTLCommandBuffer> buffer)
{
	uint64_t deadline = ns() + 1000000000;
	while (buffer.status < MTLCommandBufferStatusCompleted && ns() < deadline) {
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	check(buffer.status == MTLCommandBufferStatusCompleted, "producer GPU completion timeout");
}

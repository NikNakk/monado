// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Cross-process probe for Metal shared-handle byte serialization.
 * @ingroup ipc_shared
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "shared/ipc_metal_handle.h"

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static bool
write_all(int fd, const uint8_t *bytes, size_t size)
{
	size_t offset = 0;
	while (offset < size) {
		ssize_t written = write(fd, bytes + offset, size - offset);
		if (written < 0 && errno == EINTR) {
			continue;
		}
		if (written <= 0) {
			return false;
		}
		offset += (size_t)written;
	}
	return true;
}

static uint8_t *
read_file(const char *path, uint32_t *out_size)
{
	*out_size = 0;
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		return NULL;
	}

	struct stat st = {0};
	if (fstat(fd, &st) != 0 || st.st_size <= 0 || (uint64_t)st.st_size > UINT32_MAX) {
		close(fd);
		return NULL;
	}

	uint32_t size = (uint32_t)st.st_size;
	uint8_t *bytes = malloc(size);
	if (bytes == NULL) {
		close(fd);
		return NULL;
	}

	size_t offset = 0;
	while (offset < size) {
		ssize_t got = read(fd, bytes + offset, size - offset);
		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			free(bytes);
			close(fd);
			return NULL;
		}
		offset += (size_t)got;
	}

	close(fd);
	*out_size = size;
	return bytes;
}

static int
child_texture(const char *path)
{
	uint32_t size = 0;
	uint8_t *bytes = read_file(path, &size);
	if (bytes == NULL) {
		fprintf(stderr, "texture child: could not read archive\n");
		return 10;
	}

	void *raw_handle = NULL;
	xrt_result_t xret = ipc_metal_unarchive_shared_texture_handle(bytes, size, &raw_handle);
	free(bytes);
	if (xret != XRT_SUCCESS || raw_handle == NULL) {
		fprintf(stderr, "texture child: unarchive failed: %d\n", xret);
		return 11;
	}

	MTLSharedTextureHandle *handle = (__bridge MTLSharedTextureHandle *)raw_handle;
	id<MTLDevice> device = handle.device;
	id<MTLTexture> texture = device != nil ? [device newSharedTextureWithHandle:handle] : nil;
	bool valid = texture != nil && texture.textureType == MTLTextureType2DArray && texture.arrayLength == 2 &&
	             texture.width == 8 && texture.height == 8 && texture.pixelFormat == MTLPixelFormatRGBA8Unorm;

	if (texture != nil) {
		[texture release];
	}
	ipc_metal_shared_handle_release(raw_handle);

	if (!valid) {
		fprintf(stderr, "texture child: decoded handle did not recreate the expected 2D-array texture\n");
		return 12;
	}

	printf("texture child: recreated shared 2D-array texture successfully\n");
	return 0;
}

static int
child_event(const char *path)
{
	uint32_t size = 0;
	uint8_t *bytes = read_file(path, &size);
	if (bytes == NULL) {
		fprintf(stderr, "event child: could not read archive\n");
		return 20;
	}

	void *raw_handle = NULL;
	xrt_result_t xret = ipc_metal_unarchive_shared_event_handle(bytes, size, &raw_handle);
	free(bytes);
	if (xret != XRT_SUCCESS || raw_handle == NULL) {
		fprintf(stderr, "event child: unarchive failed: %d\n", xret);
		return 21;
	}

	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	MTLSharedEventHandle *handle = (__bridge MTLSharedEventHandle *)raw_handle;
	id<MTLSharedEvent> event = device != nil ? [device newSharedEventWithHandle:handle] : nil;
	if (event == nil || event.signaledValue != 37) {
		fprintf(stderr, "event child: shared event recreation/value check failed\n");
		ipc_metal_shared_handle_release(raw_handle);
		return 22;
	}

	event.signaledValue = 41;
	[event release];
	ipc_metal_shared_handle_release(raw_handle);
	printf("event child: recreated and signaled shared event successfully\n");
	return 0;
}

static int
spawn_archive_child(const char *self, const char *mode, const uint8_t *bytes, uint32_t size)
{
	char path[] = "/tmp/monado-metal-handle-XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) {
		fprintf(stderr, "could not create temporary archive: %s\n", strerror(errno));
		return 30;
	}

	bool wrote = write_all(fd, bytes, size);
	close(fd);
	if (!wrote) {
		unlink(path);
		fprintf(stderr, "could not write temporary archive\n");
		return 31;
	}

	char *const child_argv[] = {(char *)self, (char *)mode, path, NULL};
	pid_t pid = 0;
	int ret = posix_spawnp(&pid, self, NULL, NULL, child_argv, environ);
	if (ret != 0) {
		unlink(path);
		fprintf(stderr, "posix_spawnp failed: %s\n", strerror(ret));
		return 32;
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			unlink(path);
			return 33;
		}
	}
	unlink(path);

	if (!WIFEXITED(status)) {
		return 34;
	}
	return WEXITSTATUS(status);
}

static int
parent_probe(const char *self)
{
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil) {
			fprintf(stderr, "No Metal device available\n");
			return 1;
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
			return 2;
		}

		MTLSharedTextureHandle *texture_handle = [texture newSharedTextureHandle];
		if (texture_handle == nil) {
			[texture release];
			fprintf(stderr, "Could not create MTLSharedTextureHandle\n");
			return 3;
		}

		uint8_t *texture_bytes = NULL;
		uint32_t texture_size = 0;
		xrt_result_t xret = ipc_metal_archive_shared_texture_handle((__bridge void *)texture_handle,
		                                                              &texture_bytes,
		                                                              &texture_size);
		if (xret != XRT_SUCCESS) {
			[texture_handle release];
			[texture release];
			fprintf(stderr, "Could not archive shared texture handle: %d\n", xret);
			return 4;
		}

		printf("texture parent: archive size %u bytes\n", texture_size);
		int child_result = spawn_archive_child(self, "--texture-child", texture_bytes, texture_size);
		free(texture_bytes);
		[texture_handle release];
		[texture release];
		if (child_result != 0) {
			fprintf(stderr, "Cross-process shared-texture archive probe failed: child=%d\n", child_result);
			return 5;
		}

		id<MTLSharedEvent> event = [device newSharedEvent];
		if (event == nil) {
			fprintf(stderr, "Could not create MTLSharedEvent\n");
			return 6;
		}
		event.signaledValue = 37;
		MTLSharedEventHandle *event_handle = [event newSharedEventHandle];
		if (event_handle == nil) {
			[event release];
			fprintf(stderr, "Could not create MTLSharedEventHandle\n");
			return 7;
		}

		uint8_t *event_bytes = NULL;
		uint32_t event_size = 0;
		xret = ipc_metal_archive_shared_event_handle((__bridge void *)event_handle, &event_bytes, &event_size);
		if (xret != XRT_SUCCESS) {
			[event_handle release];
			[event release];
			fprintf(stderr, "Could not archive shared event handle: %d\n", xret);
			return 8;
		}

		printf("event parent: archive size %u bytes\n", event_size);
		child_result = spawn_archive_child(self, "--event-child", event_bytes, event_size);
		free(event_bytes);
		[event_handle release];
		if (child_result != 0 || event.signaledValue != 41) {
			fprintf(stderr,
			        "Cross-process shared-event archive probe failed: child=%d parent_value=%llu\n",
			        child_result,
			        (unsigned long long)event.signaledValue);
			[event release];
			return 9;
		}
		[event release];

		printf("PASS: keyed Metal shared handles survived a real process boundary\n");
		return 0;
	}
}

int
main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "--texture-child") == 0) {
		return child_texture(argv[2]);
	}
	if (argc == 3 && strcmp(argv[1], "--event-child") == 0) {
		return child_event(argv[2]);
	}
	if (argc != 1) {
		fprintf(stderr, "usage: %s\n", argv[0]);
		return 64;
	}
	return parent_probe(argv[0]);
}

// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  IPC shared memory helpers
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc_shared
 */

#include <xrt/xrt_config_os.h>

#include "shared/ipc_shmem.h"

#if defined(XRT_OS_UNIX)
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(XRT_OS_ANDROID)
#include <android/sharedmem.h>
#elif defined(XRT_OS_UNIX)
// non-android unix
#include <sys/stat.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#endif

#if defined(XRT_OS_ANDROID)

#if __ANDROID_API__ < 26
#error "Android API level 26 or higher needed for ASharedMemory_create"
#endif
xrt_result_t

ipc_shmem_create(size_t size, xrt_shmem_handle_t *out_handle, void **out_map)
{

	int fd = ASharedMemory_create("monado", size);
	if (fd < 0) {
		return XRT_ERROR_IPC_FAILURE;
	}
	xrt_result_t result = ipc_shmem_map(fd, size, out_map);
	if (result != XRT_SUCCESS) {
		close(fd);
		return result;
	}
	*out_handle = fd;
	return XRT_SUCCESS;
}

#elif defined(XRT_OS_UNIX)

#define MONADO_SHMEM_NAME "/monado_shm"

static xrt_result_t
shmem_create_named(const char *name,
                   bool exclusive,
                   bool readonly_handle,
                   size_t size,
                   xrt_shmem_handle_t *out_handle,
                   void **out_map);

// Impl for non-Android Unix.
xrt_result_t
ipc_shmem_create(size_t size, xrt_shmem_handle_t *out_handle, void **out_map)
{
	return shmem_create_named(MONADO_SHMEM_NAME, false, false, size, out_handle, out_map);
}

xrt_result_t
ipc_shmem_create_private(const char *suffix, size_t size, xrt_shmem_handle_t *out_handle, void **out_map)
{
	// A name of its own, so it cannot meet a concurrent ipc_shmem_create().
	char name[64];
	snprintf(name, sizeof(name), "/monado_%s_%d", suffix, (int)getpid());
	return shmem_create_named(name, true, false, size, out_handle, out_map);
}

xrt_result_t
ipc_shmem_create_private_readonly(const char *suffix, size_t size, xrt_shmem_handle_t *out_handle, void **out_map)
{
	char name[64];
	snprintf(name, sizeof(name), "/monado_%s_%d", suffix, (int)getpid());
	return shmem_create_named(name, true, true, size, out_handle, out_map);
}

static xrt_result_t
shmem_create_named(
    const char *name, bool exclusive, bool readonly_handle, size_t size, xrt_shmem_handle_t *out_handle, void **out_map)
{
	*out_handle = XRT_SHMEM_HANDLE_INVALID;
	*out_map = NULL;
	int writer = shm_open(name, O_CREAT | O_RDWR | (exclusive ? O_EXCL : 0), S_IRUSR | S_IWUSR);
	if (writer < 0) {
		return XRT_ERROR_IPC_FAILURE;
	}

	int consumer = writer;
	xrt_result_t xret = XRT_ERROR_IPC_FAILURE;
	if (ftruncate(writer, size) == 0) {
		// Open before unlinking: dup() would preserve the writer's access rights.
		if (readonly_handle) {
			consumer = shm_open(name, O_RDONLY, 0);
		}
		if (consumer >= 0) {
			xret = ipc_shmem_map(writer, size, out_map);
		}
	}
	shm_unlink(name);
	if (xret != XRT_SUCCESS || readonly_handle) {
		close(writer);
	}
	if (xret != XRT_SUCCESS) {
		if (consumer >= 0 && consumer != writer) {
			close(consumer);
		}
		return xret;
	}
	*out_handle = consumer;
	return XRT_SUCCESS;
}

#elif defined(XRT_OS_WINDOWS)

xrt_result_t
ipc_shmem_create(size_t size, xrt_shmem_handle_t *out_handle, void **out_map)
{
	*out_handle = NULL;
	LARGE_INTEGER sz = {.QuadPart = size};
	HANDLE handle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, sz.HighPart, sz.LowPart, NULL);
	if (handle == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}

	xrt_result_t result = ipc_shmem_map(handle, size, out_map);
	if (result != XRT_SUCCESS) {
		CloseHandle(handle);
		return result;
	}

	*out_handle = handle;
	return XRT_SUCCESS;
}

#else
#error "OS not yet supported"
#endif

#if defined(XRT_OS_UNIX)

void
ipc_shmem_destroy(xrt_shmem_handle_t *handle_ptr, void **map_ptr, size_t size)
{
	// Checks for NULL.
	ipc_shmem_unmap((void **)map_ptr, size);

	if (handle_ptr == NULL) {
		return;
	}
	xrt_shmem_handle_t handle = *handle_ptr;
	if (handle < 0) {
		return;
	}
	close(handle);
	*handle_ptr = -1;
}

xrt_result_t
ipc_shmem_map(xrt_shmem_handle_t handle, size_t size, void **out_map)
{

	const int access = PROT_READ | PROT_WRITE;
	const int flags = MAP_SHARED;
	void *ptr = mmap(NULL, size, access, flags, handle, 0);
	if (ptr == MAP_FAILED) {
		*out_map = NULL;
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_map = ptr;
	return XRT_SUCCESS;
}

xrt_result_t
ipc_shmem_map_readonly(xrt_shmem_handle_t handle, size_t size, void **out_map)
{
	void *ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, handle, 0);
	if (ptr == MAP_FAILED) {
		*out_map = NULL;
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_map = ptr;
	return XRT_SUCCESS;
}

void
ipc_shmem_unmap(void **map_ptr, size_t size)
{
	if (map_ptr == NULL) {
		return;
	}
	munmap(*map_ptr, size);
	*map_ptr = NULL;
}

#elif defined(XRT_OS_WINDOWS)

void
ipc_shmem_destroy(xrt_shmem_handle_t *handle_ptr, void **map_ptr, size_t size)
{
	// Checks for NULL.
	ipc_shmem_unmap((void **)map_ptr, size);

	if (handle_ptr == NULL) {
		return;
	}
	xrt_shmem_handle_t handle = *handle_ptr;
	CloseHandle(handle);
	*handle_ptr = NULL;
}

xrt_result_t
ipc_shmem_map(xrt_shmem_handle_t handle, size_t size, void **out_map)
{
	void *ptr = MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
	if (ptr == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	*out_map = ptr;
	return XRT_SUCCESS;
}

void
ipc_shmem_unmap(void **map_ptr, size_t size)
{
	if (map_ptr == NULL) {
		return;
	}
	void *map = *map_ptr;
	if (map == NULL) {
		return;
	}
	UnmapViewOfFile(map);
	*map_ptr = NULL;
}

#else
#error "OS not yet supported"
#endif

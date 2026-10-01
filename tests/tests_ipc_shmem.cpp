// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "shared/ipc_shmem.h"
#include "util/u_frame.h"
#include "util/u_frame_share.h"
#include "catch_amalgamated.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
struct Share
{
	size_t size = u_frame_share_size(1, 256);
	xrt_shmem_handle_t handle = XRT_SHMEM_HANDLE_INVALID;
	void *producer = nullptr;
	void *consumer = nullptr;
	~Share()
	{
		ipc_shmem_unmap(&consumer, size);
		ipc_shmem_destroy(&handle, &producer, size);
	}
};
} // namespace

TEST_CASE("passthrough descriptor prevents consumer writes while producer publishes")
{
	Share share;
	REQUIRE(ipc_shmem_create_private_readonly("test_passthrough", share.size, &share.handle, &share.producer) ==
	        XRT_SUCCESS);
	REQUIRE(u_frame_share_init(share.producer, share.size, 1, 256));
	REQUIRE(ipc_shmem_map_readonly(share.handle, share.size, &share.consumer) == XRT_SUCCESS);
	CHECK((fcntl(share.handle, F_GETFL) & O_ACCMODE) == O_RDONLY);
	void *writable = mmap(nullptr, share.size, PROT_READ | PROT_WRITE, MAP_SHARED, share.handle, 0);
	CHECK(writable == MAP_FAILED);
	if (writable != MAP_FAILED) {
		munmap(writable, share.size);
	}
	CHECK(mprotect(share.consumer, share.size, PROT_READ | PROT_WRITE) == -1);
	CHECK(ftruncate(share.handle, share.size) == -1);
	const char byte = 0;
	CHECK(pwrite(share.handle, &byte, 1, 0) == -1);
	CHECK(u_frame_share_is_valid(share.consumer, share.size));
	struct xrt_frame *frame = nullptr;
	u_frame_create_one_off(XRT_FORMAT_L8, 4, 4, &frame);
	frame->data[0] = 0x42;
	CHECK(u_frame_share_write(share.producer, 0, frame));
	xrt_frame_reference(&frame, nullptr);
	uint64_t sequence = 0;
	CHECK(u_frame_share_read(share.consumer, 0, &sequence, &frame));
	if (frame != nullptr) {
		CHECK(frame->data[0] == 0x42);
		xrt_frame_reference(&frame, nullptr);
	}
}

TEST_CASE("failed shared memory maps return a null output")
{
	void *map = reinterpret_cast<void *>(1);
	CHECK(ipc_shmem_map(XRT_SHMEM_HANDLE_INVALID, 4096, &map) == XRT_ERROR_IPC_FAILURE);
	CHECK(map == nullptr);
	map = reinterpret_cast<void *>(1);
	CHECK(ipc_shmem_map_readonly(XRT_SHMEM_HANDLE_INVALID, 4096, &map) == XRT_ERROR_IPC_FAILURE);
	CHECK(map == nullptr);
}

TEST_CASE("ordinary shared memory keeps writable producer and consumer mappings")
{
	Share share;
	REQUIRE(ipc_shmem_create_private("test_writable", share.size, &share.handle, &share.producer) == XRT_SUCCESS);
	CHECK((fcntl(share.handle, F_GETFL) & O_ACCMODE) == O_RDWR);
	REQUIRE(ipc_shmem_map(share.handle, share.size, &share.consumer) == XRT_SUCCESS);
	static_cast<char *>(share.consumer)[0] = 0x42;
	CHECK(static_cast<char *>(share.producer)[0] == 0x42);
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "shared/ipc_metal_xpc_service_internal.h"
#include "catch_amalgamated.hpp"
#include "os/os_time.h"

namespace {
struct Registry
{
	IPCMetalXPCServiceObject *value = [[IPCMetalXPCServiceObject alloc] init];
	NSObject *object = [[NSObject alloc] init];
	~Registry()
	{
		[object release];
		[value release];
	}
	bool
	put(uint64_t n, pid_t pid = 123, uint32_t index = 0, uint32_t count = 1)
	{
		return [value storeImageObject:object
		                         token:IPC_METAL_XPC_TOKEN_MAGIC | n
		                         index:index
		                    imageCount:count
		                      ownerPID:pid];
	}
};
} // namespace
TEST_CASE("Metal registry bounds pending tokens per process and returns capacity after discard")
{
	@autoreleasepool {
		Registry r;
		for (unsigned i = 0; i < IPC_METAL_XPC_MAX_TOKENS_PER_PID; ++i)
			REQUIRE(r.put(i));
		CHECK_FALSE(r.put(1000));
		[r.value discardToken:IPC_METAL_XPC_TOKEN_MAGIC ownerPID:456];
		CHECK_FALSE(r.put(1000));
		[r.value discardToken:IPC_METAL_XPC_TOKEN_MAGIC ownerPID:123];
		CHECK(r.put(1000));
	}
}
TEST_CASE("Metal registry bounds retained images across tokens")
{
	@autoreleasepool {
		Registry r;
		uint32_t count = XRT_MAX_SWAPCHAIN_IMAGES;
		for (unsigned i = 0; i < IPC_METAL_XPC_MAX_IMAGES_PER_PID; ++i)
			REQUIRE(r.put(i / count, 123, i % count, count));
		CHECK_FALSE(r.put(1000));
		CHECK(r.put(0, 123, 0, count)); // replacement does not consume another slot
		[r.value discardAllForPID:123];
		CHECK(r.put(1000));
	}
}
TEST_CASE("Metal registry expires abandoned publications without an ordinary IPC connection")
{
	@autoreleasepool {
		Registry r;
		REQUIRE(r.put(1));
		uint64_t now = os_monotonic_get_ns();
		[r.value expireTokensBefore:now > IPC_METAL_XPC_TOKEN_LIFETIME_NS
		                                ? now - IPC_METAL_XPC_TOKEN_LIFETIME_NS
						: 0];
		id object = [r.value copyImageObjectForToken:IPC_METAL_XPC_TOKEN_MAGIC | 1
		                                       index:0
		                                    ownerPID:123
		                               expectedClass:[NSObject class]];
		CHECK(object != nil);
		[object release];
		[r.value expireTokensBefore:UINT64_MAX];
		object = [r.value copyImageObjectForToken:IPC_METAL_XPC_TOKEN_MAGIC | 1
		                                    index:0
		                                 ownerPID:123
		                            expectedClass:[NSObject class]];
		CHECK(object == nil);
		[object release];
		CHECK(r.put(1, 456));
	}
}

TEST_CASE("Metal registry bounds global token and image retention across processes")
{
	@autoreleasepool {
		Registry r;
		for (unsigned i = 0; i < IPC_METAL_XPC_MAX_TOKENS; ++i) {
			REQUIRE([r.value storeSharedEventHandle:(MTLSharedEventHandle *)r.object
			                                  token:IPC_METAL_XPC_TOKEN_MAGIC | i
			                               ownerPID:100 + i % 32]);
		}
		CHECK_FALSE(r.put(2000, 999));
		[r.value discardAllForPID:100];
		CHECK(r.put(2000, 999));
	}
	@autoreleasepool {
		Registry r;
		unsigned count = XRT_MAX_SWAPCHAIN_IMAGES;
		for (unsigned i = 0; i < IPC_METAL_XPC_MAX_IMAGES; ++i)
			REQUIRE(r.put(i / count, 100 + i / IPC_METAL_XPC_MAX_IMAGES_PER_PID, i % count, count));
		CHECK_FALSE(r.put(2000, 999));
		CHECK(r.put(0, 100, 0, count));
	}
}
TEST_CASE("claimable texture transfers validate the request before changing ownership")
{
	@autoreleasepool {
		Registry r;
		uint64_t token = IPC_METAL_XPC_EXTERNAL_TOKEN_MAGIC | 1;
		REQUIRE([r.value storeImageObject:r.object token:token index:0 imageCount:1 ownerPID:123]);
		REQUIRE([r.value markTextureTokenClaimable:token ownerPID:123]);
		id object = [r.value copyImageObjectForToken:token index:1 ownerPID:456 expectedClass:[NSObject class]];
		CHECK(object == nil);
		[object release];
		object = [r.value copyImageObjectForToken:token index:0 ownerPID:456 expectedClass:[NSString class]];
		CHECK(object == nil);
		[object release];
		object = [r.value copyImageObjectForToken:token index:0 ownerPID:123 expectedClass:[NSObject class]];
		CHECK(object != nil);
		[object release];
		object = [r.value copyImageObjectForToken:token index:0 ownerPID:456 expectedClass:[NSObject class]];
		CHECK(object != nil);
		[object release];
		object = [r.value copyImageObjectForToken:token index:0 ownerPID:123 expectedClass:[NSObject class]];
		CHECK(object == nil);
		[object release];
	}
}

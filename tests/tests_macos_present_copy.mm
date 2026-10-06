// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Tests for the macOS presenter's final copy into a drawable.
 *
 * The destination stands in for a BGRA8 CAMetalLayer drawable. Each case
 * checks that the bytes reaching it are the source's display values.
 */

#import <Metal/Metal.h>

#include "main/comp_macos_present_copy.h"

#include "catch_amalgamated.hpp"

#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

constexpr uint32_t kWidth = 4;
constexpr uint32_t kHeight = 2;

id<MTLTexture>
make_texture(id<MTLDevice> device, MTLPixelFormat format, uint32_t width, uint32_t height)
{
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
	                                                                                width:width
	                                                                               height:height
	                                                                            mipmapped:NO];
	[desc setUsage:MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget];
	[desc setStorageMode:MTLStorageModeShared];
	return [device newTextureWithDescriptor:desc];
}

void
upload(id<MTLTexture> texture, const std::vector<uint8_t> &bytes)
{
	[texture replaceRegion:MTLRegionMake2D(0, 0, [texture width], [texture height])
	           mipmapLevel:0
	             withBytes:bytes.data()
	           bytesPerRow:[texture width] * 4];
}

std::vector<uint8_t>
download(id<MTLTexture> texture)
{
	std::vector<uint8_t> bytes([texture width] * [texture height] * 4);
	[texture getBytes:bytes.data()
	      bytesPerRow:[texture width] * 4
	       fromRegion:MTLRegionMake2D(0, 0, [texture width], [texture height])
	      mipmapLevel:0];
	return bytes;
}

//! Distinct values per pixel and channel, so swaps and flips show.
std::vector<uint8_t>
pattern(uint32_t width, uint32_t height)
{
	std::vector<uint8_t> bytes(width * height * 4);
	for (uint32_t i = 0; i < width * height; i++) {
		bytes[i * 4 + 0] = (uint8_t)(10 + i * 20);
		bytes[i * 4 + 1] = (uint8_t)(30 + i * 15);
		bytes[i * 4 + 2] = (uint8_t)(200 - i * 10);
		bytes[i * 4 + 3] = 255;
	}
	return bytes;
}

bool
run_copy(id<MTLDevice> device, comp_macos_present_copy *pc, id<MTLTexture> src, id<MTLTexture> dst)
{
	id<MTLCommandQueue> queue = [device newCommandQueue];
	id<MTLCommandBuffer> cb = [queue commandBuffer];
	bool ok = comp_macos_present_copy_encode(pc, cb, src, dst);
	[cb commit];
	[cb waitUntilCompleted];
	ok = ok && [cb status] == MTLCommandBufferStatusCompleted;
	[queue release];
	return ok;
}

bool
near(uint8_t a, uint8_t b)
{
	return std::abs((int)a - (int)b) <= 1;
}

} // namespace

TEST_CASE("macOS present copy")
{
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (device == nil) {
		SKIP("No Metal device");
	}
	comp_macos_present_copy *pc = comp_macos_present_copy_create(device);
	REQUIRE(pc != nullptr);
	std::vector<uint8_t> src_bytes = pattern(kWidth, kHeight);
	id<MTLTexture> dst = make_texture(device, MTLPixelFormatBGRA8Unorm, kWidth, kHeight);

	SECTION("matching format and size is blitted unchanged")
	{
		id<MTLTexture> src = make_texture(device, MTLPixelFormatBGRA8Unorm, kWidth, kHeight);
		upload(src, src_bytes);
		CHECK_FALSE(comp_macos_present_copy_needs_draw(src, dst));
		REQUIRE(run_copy(device, pc, src, dst));
		CHECK(download(dst) == src_bytes);
		[src release];
	}

	SECTION("RGBA is drawn into BGRA with channels in place")
	{
		id<MTLTexture> src = make_texture(device, MTLPixelFormatRGBA8Unorm, kWidth, kHeight);
		upload(src, src_bytes);
		CHECK(comp_macos_present_copy_needs_draw(src, dst));
		REQUIRE(run_copy(device, pc, src, dst));
		std::vector<uint8_t> out = download(dst);
		for (uint32_t i = 0; i < kWidth * kHeight; i++) {
			INFO("pixel " << i);
			CHECK(out[i * 4 + 0] == src_bytes[i * 4 + 2]);
			CHECK(out[i * 4 + 1] == src_bytes[i * 4 + 1]);
			CHECK(out[i * 4 + 2] == src_bytes[i * 4 + 0]);
			CHECK(out[i * 4 + 3] == src_bytes[i * 4 + 3]);
		}
		[src release];
	}

	SECTION("an sRGB source keeps its encoded values")
	{
		id<MTLTexture> src = make_texture(device, MTLPixelFormatRGBA8Unorm_sRGB, kWidth, kHeight);
		upload(src, src_bytes);
		REQUIRE(run_copy(device, pc, src, dst));
		std::vector<uint8_t> out = download(dst);
		for (uint32_t i = 0; i < kWidth * kHeight; i++) {
			INFO("pixel " << i);
			CHECK(near(out[i * 4 + 0], src_bytes[i * 4 + 2]));
			CHECK(near(out[i * 4 + 1], src_bytes[i * 4 + 1]));
			CHECK(near(out[i * 4 + 2], src_bytes[i * 4 + 0]));
		}
		[src release];
	}

	SECTION("a smaller source is scaled to the whole destination")
	{
		id<MTLTexture> src = make_texture(device, MTLPixelFormatRGBA8Unorm, 1, 1);
		upload(src, {40, 80, 120, 255});
		REQUIRE(run_copy(device, pc, src, dst));
		std::vector<uint8_t> out = download(dst);
		for (uint32_t i = 0; i < kWidth * kHeight; i++) {
			INFO("pixel " << i);
			CHECK(out[i * 4 + 0] == 120);
			CHECK(out[i * 4 + 1] == 80);
			CHECK(out[i * 4 + 2] == 40);
		}
		[src release];
	}

	[dst release];
	comp_macos_present_copy_destroy(&pc);
	CHECK(pc == nullptr);
	[device release];
}

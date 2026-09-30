// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief u_frame_share tests.
 */

#include <util/u_frame_share.h>
#include <util/u_frame.h>

#include "catch_amalgamated.hpp"

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace {

struct Share
{
	std::vector<uint8_t> mem;

	Share(uint32_t streams, size_t max_frame_size)
	{
		mem.resize(u_frame_share_size(streams, max_frame_size));
		REQUIRE(u_frame_share_init(mem.data(), mem.size(), streams, max_frame_size));
	}
};

struct xrt_frame *
make_frame(enum xrt_format format, uint32_t w, uint32_t h, uint8_t fill)
{
	struct xrt_frame *xf = nullptr;
	u_frame_create_one_off(format, w, h, &xf);
	std::memset(xf->data, fill, xf->size);
	return xf;
}

} // namespace

TEST_CASE("u_frame_share layout")
{
	CHECK(u_frame_share_size(0, 100) == 0);
	CHECK(u_frame_share_size(U_FRAME_SHARE_MAX_STREAMS + 1, 100) == 0);
	CHECK(u_frame_share_size(1, 0) == 0);

	std::vector<uint8_t> mem(u_frame_share_size(2, 1000));
	CHECK_FALSE(u_frame_share_is_valid(mem.data(), mem.size()));
	CHECK_FALSE(u_frame_share_init(mem.data(), mem.size() - 1, 2, 1000));
	REQUIRE(u_frame_share_init(mem.data(), mem.size(), 2, 1000));
	CHECK(u_frame_share_is_valid(mem.data(), mem.size()));
	CHECK_FALSE(u_frame_share_is_valid(mem.data(), 16));
}

TEST_CASE("u_frame_share passes the newest BC4 camera frame per stream")
{
	// The PS VR2 passthrough frame: BC4, 1024 x 1016.
	Share share(2, 1024 * 1016 / 2);

	uint64_t seq[2] = {0, 0};
	struct xrt_frame *out = nullptr;
	CHECK_FALSE(u_frame_share_read(share.mem.data(), 0, &seq[0], &out));

	struct xrt_frame *left = make_frame(XRT_FORMAT_BC4, 1024, 1016, 0x11);
	struct xrt_frame *right = make_frame(XRT_FORMAT_BC4, 1024, 1016, 0x22);
	left->timestamp = 1000;
	left->source_timestamp = 900;
	REQUIRE(u_frame_share_write(share.mem.data(), 0, left));
	REQUIRE(u_frame_share_write(share.mem.data(), 1, right));

	REQUIRE(u_frame_share_read(share.mem.data(), 0, &seq[0], &out));
	CHECK(out->format == XRT_FORMAT_BC4);
	CHECK(out->width == 1024);
	CHECK(out->height == 1016);
	CHECK(out->size == left->size);
	CHECK(out->timestamp == 1000);
	CHECK(out->source_timestamp == 900);
	CHECK(std::memcmp(out->data, left->data, left->size) == 0);
	xrt_frame_reference(&out, nullptr);

	// Nothing new until the next write.
	CHECK_FALSE(u_frame_share_read(share.mem.data(), 0, &seq[0], &out));

	REQUIRE(u_frame_share_read(share.mem.data(), 1, &seq[1], &out));
	CHECK(out->data[0] == 0x22);
	xrt_frame_reference(&out, nullptr);

	// A reader that fell behind gets the newest frame, not the ones it missed.
	for (uint8_t i = 1; i <= 5; i++) {
		std::memset(left->data, i, left->size);
		REQUIRE(u_frame_share_write(share.mem.data(), 0, left));
	}
	REQUIRE(u_frame_share_read(share.mem.data(), 0, &seq[0], &out));
	CHECK(out->data[0] == 5);
	CHECK(seq[0] == 6);
	xrt_frame_reference(&out, nullptr);

	xrt_frame_reference(&left, nullptr);
	xrt_frame_reference(&right, nullptr);
}

TEST_CASE("u_frame_share packs a region of a wider frame")
{
	// Like the PS VR2's 640 x 640 per-eye regions of a 1280 x 640 L8 frame.
	Share share(2, 640 * 640);

	struct xrt_frame *wide = make_frame(XRT_FORMAT_L8, 1280, 640, 0);
	for (uint32_t y = 0; y < 640; y++) {
		std::memset(wide->data + y * wide->stride, 0xAA, 640);
		std::memset(wide->data + y * wide->stride + 640, 0xBB, 640);
	}

	struct xrt_rect roi = {};
	roi.offset.w = 640;
	roi.extent.w = 640;
	roi.extent.h = 640;
	struct xrt_frame *right = nullptr;
	u_frame_create_roi(wide, roi, &right);
	REQUIRE(right != nullptr);

	REQUIRE(u_frame_share_write(share.mem.data(), 1, right));

	uint64_t seq = 0;
	struct xrt_frame *out = nullptr;
	REQUIRE(u_frame_share_read(share.mem.data(), 1, &seq, &out));
	CHECK(out->width == 640);
	CHECK(out->height == 640);
	CHECK(out->stride == 640);
	bool all_right = true;
	for (size_t i = 0; i < out->size; i++) {
		all_right = all_right && out->data[i] == 0xBB;
	}
	CHECK(all_right);

	xrt_frame_reference(&out, nullptr);
	xrt_frame_reference(&right, nullptr);
	xrt_frame_reference(&wide, nullptr);
}

TEST_CASE("u_frame_share rejects what does not fit")
{
	Share share(1, 1000);
	struct xrt_frame *big = make_frame(XRT_FORMAT_L8, 100, 100, 1);
	CHECK_FALSE(u_frame_share_write(share.mem.data(), 0, big));
	CHECK_FALSE(u_frame_share_write(share.mem.data(), 1, big));
	xrt_frame_reference(&big, nullptr);
}

TEST_CASE("u_frame_share never hands out a torn frame")
{
	// Small frames and no pause in the writer, so the reader is overtaken often.
	Share share(1, 64 * 64);
	std::atomic<bool> stop{false};

	std::thread writer([&] {
		struct xrt_frame *xf = make_frame(XRT_FORMAT_L8, 64, 64, 0);
		for (uint32_t i = 1; !stop.load(); i++) {
			std::memset(xf->data, (uint8_t)i, xf->size);
			xf->timestamp = i;
			u_frame_share_write(share.mem.data(), 0, xf);
		}
		xrt_frame_reference(&xf, nullptr);
	});

	uint64_t seq = 0;
	uint64_t last_seq = 0;
	uint32_t frames = 0;
	bool torn = false;
	bool ordered = true;
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
	while (std::chrono::steady_clock::now() < end) {
		struct xrt_frame *out = nullptr;
		if (!u_frame_share_read(share.mem.data(), 0, &seq, &out)) {
			continue;
		}
		frames++;
		ordered = ordered && seq > last_seq;
		last_seq = seq;
		// Every byte of a frame was written with the low byte of its timestamp.
		uint8_t expected = (uint8_t)out->timestamp;
		for (size_t i = 0; i < out->size; i++) {
			torn = torn || out->data[i] != expected;
		}
		xrt_frame_reference(&out, nullptr);
	}
	stop = true;
	writer.join();

	CHECK(frames > 0);
	CHECK(ordered);
	CHECK_FALSE(torn);
}

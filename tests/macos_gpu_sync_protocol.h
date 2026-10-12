// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <stdint.h>
struct Command
{
	uint64_t frame, value;
	uint32_t image, producer, mode, delay_ms;
};
struct Receipt
{
	uint64_t commit_ns;
	uint64_t gpu_status;
};

/*!
 * Expected BGRA pixel at (x, y) for a command's frame, producer and image.
 * Shared by every producer (Metal and the Windows D3D11/DXMT one) and the
 * consumer's checks, so they cannot drift apart.
 */
static inline void
gpu_sync_pattern(const struct Command *c, unsigned x, unsigned y, uint8_t p[4])
{
	const uint32_t words[4] = {(uint32_t)c->frame, (uint32_t)(c->frame >> 32), c->producer, c->image};
	p[0] = (uint8_t)(y % 251);
	p[1] = (uint8_t)((x + y) % 251);
	p[2] = (uint8_t)((words[(x / 4) % 4] >> ((x % 4) * 8)) & 255);
	p[3] = 255;
}

// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PSVR2_MODE4_PT_SOURCE_SIZE (512u * 508u)
#define PSVR2_MODE4_PT_SIZE (1024u * 1016u / 2u)

/* Convert one lower mode-4 L8 view to the existing 1024x1016 BC4 transport.
 * Every source pixel becomes a 2x2 block: no extra camera mode or stream is
 * requested. A BC4 block therefore contains only four source values. Local
 * min/max endpoints preserve contrast; the 508 active columns exclude USB
 * padding. Output is little-endian BC4 irrespective of host byte order.
 */
static inline bool
psvr2_mode4_encode_passthrough(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_size)
{
	if (src == NULL || dst == NULL || src_size < PSVR2_MODE4_PT_SOURCE_SIZE || dst_size < PSVR2_MODE4_PT_SIZE)
		return false;
	for (uint32_t y = 0; y < 508; y += 2) {
		for (uint32_t x = 0; x < 512; x += 2) {
			uint8_t values[4] = {0};
			if (x < 508) {
				values[0] = src[y * 512 + x];
				values[1] = src[y * 512 + x + 1];
				values[2] = src[(y + 1) * 512 + x];
				values[3] = src[(y + 1) * 512 + x + 1];
			}
			uint32_t lo = values[0], hi = values[0];
			for (uint32_t i = 1; i < 4; i++) {
				if (values[i] < lo)
					lo = values[i];
				if (values[i] > hi)
					hi = values[i];
			}
			uint32_t index[4];
			for (uint32_t i = 0; i < 4; i++) {
				uint32_t q = hi == lo ? 0 : (7 * (hi - values[i]) + (hi - lo) / 2) / (hi - lo);
				index[i] = q == 0 ? 0 : q == 7 ? 1 : q + 1;
			}
			uint64_t top = index[0] | (index[0] << 3) | (index[1] << 6) | (index[1] << 9);
			uint64_t bottom = index[2] | (index[2] << 3) | (index[3] << 6) | (index[3] << 9);
			uint64_t bits = top | (top << 12) | (bottom << 24) | (bottom << 36);
			*dst++ = (uint8_t)hi;
			*dst++ = (uint8_t)lo;
			for (uint32_t byte = 0; byte < 6; byte++)
				*dst++ = (uint8_t)(bits >> (8 * byte));
		}
	}
	return true;
}

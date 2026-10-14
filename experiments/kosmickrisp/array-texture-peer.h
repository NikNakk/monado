/* Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <IOSurface/IOSurfaceObjC.h>
@protocol KKArrayPeerProtocol
- (void)configureProducer:(MTLSharedEventHandle *)producer
                     done:(MTLSharedEventHandle *)done
                  texture:(MTLSharedTextureHandle *)texture
                   format:(uint64_t)format
                 registry:(uint64_t)registry
                    reply:(void (^)(BOOL, int, uint32_t))reply;
- (void)consumeValue:(uint64_t)value reply:(void (^)(BOOL, uint64_t, uint32_t))reply;
@end

#include <cmath>
static inline bool
checkArrayPixels(const uint8_t *pixels, uint64_t value, unsigned layers, bool bgra, bool srgb, bool marker = false)
{
	for (unsigned layer = 0; layer < layers; ++layer)
		for (unsigned y = 0; y < 64; ++y)
			for (unsigned x = 0; x < 64; ++x) {
				uint64_t frame = value + layer * 73;
				unsigned expected[4] = {(unsigned)(frame % 251), (unsigned)((x / 8 + frame * 3) % 251),
				                        (unsigned)((y / 8 + frame * 5) % 251), 255};
				if (srgb)
					for (unsigned c = 0; c < 3; ++c) {
						double linear = expected[c] / 255.0;
						double encoded = linear <= 0.0031308
						                     ? linear * 12.92
						                     : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
						expected[c] = (unsigned)std::lround(encoded * 255.0);
					}
				if (bgra) {
					unsigned tmp = expected[0];
					expected[0] = expected[2];
					expected[2] = tmp;
				}
				if (marker && x == 63 && y == 63) {
					expected[0] = 17 + layer;
					expected[1] = 33 + layer;
					expected[2] = 49 + layer;
					expected[3] = 255;
				}
				for (unsigned c = 0; c < 4; ++c) {
					int delta = (int)pixels[(layer * 4096 + y * 64 + x) * 4 + c] - (int)expected[c];
					if (std::abs(delta) >
					    (srgb && c < 3 && !(marker && x == 63 && y == 63) ? 1 : 0))
						return false;
				}
			}
	return true;
}

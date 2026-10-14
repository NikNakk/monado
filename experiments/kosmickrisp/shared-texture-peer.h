/* Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <IOSurface/IOSurfaceObjC.h>
@protocol KKTexturePeerProtocol
- (void)configureProducer:(MTLSharedEventHandle *)producer
                     done:(MTLSharedEventHandle *)done
                  surface:(IOSurface *)surface
                   format:(uint64_t)format
                 registry:(uint64_t)registry
                    reply:(void (^)(BOOL, int, uint32_t))reply;
- (void)consumeValue:(uint64_t)value reply:(void (^)(BOOL, uint64_t, uint32_t))reply;
@end

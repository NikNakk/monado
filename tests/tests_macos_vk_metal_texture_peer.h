// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
@protocol VulkanMetalTexturePeerProtocol
- (void)configureProducer:(MTLSharedEventHandle *)producer
                     done:(MTLSharedEventHandle *)done
                  texture:(MTLSharedTextureHandle *)texture
                 registry:(uint64_t)registry
                    reply:(void (^)(BOOL, int))reply;
- (void)queuedValue:(void (^)(uint64_t))reply;
- (void)consumeValue:(uint64_t)value reply:(void (^)(BOOL, uint64_t))reply;
@end

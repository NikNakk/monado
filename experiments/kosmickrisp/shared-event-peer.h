/* Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
@protocol KKEventPeerProtocol
- (void)configureProducer:(MTLSharedEventHandle *)producer
                     done:(MTLSharedEventHandle *)done
                 registry:(uint64_t)registry
                    reply:(void (^)(BOOL, int))reply;
- (void)consumeValue:(uint64_t)value reply:(void (^)(BOOL, uint64_t))reply;
@end

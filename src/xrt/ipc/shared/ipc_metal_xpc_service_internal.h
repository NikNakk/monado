// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "shared/ipc_metal_xpc.h"

#define IPC_METAL_XPC_MAX_TOKENS_PER_PID 64
#define IPC_METAL_XPC_MAX_TOKENS 1024
#define IPC_METAL_XPC_MAX_IMAGES_PER_PID 128
#define IPC_METAL_XPC_MAX_IMAGES 1024
#define IPC_METAL_XPC_TOKEN_LIFETIME_NS (60ULL * 1000000000ULL)

@interface IPCMetalXPCServiceObject : NSObject <IPCMetalXPCServiceProtocol> {
	NSLock *_lock;
	NSMutableDictionary *_handlesByToken;
	NSMutableDictionary *_countsByToken;
	NSMutableDictionary *_eventsByToken;
	NSMutableDictionary *_ownersByToken;
	NSMutableSet *_claimableTextureTokens;
	NSMutableDictionary *_createdByToken;
}

- (BOOL)storeImageObject:(id)handle
                   token:(uint64_t)token
                   index:(uint32_t)index
              imageCount:(uint32_t)imageCount
                ownerPID:(pid_t)ownerPID;
- (id)copyImageObjectForToken:(uint64_t)token
                        index:(uint32_t)index
                     ownerPID:(pid_t)ownerPID
                expectedClass:(Class)expectedClass;
- (BOOL)storeSharedEventHandle:(MTLSharedEventHandle *)handle token:(uint64_t)token ownerPID:(pid_t)ownerPID;
- (MTLSharedEventHandle *)copySharedEventHandleForToken:(uint64_t)token ownerPID:(pid_t)ownerPID;
- (void)discardToken:(uint64_t)token ownerPID:(pid_t)ownerPID;
- (NSUInteger)discardAllForPID:(pid_t)ownerPID;
- (void)expireTokensBefore:(uint64_t)cutoff;
- (BOOL)markTextureTokenClaimable:(uint64_t)token ownerPID:(pid_t)pid;
@end

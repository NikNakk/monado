// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS XPC transport for Metal shared handles.
 * @ingroup ipc_shared
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPC_METAL_XPC_SERVICE_NAME "org.freedesktop.monado.metal-ipc"
/*
 * Keep tokens representable in the legacy uint32_t ipc_arg_swapchain_from_native::sizes[]
 * field used by swapchain_import. The upper byte is a Metal-XPC tag and the lower
 * 24 bits are random. The mask also requires all bits above bit 31 to be zero.
 */
#define IPC_METAL_XPC_TOKEN_MAGIC UINT64_C(0x000000004d000000)
#define IPC_METAL_XPC_TOKEN_MASK UINT64_C(0xffffffffff000000)

/*
 * Cross-process claimable texture tokens are not constrained by the legacy
 * uint32_t image metadata transport. Reserve a separate namespace with 56 bits
 * of entropy so the token itself is a strong one-shot capability.
 */
#define IPC_METAL_XPC_EXTERNAL_TOKEN_MAGIC UINT64_C(0xc700000000000000)
#define IPC_METAL_XPC_EXTERNAL_TOKEN_MASK UINT64_C(0xff00000000000000)

#ifdef XRT_OS_OSX

/*!
 * Publish borrowed MTLTexture objects to the per-user Metal XPC broker.
 */
xrt_result_t
ipc_metal_xpc_publish_textures(void *const *metal_textures, uint32_t image_count, uint64_t *out_token);

/*!
 * Publish borrowed MTLTexture objects under a one-time token that may be
 * claimed by a different process. Intended for Chromium's XR-process to
 * GPU-process handoff; ordinary Monado texture tokens remain PID-scoped.
 */
xrt_result_t
ipc_metal_xpc_publish_claimable_textures(void *const *metal_textures,
                                         uint32_t image_count,
                                         uint64_t *out_token);

/*!
 * Recreate textures previously published under @p token.
 * Each returned pointer is a retained id<MTLTexture>.
 */
xrt_result_t
ipc_metal_xpc_take_textures(uint64_t token, uint32_t expected_count, void **out_metal_textures);

/*!
 * Recreate textures on a specific receiving-process MTLDevice. This is needed
 * by external clients such as Chromium/ANGLE, which require imported textures
 * to belong to the exact Metal device backing their EGLDisplay.
 */
xrt_result_t
ipc_metal_xpc_take_textures_on_device(uint64_t token,
                                      uint32_t expected_count,
                                      void *metal_device,
                                      void **out_metal_textures);

/*! Release textures returned by ipc_metal_xpc_take_textures(). */
void
ipc_metal_xpc_release_textures(void **metal_textures, uint32_t image_count);

/*!
 * Publish a borrowed MTLSharedEvent through the broker and return its token.
 */
xrt_result_t
ipc_metal_xpc_publish_shared_event(void *metal_shared_event, uint64_t *out_token);

/*!
 * Recreate an MTLSharedEvent published under @p token using the supplied
 * receiving-process MTLDevice. The returned pointer owns one Objective-C
 * reference.
 */
xrt_result_t
ipc_metal_xpc_take_shared_event(uint64_t token, void *metal_device, void **out_metal_shared_event);

/*! Release an event returned by ipc_metal_xpc_take_shared_event(). */
void
ipc_metal_xpc_release_shared_event(void *metal_shared_event);

/*
 * Thread-local marker used only while the Metal Stage-4 wrapper asks the IPC
 * compositor to create its service-side timeline semaphore. The source-local
 * IPC override consumes the returned broker token and stores the reconstructed
 * event here so the ordinary xrt_comp_create_semaphore call can still return
 * the normal IPC semaphore proxy.
 */
void
ipc_metal_xpc_begin_shared_event_request(void *metal_device);

bool
ipc_metal_xpc_shared_event_request_active(void);

xrt_result_t
ipc_metal_xpc_resolve_shared_event_request(uint64_t token);

bool
ipc_metal_xpc_end_shared_event_request(void **out_metal_shared_event);

/*! Best-effort removal of a token from the broker. */
void
ipc_metal_xpc_discard_token(uint64_t token);

/*!
 * Encode a broker token in xrt_image_native metadata for the existing
 * client-side import_swapchain call. The native handles remain invalid; a
 * macOS IPC shim recognizes the token before ordinary handle transport.
 */
void
ipc_metal_xpc_make_token_images(uint64_t token, uint32_t image_count, struct xrt_image_native *out_images);

/*! True if all images contain one valid Metal-broker token and invalid handles. */
bool
ipc_metal_xpc_get_token_from_images(const struct xrt_image_native *images,
                                    uint32_t image_count,
                                    uint64_t *out_token);

#endif // XRT_OS_OSX

#ifdef __cplusplus
}
#endif

#ifdef __OBJC__

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

@protocol IPCMetalXPCBrokerProtocol

- (void)publishTextureHandle:(MTLSharedTextureHandle *)handle
                       token:(uint64_t)token
                       index:(uint32_t)index
                  imageCount:(uint32_t)imageCount
                       reply:(void (^)(BOOL success))reply;

- (void)takeTextureHandleForToken:(uint64_t)token
                            index:(uint32_t)index
                            reply:(void (^)(MTLSharedTextureHandle *handle))reply;

- (void)markTextureTokenClaimable:(uint64_t)token
                            reply:(void (^)(BOOL success))reply;

- (void)publishSharedEventHandle:(MTLSharedEventHandle *)handle
                           token:(uint64_t)token
                           reply:(void (^)(BOOL success))reply;

- (void)takeSharedEventHandleForToken:(uint64_t)token
                                reply:(void (^)(MTLSharedEventHandle *handle))reply;

- (void)discardToken:(uint64_t)token reply:(void (^)(void))reply;

@end

NSXPCInterface *
ipc_metal_xpc_create_interface(void);

#endif // __OBJC__

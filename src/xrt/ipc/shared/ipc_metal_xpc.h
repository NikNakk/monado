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
#define IPC_METAL_XPC_TOKEN_MAGIC UINT64_C(0x4d58000000000000)
#define IPC_METAL_XPC_TOKEN_MASK UINT64_C(0xffff000000000000)

#ifdef XRT_OS_OSX

/*!
 * Publish borrowed MTLTexture objects to the per-user Metal XPC broker.
 *
 * The broker retains the corresponding MTLSharedTextureHandle objects until
 * the token is taken/discarded. The returned token is deliberately plain data
 * so it can travel through Monado's existing Unix-socket IPC protocol.
 */
xrt_result_t
ipc_metal_xpc_publish_textures(void *const *metal_textures, uint32_t image_count, uint64_t *out_token);

/*!
 * Recreate textures previously published under @p token.
 *
 * Each returned pointer is a retained id<MTLTexture> and must be released with
 * ipc_metal_xpc_release_textures(). On success the token is discarded from the
 * broker after all textures have been reconstructed.
 */
xrt_result_t
ipc_metal_xpc_take_textures(uint64_t token, uint32_t expected_count, void **out_metal_textures);

/*! Release textures returned by ipc_metal_xpc_take_textures(). */
void
ipc_metal_xpc_release_textures(void **metal_textures, uint32_t image_count);

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

- (void)discardToken:(uint64_t)token reply:(void (^)(void))reply;

@end

NSXPCInterface *
ipc_metal_xpc_create_interface(void);

#endif // __OBJC__

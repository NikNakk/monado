// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS Metal shared-handle serialization helpers for Monado IPC.
 * @ingroup ipc_shared
 */

#pragma once

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX

/*!
 * Archive a retained-or-borrowed MTLSharedTextureHandle into an NSSecureCoding
 * byte payload. Apple documents cross-process transfer of this object through
 * XPC; this byte form deliberately lets Monado probe whether the same encoded
 * handle remains valid over its existing socket IPC before committing to an
 * XPC side channel.
 *
 * The returned byte buffer is heap allocated and must be freed by the caller
 * with free().
 */
xrt_result_t
ipc_metal_archive_shared_texture_handle(void *shared_handle, uint8_t **out_bytes, uint32_t *out_size);

/*!
 * Decode an MTLSharedTextureHandle previously produced by
 * ipc_metal_archive_shared_texture_handle(). The returned Objective-C object is
 * retained for the caller and must be released with
 * ipc_metal_shared_handle_release().
 */
xrt_result_t
ipc_metal_unarchive_shared_texture_handle(const uint8_t *bytes, uint32_t size, void **out_shared_handle);

/*!
 * Archive an MTLSharedEventHandle using the same transport probe. This is
 * included alongside the texture path because a working cross-process encoding
 * can later carry the app-release timeline used by the compositor wait thread.
 */
xrt_result_t
ipc_metal_archive_shared_event_handle(void *shared_handle, uint8_t **out_bytes, uint32_t *out_size);

/*!
 * Decode an MTLSharedEventHandle. The returned object is retained for the
 * caller and must be released with ipc_metal_shared_handle_release().
 */
xrt_result_t
ipc_metal_unarchive_shared_event_handle(const uint8_t *bytes, uint32_t size, void **out_shared_handle);

/*!
 * Release a shared texture/event handle returned by one of the unarchive
 * helpers above.
 */
void
ipc_metal_shared_handle_release(void *shared_handle);

#endif // XRT_OS_OSX

#ifdef __cplusplus
}
#endif

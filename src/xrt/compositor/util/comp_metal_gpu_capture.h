// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experiment: Metal GPU capture around the first client-frame submissions.
 * @ingroup comp_util
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct vk_bundle;

/*!
 * With XRT_MACOS_GPU_CAPTURE_DIR set (and MTL_CAPTURE_ENABLED=1), start a
 * GPU trace document capture of the compositor's MTLDevice before the first
 * producer-waiting submission, and stop it before the fifth. Call before each
 * such submission. Does nothing once the capture has ended.
 */
void
comp_metal_gpu_capture_note_producer_submit(struct vk_bundle *vk);

/*!
 * Stop an active capture, for example after device loss. Safe to call at any time.
 */
void
comp_metal_gpu_capture_end(const char *reason);

#ifdef __cplusplus
}
#endif

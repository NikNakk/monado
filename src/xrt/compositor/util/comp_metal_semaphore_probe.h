// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS Metal shared-event semaphore diagnostic probe.
 * @ingroup comp_util
 */

#pragma once

#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX

/*!
 * Probe Vulkan timeline semaphore export to an MTLSharedEvent once per process.
 * Failure is diagnostic only and does not change compositor behaviour.
 */
void
comp_metal_semaphore_probe(struct vk_bundle *vk);

#endif

#ifdef __cplusplus
}
#endif

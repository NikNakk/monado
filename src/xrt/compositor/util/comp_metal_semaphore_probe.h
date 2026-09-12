// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS Metal shared-event semaphore provider and diagnostic probe.
 * @ingroup comp_util
 */

#pragma once

#include "util/comp_metal_semaphore_provider.h"
#include "vk/vk_helpers.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX

/*!
 * Probe Vulkan timeline semaphore export to an MTLSharedEvent and register the
 * compositor Vulkan bundle for the Metal client path.
 * Failure is diagnostic only and does not change compositor behaviour.
 */
void
comp_metal_semaphore_probe(struct vk_bundle *vk);

/*!
 * Clear the registered Vulkan bundle before its compositor is torn down.
 */
void
comp_metal_semaphore_provider_clear(struct vk_bundle *vk);

#endif

#ifdef __cplusplus
}
#endif

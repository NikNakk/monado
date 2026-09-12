// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Scoped allocator redirect for comp_swapchain.c on macOS.
 * @ingroup comp_util
 *
 * This header is force-included for comp_swapchain.c only. It leaves the
 * generic allocator unchanged everywhere else, while allowing a synchronous
 * in-process Metal request to provide VkImages directly.
 */

#pragma once

/* Pull in the real declarations before defining the call-site redirects. */
#include "vk/vk_image_allocator.h"
#include "util/comp_metal_swapchain_import.h"

#define vk_ic_allocate(VK, INFO, IMAGE_COUNT, OUT_VKIC)                                                               \
	comp_metal_swapchain_import_allocate_or_default((VK), (INFO), (IMAGE_COUNT), (OUT_VKIC))

#define vk_ic_get_handles(VK, VKIC, MAX_HANDLES, OUT_HANDLES)                                                         \
	comp_metal_swapchain_import_get_handles_or_default((VK), (VKIC), (MAX_HANDLES), (OUT_HANDLES))

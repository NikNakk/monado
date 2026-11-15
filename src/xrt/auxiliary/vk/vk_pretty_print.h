// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.
// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pretty printing various Vulkan things.
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Christoph Haag <christoph.haag@collabora.com>
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup aux_vk
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_vulkan_includes.h"

#include "util/u_pretty_print.h"


#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * String helper functions.
 *
 */

/*!
 * @name Enum string functions
 *
 * Returns a string of the enum value, or NULL if unknown. The versions without
 * the `_or_null` suffix return 'UNKNOWN <TYPE>' instead of NULL.
 *
 * @ingroup aux_vk
 * @{
 */

XRT_CHECK_RESULT const char *
vk_str_result_or_null(VkResult code);

XRT_CHECK_RESULT const char *
vk_str_object_type_or_null(VkObjectType type);

XRT_CHECK_RESULT const char *
vk_str_physical_device_type_or_null(VkPhysicalDeviceType device_type);

XRT_CHECK_RESULT const char *
vk_str_format_or_null(VkFormat code);

XRT_CHECK_RESULT const char *
vk_str_sharing_mode_or_null(VkSharingMode code);

XRT_CHECK_RESULT const char *
vk_str_present_mode_or_null(VkPresentModeKHR code);

XRT_CHECK_RESULT const char *
vk_str_color_space_or_null(VkColorSpaceKHR code);

XRT_CHECK_RESULT const char *
vk_str_display_power_state_or_null(VkDisplayPowerStateEXT code);

XRT_CHECK_RESULT const char *
vk_str_queue_global_priority_or_null(VkQueueGlobalPriorityEXT priority);

/*!
 * @}
 */

#define VK_STR_NO_NULL(NAME, TYPE, UNKNOWN_STR)                                                                        \
	static inline const char *NAME(TYPE enumerate)                                                                 \
	{                                                                                                              \
		const char *str = NAME##_or_null(enumerate);                                                           \
		return str != NULL ? str : UNKNOWN_STR;                                                                \
	}

VK_STR_NO_NULL(vk_str_result, VkResult, "UNKNOWN RESULT")
VK_STR_NO_NULL(vk_str_object_type, VkObjectType, "UNKNOWN OBJECT TYPE")
VK_STR_NO_NULL(vk_str_physical_device_type, VkPhysicalDeviceType, "UNKNOWN PHYSICAL DEVICE TYPE")
VK_STR_NO_NULL(vk_str_format, VkFormat, "UNKNOWN FORMAT")
VK_STR_NO_NULL(vk_str_sharing_mode, VkSharingMode, "UNKNOWN SHARING MODE")
VK_STR_NO_NULL(vk_str_present_mode, VkPresentModeKHR, "UNKNOWN PRESENT MODE")
VK_STR_NO_NULL(vk_str_color_space, VkColorSpaceKHR, "UNKNOWN COLOR SPACE")
VK_STR_NO_NULL(vk_str_display_power_state, VkDisplayPowerStateEXT, "UNKNOWN DISPLAY POWER STATE")
VK_STR_NO_NULL(vk_str_queue_global_priority, VkQueueGlobalPriorityEXT, "UNKNOWN QUEUE GLOBAL PRIORITY")


/*
 *
 * Flag bits string functions.
 *
 */

/*!
 * @name Flag bits string functions
 *
 * Returns a string of the flag bit, or NULL if unknown or if zero or multiple
 * bits are set. The versions without the `_or_null` suffix instead return
 * '<TYPE>: NO BITS SET', '<TYPE>: MULTIPLE BITS SET' or '<TYPE>: UNKNOWN BIT'.
 *
 * @ingroup aux_vk
 * @{
 */

XRT_CHECK_RESULT const char *
vk_str_format_feature_flag_bits_or_null(VkFormatFeatureFlagBits bits);

XRT_CHECK_RESULT const char *
vk_str_swapchain_create_flag_bits_or_null(VkSwapchainCreateFlagBitsKHR bits);

XRT_CHECK_RESULT const char *
vk_str_image_usage_flag_bits_or_null(VkImageUsageFlagBits bits);

XRT_CHECK_RESULT const char *
vk_str_composite_alpha_flag_bits_or_null(VkCompositeAlphaFlagBitsKHR bits);

XRT_CHECK_RESULT const char *
vk_str_surface_transform_flag_bits_or_null(VkSurfaceTransformFlagBitsKHR bits);

#ifdef VK_KHR_display
XRT_CHECK_RESULT const char *
vk_str_display_plane_alpha_flag_bits_or_null(VkDisplayPlaneAlphaFlagBitsKHR bits);
#endif // VK_KHR_display

/*!
 * @}
 */

#define VK_STR_FLAG_BITS_NO_NULL(NAME, TYPE, PREFIX)                                                                   \
	static inline const char *NAME(TYPE bits)                                                                      \
	{                                                                                                              \
		const char *str = NAME##_or_null(bits);                                                                \
		uint32_t value = (uint32_t)bits;                                                                       \
		if (str != NULL) {                                                                                     \
			return str;                                                                                    \
		} else if (value == 0) {                                                                               \
			return PREFIX ": NO BITS SET";                                                                 \
		} else if ((value & (value - 1)) != 0) {                                                               \
			return PREFIX ": MULTIPLE BITS SET";                                                           \
		} else {                                                                                               \
			return PREFIX ": UNKNOWN BIT";                                                                 \
		}                                                                                                      \
	}

VK_STR_FLAG_BITS_NO_NULL(vk_str_format_feature_flag_bits, VkFormatFeatureFlagBits, "FORMAT FEATURE")
VK_STR_FLAG_BITS_NO_NULL(vk_str_swapchain_create_flag_bits, VkSwapchainCreateFlagBitsKHR, "SWAPCHAIN CREATE")
VK_STR_FLAG_BITS_NO_NULL(vk_str_image_usage_flag_bits, VkImageUsageFlagBits, "IMAGE USAGE")
VK_STR_FLAG_BITS_NO_NULL(vk_str_composite_alpha_flag_bits, VkCompositeAlphaFlagBitsKHR, "COMPOSITE ALPHA")
VK_STR_FLAG_BITS_NO_NULL(vk_str_surface_transform_flag_bits, VkSurfaceTransformFlagBitsKHR, "SURFACE TRANSFORM")
#ifdef VK_KHR_display
VK_STR_FLAG_BITS_NO_NULL(vk_str_display_plane_alpha_flag_bits, VkDisplayPlaneAlphaFlagBitsKHR, "DISPLAY PLANE ALPHA")
#endif // VK_KHR_display

#undef VK_STR_FLAG_BITS_NO_NULL
#undef VK_STR_NO_NULL


/*
 *
 * Pretty print functions.
 *
 */

/*!
 * @name Pretty print functions
 *
 * Pretty prints the value to the delegate, unknown values are printed as hex
 * with a prefix, like `VK_FORMAT_0x12345678`. The flag bits printers expect a
 * single bit to be set, if no, multiple or an unknown bit is set they say so
 * and print the hex value, like `VK_IMAGE_USAGE: UNKNOWN BIT 0x00000100`.
 *
 * @ingroup aux_vk
 * @{
 */

void
vk_pp_result(u_pp_delegate_t dg, VkResult ret);

void
vk_pp_object_type(u_pp_delegate_t dg, VkObjectType type);

void
vk_pp_physical_device_type(u_pp_delegate_t dg, VkPhysicalDeviceType device_type);

void
vk_pp_format(u_pp_delegate_t dg, VkFormat format);

void
vk_pp_sharing_mode(u_pp_delegate_t dg, VkSharingMode mode);

void
vk_pp_present_mode(u_pp_delegate_t dg, VkPresentModeKHR mode);

void
vk_pp_color_space(u_pp_delegate_t dg, VkColorSpaceKHR color_space);

void
vk_pp_display_power_state(u_pp_delegate_t dg, VkDisplayPowerStateEXT state);

void
vk_pp_queue_global_priority(u_pp_delegate_t dg, VkQueueGlobalPriorityEXT priority);

void
vk_pp_format_feature_flag_bits(u_pp_delegate_t dg, VkFormatFeatureFlagBits bits);

void
vk_pp_swapchain_create_flag_bits(u_pp_delegate_t dg, VkSwapchainCreateFlagBitsKHR bits);

void
vk_pp_image_usage_flag_bits(u_pp_delegate_t dg, VkImageUsageFlagBits bits);

void
vk_pp_composite_alpha_flag_bits(u_pp_delegate_t dg, VkCompositeAlphaFlagBitsKHR bits);

void
vk_pp_surface_transform_flag_bits(u_pp_delegate_t dg, VkSurfaceTransformFlagBitsKHR bits);

#ifdef VK_KHR_display
void
vk_pp_display_plane_alpha_flag_bits(u_pp_delegate_t dg, VkDisplayPlaneAlphaFlagBitsKHR bits);
#endif // VK_KHR_display

/*!
 * @}
 */

#ifdef __cplusplus
}
#endif

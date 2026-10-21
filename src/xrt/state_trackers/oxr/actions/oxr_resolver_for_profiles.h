// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Resolve suggested profiles for a device.
 * @ingroup oxr_main
 */

#pragma once

#include "xrt/xrt_defines.h"

#include "oxr_extension_support.h"
#include "oxr_forward_declarations.h"
#include "oxr_suggested_profile_array.h"


#ifdef __cplusplus
extern "C" {
#endif


/*!
 * Short-lived result of choosing a suggested profile for one subaction slot
 * (for example `/user/hand/left`), plus any rebinding or device fallback needed
 * to apply it.
 */
struct oxr_profile_resolver_info
{
	/*!
	 * The suggested profile to bind for, if no matching profile is found,
	 * no rebinding or fallback is found this and all other fields will be
	 * NULL.
	 */
	const struct oxr_suggested_profile *profile;

	/*!
	 * Runtime binding profile for @ref profile.
	 */
	const struct oxr_binding_profile *binding_profile;

	/*!
	 * If the device is not the same as the profile, this will be set to the
	 * rebinding profile that applies to the device. If no rebinding profile is
	 * found, this will be NULL.
	 */
	const struct oxr_rebinding_profile *rebinding_profile;

	/*!
	 * Runtime binding profile targeted by @ref rebinding_profile.
	 */
	const struct oxr_binding_profile *rebinding_target_profile;

	/*!
	 * If the device is not the same as the profile, and no rebinding
	 * profile the code will look for a fallback on the device and set this
	 * to the fallback binding profile.
	 */
	const struct xrt_binding_profile *xbp;
};

/*!
 * Resolve the binding information for a given device.
 *
 * @param log Logger, must not be NULL.
 * @param slog Sink logger, must not be NULL.
 * @param registry Binding registry, must not be NULL.
 * @param suggested_profiles Suggested profiles, must not be NULL.
 * @param xdev Device, must not be NULL.
 * @param override_device_name Profile name to prefer before @p xdev->name, can be XRT_DEVICE_INVALID.
 * @param info Binding information, must not be NULL.
 */
XRT_NONNULL_ALL void
oxr_resolver_populate_profile(struct oxr_logger *log,
                              struct oxr_sink_logger *slog,
                              const struct oxr_binding_registry *registry,
                              const struct oxr_suggested_profile_array *suggested_profiles,
                              struct xrt_device *xdev,
                              enum xrt_device_name override_device_name,
                              struct oxr_profile_resolver_info *info);


#ifdef __cplusplus
}
#endif

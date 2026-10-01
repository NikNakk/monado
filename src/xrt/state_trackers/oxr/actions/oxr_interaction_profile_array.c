// Copyright 2018-2020,2023 Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds binding related functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup oxr_main
 */

#include "util/u_misc.h"

#include "oxr_interaction_profile_array.h"
#include "oxr_generated_bindings.h"
#include "oxr_binding.h"

#include "../oxr_objects.h"

#include <assert.h>


void
oxr_interaction_profile_array_clear(struct oxr_interaction_profile_array *array)
{
	for (size_t i = 0; i < array->count; i++) {
		struct oxr_interaction_profile *p = array->profiles[i];
		oxr_interaction_profile_destroy(p);
	}

	free(array->profiles);
	array->profiles = NULL;
	array->count = 0;
}

void
oxr_interaction_profile_array_add(struct oxr_interaction_profile_array *array, struct oxr_interaction_profile *profile)
{
	U_ARRAY_REALLOC_OR_FREE(array->profiles, struct oxr_interaction_profile *, (array->count + 1));
	array->profiles[array->count++] = profile;
}

void
oxr_interaction_profile_array_clone(const struct oxr_interaction_profile_array *src,
                                    struct oxr_interaction_profile_array *dst)
{
	oxr_interaction_profile_array_clear(dst);

	if (src->profiles == NULL || src->count == 0) {
		return;
	}

	dst->count = src->count;
	dst->profiles = U_TYPED_ARRAY_CALLOC(struct oxr_interaction_profile *, src->count);

	for (size_t i = 0; i < src->count; i++) {
		dst->profiles[i] = oxr_interaction_profile_clone(src->profiles[i]);
	}
}

bool
oxr_interaction_profile_array_find_by_path(const struct oxr_interaction_profile_array *array,
                                           XrPath path,
                                           struct oxr_interaction_profile **out_p)
{
	for (size_t x = 0; x < array->count; x++) {
		struct oxr_interaction_profile *p = array->profiles[x];
		if (p->path != path) {
			continue;
		}

		*out_p = p;
		return true;
	}

	*out_p = NULL;

	return false;
}

bool
oxr_interaction_profile_array_find_by_device_name(const struct oxr_interaction_profile_array *array,
                                                  const struct oxr_instance_path_cache *cache,
                                                  enum xrt_device_name name,
                                                  struct oxr_interaction_profile **out_p)
{
	if (name == XRT_DEVICE_INVALID) {
		*out_p = NULL;
		return false;
	}

	/*
	 * Map xrt_device_name to an interaction profile XrPath.
	 *
	 * There might be multiple OpenXR interaction profiles that maps to a
	 * a single @ref xrt_device_name, so we can't just grab the first one
	 * that we find and assume that wasn't bound then there isn't an OpenXR
	 * interaction profile bound for that device name. So we will need to
	 * keep looping until we find an OpenXR interaction profile, or we run
	 * out of interaction profiles that the app has suggested.
	 *
	 * For XRT_DEVICE_HAND_INTERACTION both the OpenXR hand-interaction
	 * profiles maps to it, but the app might only provide binding for one.
	 *
	 * Set *out_p to an oxr_interaction_profile if bindings for that
	 * interaction profile XrPath have been suggested.
	 */
	for (size_t i = 0; i < ARRAY_SIZE(oxr_profile_templates); i++) {
		// If it's for a different device name, skip.
		if (name != oxr_profile_templates[i].name) {
			continue;
		}

		bool found = oxr_interaction_profile_array_find_by_path( //
		    array,                                               //
		    cache->template_paths[i],                            //
		    out_p);                                              //

		/*
		 * Keep looping even if the current matching OpenXR
		 * interaction profile wasn't suggested by the app.
		 * See comment above.
		 */
		if (found) {
			return true;
		}
	}

	*out_p = NULL;
	return false;
}


static bool
xdev_supports_khr_generic_controller(const struct xrt_device *xdev)
{
	switch (xdev->name) {
	case XRT_DEVICE_GENERIC_CONTROLLER:
	case XRT_DEVICE_TOUCH_CONTROLLER:
	case XRT_DEVICE_INDEX_CONTROLLER:
	case XRT_DEVICE_TOUCH_PRO_CONTROLLER:
	case XRT_DEVICE_TOUCH_PLUS_CONTROLLER:
	case XRT_DEVICE_TOUCH_CONTROLLER_RIFT_CV1:
	case XRT_DEVICE_TOUCH_CONTROLLER_QUEST_1_RIFT_S:
	case XRT_DEVICE_TOUCH_CONTROLLER_QUEST_2: return true;
	default: break;
	}

	/*
	 * XR_KHR_generic_controller requires a generic fallback in conditions
	 * where the runtime would otherwise select the legacy Touch or Index
	 * interaction profiles. Devices can advertise those profiles through
	 * xrt_device::binding_profiles even when their native xrt_device_name is
	 * different.
	 */
	for (size_t i = 0; i < xdev->binding_profile_count; i++) {
		switch (xdev->binding_profiles[i].name) {
		case XRT_DEVICE_GENERIC_CONTROLLER:
		case XRT_DEVICE_TOUCH_CONTROLLER:
		case XRT_DEVICE_INDEX_CONTROLLER: return true;
		default: break;
		}
	}

	return false;
}

static bool
find_khr_generic_controller(const struct oxr_interaction_profile_array *array,
                            const struct oxr_instance_path_cache *cache,
                            struct oxr_interaction_profile **out_p)
{
	return oxr_interaction_profile_array_find_by_device_name(
	    array, cache, XRT_DEVICE_GENERIC_CONTROLLER, out_p);
}

static bool
find_suggested_touch_or_index_fallback(const struct oxr_interaction_profile_array *array,
                                       const struct oxr_instance_path_cache *cache,
                                       const struct xrt_device *xdev,
                                       struct oxr_interaction_profile **out_p)
{
	for (size_t i = 0; i < xdev->binding_profile_count; i++) {
		enum xrt_device_name name = xdev->binding_profiles[i].name;
		if (name != XRT_DEVICE_TOUCH_CONTROLLER && name != XRT_DEVICE_INDEX_CONTROLLER) {
			continue;
		}

		if (oxr_interaction_profile_array_find_by_device_name(array, cache, name, out_p)) {
			return true;
		}
	}

	*out_p = NULL;
	return false;
}

bool
oxr_interaction_profile_array_find_by_device(const struct oxr_interaction_profile_array *array,
                                             const struct oxr_instance_path_cache *cache,
                                             struct xrt_device *xdev,
                                             struct oxr_interaction_profile **out_p)
{
	bool found = false;

	if (xdev == NULL) {
		*out_p = NULL;
		return false;
	}

	// Have bindings for this device's interaction profile been suggested?
	found = oxr_interaction_profile_array_find_by_device_name( //
	    array,                                                 //
	    cache,                                                 //
	    xdev->name,                                            //
	    out_p);                                                //
	if (found) {
		return true;
	}

	bool supports_generic = xdev_supports_khr_generic_controller(xdev);

	/*
	 * A suggested Touch or Index profile remains more specific than the
	 * generic fallback. Check those first even if a driver's historical
	 * fallback ordering placed simple_controller ahead of them.
	 */
	if (supports_generic && find_suggested_touch_or_index_fallback(array, cache, xdev, out_p)) {
		return true;
	}

	bool tried_generic = false;

	// Check if bindings for any of this device's alternative interaction profiles have been suggested.
	for (size_t i = 0; i < xdev->binding_profile_count; i++) {
		struct xrt_binding_profile *xbp = &xdev->binding_profiles[i];

		/*
		 * The generic profile is a richer hardware-neutral fallback than
		 * simple_controller. Prefer it before falling all the way back to
		 * the simple profile when this device is known to map to
		 * generic_controller semantics.
		 */
		if (supports_generic && !tried_generic && xbp->name == XRT_DEVICE_SIMPLE_CONTROLLER) {
			tried_generic = true;
			if (find_khr_generic_controller(array, cache, out_p)) {
				return true;
			}
		}

		found = oxr_interaction_profile_array_find_by_device_name( //
		    array,                                                 //
		    cache,                                                 //
		    xbp->name,                                             //
		    out_p);                                                //
		if (found) {
			return true;
		}
	}

	/*
	 * Index/Touch devices do not need an explicit generic entry in every
	 * driver. The common OpenXR layer provides the standards-required
	 * fallback after hardware-specific alternatives have been considered.
	 */
	if (supports_generic && !tried_generic && find_khr_generic_controller(array, cache, out_p)) {
		return true;
	}

	*out_p = NULL;
	return false;
}

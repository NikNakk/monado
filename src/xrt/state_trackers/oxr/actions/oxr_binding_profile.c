// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Helpers for runtime interaction profile binding data.
 * @ingroup oxr_main
 */

#include "util/u_hashmap.h"
#include "oxr_binding_profile.h"


bool
oxr_binding_profile_find_binding_point(const struct oxr_binding_profile *profile,
                                       XrPath path,
                                       const struct oxr_binding_point **out_binding_point)
{
	void *ptr = NULL;
	if (profile == NULL || u_hashmap_int_find(profile->binding_points_by_path, path, &ptr) != 0) {
		*out_binding_point = NULL;
		return false;
	}

	*out_binding_point = (struct oxr_binding_point *)ptr;
	return true;
}

bool
oxr_binding_profile_binds_to_subaction(const struct oxr_binding_profile *profile, XrPath subaction_path)
{
	/*
	 * Profiles have no explicit list of the subaction paths they bind to yet,
	 * for now the subaction paths of the binding points are a good proxy.
	 */
	for (size_t i = 0; i < profile->binding_point_count; i++) {
		if (profile->binding_points[i].subaction->path == subaction_path) {
			return true;
		}
	}

	return false;
}

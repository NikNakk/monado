// Copyright 2026, NVIDIA CORPORATION.
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Resolve suggested profiles for a device.
 * @ingroup oxr_main
 */

#include "util/u_misc.h"
#include "util/u_pretty_print.h"

#include "oxr_binding_registry.h"
#include "oxr_binding_profile.h"
#include "oxr_rebinding_profile.h"
#include "oxr_io_cache.h"
#include "oxr_resolver_for_profiles.h"
#include "oxr_suggested_profile.h"

#include "../oxr_logger.h"
#include "../oxr_objects.h"


/*
 *
 * Helpers
 *
 */

static void
log_resolver_info(struct oxr_sink_logger *slog, const struct oxr_profile_resolver_info *info)
{
	if (info->profile == NULL) {
		return;
	}

	oxr_slog(slog, "\n\t\t\tSuggested profile: %s (%s)", info->profile->templ->path,
	         u_str_xrt_device_name(info->profile->templ->name));

	if (info->rebinding_profile != NULL) {
		oxr_slog(slog, "\n\t\t\tRe-binding profile: %s (%s)",
		         info->rebinding_profile->templ->target_profile_path,
		         u_str_xrt_device_name(info->rebinding_target_profile->device_name));
	}

	if (info->compatibility_profile != NULL) {
		oxr_slog(slog, "\n\t\t\tCache compatibility profile: %s",
		         u_str_xrt_device_name(info->compatibility_profile->name));
	}
}

static void
fill_binding_profiles(struct oxr_sink_logger *slog,
                      const struct oxr_binding_registry *registry,
                      const struct oxr_suggested_profile *profile,
                      const struct oxr_rebinding_profile *rebinding_profile,
                      const struct xrt_io_cache_binding_profile *compatibility_profile,
                      struct oxr_profile_resolver_info *info)
{
	assert(profile != NULL);

	info->profile = profile;
	info->rebinding_profile = rebinding_profile;
	info->compatibility_profile = compatibility_profile;

	if (!oxr_binding_registry_find_profile(registry, info->profile->path, &info->binding_profile)) {
		oxr_slog(slog, "\n\t\tNo binding profile for suggested profile.");
		U_ZERO(info);
		return;
	}

	if (info->rebinding_profile == NULL) {
		return;
	}

	if (!oxr_binding_registry_find_profile(registry, info->rebinding_profile->target_profile_path,
	                                       &info->rebinding_target_profile)) {
		oxr_slog(slog, "\n\t\tNo target binding profile for rebinding.");
		U_ZERO(info);
		return;
	}
}

static bool
try_resolve_with_device_name(struct oxr_sink_logger *slog,
                             const struct oxr_binding_registry *registry,
                             const struct oxr_suggested_profile_array *suggested_profiles,
                             const struct oxr_io_cache *cache,
                             const struct xrt_io_cache_device *dev,
                             enum xrt_device_name device_name,
                             struct oxr_profile_resolver_info *info)
{
	struct oxr_suggested_profile *profile = NULL;
	assert(dev != NULL);
	assert(dev->name != device_name);
	assert(device_name != XRT_DEVICE_INVALID);

	// First can we try to find the profile by the requested device name?
	bool found = oxr_suggested_profile_array_find_by_device_name(suggested_profiles, device_name, &profile);
	if (!found) {
		/*
		 * The app didn't suggest a profile for this device name, so there
		 * is no way we can honor the request to bind with that profile.
		 */
		return false;
	}

	// Look for a rebinding profile for the requested device name.
	const struct oxr_rebinding_profile *rebinding_profile = NULL;
	found = oxr_binding_registry_find_rebinding_from_source_for_device( //
	    registry,                                                       //
	    profile->path,                                                  //
	    dev->name,                                                      //
	    &rebinding_profile);                                            //
	if (found) {
		fill_binding_profiles( //
		    slog,              //
		    registry,          //
		    profile,           // oxr_suggested_profile
		    rebinding_profile, // oxr_rebinding_profile
		    NULL,              // xrt_binding_profile
		    info);             //
		log_resolver_info(slog, info);
		return true;
	}

	// Look for a binding profile for the requested device name.
	for (size_t i = 0; i < dev->binding_profile_count; i++) {
		const struct xrt_io_cache_binding_profile *xbp =
		    &cache->info.binding_profiles[dev->first_binding_profile + i];
		if (xbp->name != device_name) {
			continue;
		}

		fill_binding_profiles( //
		    slog,              //
		    registry,          //
		    profile,           // oxr_suggested_profile
		    NULL,              // oxr_rebinding_profile
		    xbp,               // xrt_binding_profile
		    info);             //
		log_resolver_info(slog, info);
		return true;
	}

	return false;
}

static bool
try_resolve_with_xdev(struct oxr_sink_logger *slog,
                      const struct oxr_binding_registry *registry,
                      const struct oxr_suggested_profile_array *suggested_profiles,
                      const struct oxr_io_cache *cache,
                      const struct xrt_io_cache_device *dev,
                      struct oxr_profile_resolver_info *info)
{
	struct oxr_suggested_profile *profile = NULL;

	// First can we try to find the profile by the requested dev->name?
	bool found = oxr_suggested_profile_array_find_by_device_name(suggested_profiles, dev->name, &profile);

	// If the profile is found and the device name matches, use the profile.
	if (found) {
		fill_binding_profiles( //
		    slog,              //
		    registry,          //
		    profile,           // oxr_suggested_profile
		    NULL,              // oxr_rebinding_profile
		    NULL,              // xrt_binding_profile
		    info);             //
		log_resolver_info(slog, info);
		return true;
	}

	/* Keep a suggested Touch/Index compatibility profile ahead of generic and
	 * simple fallbacks, even when the driver's historical order differs. */
	for (size_t i = 0; i < dev->binding_profile_count; i++) {
		enum xrt_device_name name = cache->info.binding_profiles[dev->first_binding_profile + i].name;
		if (name == dev->name || (name != XRT_DEVICE_TOUCH_CONTROLLER && name != XRT_DEVICE_INDEX_CONTROLLER)) {
			continue;
		}
		if (try_resolve_with_device_name(slog, registry, suggested_profiles, cache, dev, name, info)) {
			return true;
		}
	}

	// Can we find a rebinding profile for the requested device name?
	const struct oxr_rebinding_profile *rebinding_profile = NULL;
	found = oxr_suggested_profile_array_find_rebinding_for_device_name( //
	    suggested_profiles,                                             //
	    registry,                                                       //
	    dev->name,                                                      //
	    &profile,                                                       //
	    &rebinding_profile);                                            //
	if (found) {
		fill_binding_profiles( //
		    slog,              //
		    registry,          //
		    profile,           // oxr_suggested_profile
		    rebinding_profile, // oxr_rebinding_profile
		    NULL,              // xrt_binding_profile
		    info);             //
		log_resolver_info(slog, info);
		return true;
	}

	for (size_t i = 0; i < dev->binding_profile_count; i++) {
		const struct xrt_io_cache_binding_profile *xbp =
		    &cache->info.binding_profiles[dev->first_binding_profile + i];
		bool found = oxr_suggested_profile_array_find_by_device_name(suggested_profiles, xbp->name, &profile);
		if (!found) {
			continue;
		}

		fill_binding_profiles( //
		    slog,              //
		    registry,          //
		    profile,           // oxr_suggested_profile
		    NULL,              // oxr_rebinding_profile
		    xbp,               // xrt_binding_profile
		    info);             //
		log_resolver_info(slog, info);
		return true;
	}

	return false;
}


/*
 *
 * Exported functions
 *
 */

void
oxr_resolver_populate_profile(struct oxr_logger *log,
                              struct oxr_sink_logger *slog,
                              const struct oxr_binding_registry *registry,
                              const struct oxr_suggested_profile_array *suggested_profiles,
                              const struct oxr_io_cache *cache,
                              const struct xrt_io_cache_device *dev,
                              enum xrt_device_name override_device_name,
                              struct oxr_profile_resolver_info *info)
{
	assert(log != NULL);
	assert(slog != NULL);
	assert(registry != NULL);
	assert(suggested_profiles != NULL);
	assert(dev != NULL);
	assert(info != NULL);

	U_ZERO(info);

	// We are overriding the device name with a different one.
	if (override_device_name != XRT_DEVICE_INVALID && override_device_name != dev->name) {
		oxr_slog(slog, "\n\t\t\toverride: %s", u_str_xrt_device_name(override_device_name));
		if (try_resolve_with_device_name(slog, registry, suggested_profiles, cache, dev, override_device_name,
		                                 info)) {
			return;
		}
		oxr_slog(slog, "\n\t\t\t\tOverride failed");
	}

	if (try_resolve_with_xdev(slog, registry, suggested_profiles, cache, dev, info)) {
		return;
	}

	oxr_slog(slog, "\n\t\tNo rebinding or fallback for suggested profile.");
}

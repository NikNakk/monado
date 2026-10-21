// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Resolve suggested bindings into action cache IO.
 * @ingroup oxr_main
 */

#include "util/u_debug.h"
#include "util/u_misc.h"
#include "oxr_resolver_for_bindings.h"
#include "oxr_binding_profile.h"
#include "oxr_binding_registry.h"
#include "oxr_dpad_state.h"
#include "oxr_input_transform.h"
#include "oxr_priority_group_cache.h"
#include "oxr_rebinding_profile.h"
#include "oxr_session_action_context.h"
#include "oxr_suggested_profile.h"

#include "../oxr_conversions.h"
#include "../oxr_logger.h"
#include "../oxr_objects.h"

#include <string.h>


/*
 *
 * Structs
 *
 */

struct input_storage
{
	struct oxr_action_input inputs[OXR_MAX_BINDINGS_PER_ACTION];
	uint32_t count;
};

struct output_storage
{
	struct oxr_action_output outputs[OXR_MAX_BINDINGS_PER_ACTION];
	uint32_t count;
};


/*
 *
 * Input & output helpers.
 *
 */

static bool
find_xdev_input_name_from_pairs(const struct xrt_device *xdev,
                                const struct xrt_binding_profile *xbp,
                                enum xrt_input_name from_name,
                                enum xrt_input_name *out_name)
{
	// Dpad activation is optional. An unset activate input means the dpad is always active.
	if (from_name == XRT_INPUT_NAME_INVALID) {
		*out_name = XRT_INPUT_NAME_INVALID;
		return true;
	}

	for (size_t i = 0; i < xbp->input_count; i++) {
		if (from_name != xbp->inputs[i].from) {
			continue;
		}

		enum xrt_input_name xdev_name = xbp->inputs[i].device;
		for (uint32_t k = 0; k < xdev->input_count; k++) {
			if (xdev->inputs[k].name == xdev_name) {
				*out_name = xdev_name;
				return true;
			}
		}
	}

	return false;
}

static bool
is_action_type_output(XrActionType action_type)
{
	switch (action_type) {
	case XR_ACTION_TYPE_BOOLEAN_INPUT:
	case XR_ACTION_TYPE_FLOAT_INPUT:
	case XR_ACTION_TYPE_VECTOR2F_INPUT:
	case XR_ACTION_TYPE_POSE_INPUT: return false;
	case XR_ACTION_TYPE_VIBRATION_OUTPUT: return true;
	// We don't want a default case so we can get a compiler warning if we add a new action type.
	case XR_ACTION_TYPE_MAX_ENUM: assert(!"Unreachable"); break;
	}

	// This should never happen.
	assert(!"Unreachable");
	return false;
}

static bool
binding_point_matches_action_type(const struct oxr_binding_point *point, XrActionType action_type)
{
	switch (point->kind) {
	case OXR_BINDING_POINT_KIND_INPUT:
		return point->input.name != XRT_INPUT_NAME_INVALID && !is_action_type_output(action_type);
	case OXR_BINDING_POINT_KIND_OUTPUT:
		return point->output.name != XRT_OUTPUT_NAME_INVALID && is_action_type_output(action_type);
	case OXR_BINDING_POINT_KIND_DPAD: return !is_action_type_output(action_type);
	case OXR_BINDING_POINT_KIND_UPGRADE: return false;
	}

	return false;
}

static bool
resolve_upgrade_binding_point(struct oxr_sink_logger *slog,
                              const struct oxr_binding_profile *profile,
                              const struct oxr_binding_point *upgrade_point,
                              XrActionType action_type,
                              const struct oxr_binding_point **out_point)
{
	XrPath target_path = XR_NULL_PATH;
	switch (action_type) {
	case XR_ACTION_TYPE_BOOLEAN_INPUT: target_path = upgrade_point->upgrade.boolean_path; break;
	case XR_ACTION_TYPE_FLOAT_INPUT: target_path = upgrade_point->upgrade.float_path; break;
	case XR_ACTION_TYPE_VECTOR2F_INPUT: target_path = upgrade_point->upgrade.vec2_path; break;
	case XR_ACTION_TYPE_POSE_INPUT: target_path = upgrade_point->upgrade.pose_path; break;
	case XR_ACTION_TYPE_VIBRATION_OUTPUT: target_path = upgrade_point->upgrade.haptic_path; break;
	case XR_ACTION_TYPE_MAX_ENUM: assert(!"Unreachable"); break;
	}

	if (target_path == XR_NULL_PATH) {
		oxr_slog(slog, "\t\t\tRejected! (NO UPGRADE TARGET PATH FOR %s)\n",
		         xr_action_type_to_short_str(action_type));
		*out_point = NULL;
		return false;
	}

	bool bret = oxr_binding_profile_find_binding_point(profile, target_path, out_point);
	if (!bret) {
		// If there is an upgrade path there really should be a binding point.
		oxr_slog(slog, "\t\t\tWARNING: No binding point found for %s\n", upgrade_point->templ->path);
		*out_point = NULL;
		assert(!"Unreachable");
		return false;
	}

	oxr_slog(slog, "\t\t\tUpgraded to: %s (%s)\n", (*out_point)->templ->path,
	         xr_action_type_to_short_str(action_type));

	return true;
}

static bool
resolve_binding_point(struct oxr_sink_logger *slog,
                      const struct oxr_profile_resolver_info *info,
                      const struct oxr_suggested_binding *suggested_binding,
                      XrActionType action_type,
                      const struct oxr_binding_point **out_point)
{
	*out_point = NULL;

	const struct oxr_binding_profile *binding_profile = info->binding_profile;

	assert(info->profile != NULL); // Not used but must hold true.
	assert(binding_profile != NULL);

	XrPath target_path = suggested_binding->path;

	// If we have a rebinding profile, update the target path to the rebinding target path.
	if (info->rebinding_profile != NULL) {
		assert(info->rebinding_target_profile != NULL);

		// Update the target path to the rebinding target path.
		if (!oxr_rebinding_profile_find_target(info->rebinding_profile, target_path, &target_path)) {
			oxr_slog(slog, "\t\t\tRejected! (NO REBINDING TARGET PATH)\n");
			return false;
		}
		assert(target_path != XR_NULL_PATH);
		oxr_slog(slog, "\t\t\tRebinding target path\n");

		// Rebind the binding profile to the rebinding target profile.
		binding_profile = info->rebinding_target_profile;
		assert(binding_profile != NULL);
	}

	const struct oxr_binding_point *binding_point = NULL;
	if (!oxr_binding_profile_find_binding_point(binding_profile, target_path, &binding_point)) {
		oxr_slog(slog, "\t\t\tRejected! (NO BINDING POINT)\n");
		return false;
	}

	// If the binding point is an upgrade, resolve it until we get to a non-upgrade binding point.
	while (binding_point->kind == OXR_BINDING_POINT_KIND_UPGRADE) {
		bool bret = resolve_upgrade_binding_point( //
		    slog,                                  //
		    binding_profile,                       //
		    binding_point,                         //
		    action_type,                           //
		    &binding_point);                       //
		if (!bret) {
			// Already logged.
			return false;
		}
	}

	if (!binding_point_matches_action_type(binding_point, action_type)) {
		oxr_slog(slog, "\t\t\tRejected! (NO MATCHING ACTION TYPE)\n");
		return false;
	}

	*out_point = binding_point;
	oxr_slog(slog, "\t\t\tBound binding point: %s\n", binding_point->templ->path);
	return true;
}

static bool
get_dpad_region_from_binding_point(const struct oxr_binding_point *binding_point, enum oxr_dpad_region *out_dpad_region)
{
	if (binding_point == NULL || binding_point->kind != OXR_BINDING_POINT_KIND_DPAD) {
		return false;
	}

	switch (binding_point->dpad.binding_point) {
	case OXR_DPAD_BINDING_POINT_UP: *out_dpad_region = OXR_DPAD_REGION_UP; return true;
	case OXR_DPAD_BINDING_POINT_DOWN: *out_dpad_region = OXR_DPAD_REGION_DOWN; return true;
	case OXR_DPAD_BINDING_POINT_LEFT: *out_dpad_region = OXR_DPAD_REGION_LEFT; return true;
	case OXR_DPAD_BINDING_POINT_RIGHT: *out_dpad_region = OXR_DPAD_REGION_RIGHT; return true;
	case OXR_DPAD_BINDING_POINT_CENTER: *out_dpad_region = OXR_DPAD_REGION_CENTER; return true;
	case OXR_DPAD_BINDING_POINT_NONE: return false;
	}

	assert(!"Unreachable");
	return false;
}

static bool
find_matching_dpad(const struct oxr_dpad_entry *dpad_entry,
                   XrPath dpad_emulation_path,
                   const struct oxr_dpad_binding_modification **out_dpad_binding)
{
	*out_dpad_binding = NULL;

	if (dpad_entry == NULL) {
		return false;
	}

	for (uint32_t i = 0; i < dpad_entry->dpad_count; i++) {
		if (dpad_entry->dpads[i].binding == dpad_emulation_path) {
			*out_dpad_binding = &dpad_entry->dpads[i];
			return true;
		}
	}

	return false;
}

/* Prefer the calibrated palm pose. Preserve the fork's approximate grip
 * fallback only for the generic profile's grip-surface input when absent. */
static bool
find_generic_grip_surface_fallback(struct xrt_device *xdev,
                                   const struct oxr_suggested_profile *profile,
                                   const struct oxr_binding_point *point,
                                   struct xrt_input **out_input)
{
	if (profile->templ->name != XRT_DEVICE_GENERIC_CONTROLLER ||
	    strstr(point->templ->path, "/input/grip_surface/pose") == NULL) {
		return false;
	}

	for (size_t i = 0; i < point->profile->binding_point_count; i++) {
		const struct oxr_binding_point *grip = &point->profile->binding_points[i];
		if (grip->subaction == point->subaction && grip->kind == OXR_BINDING_POINT_KIND_INPUT &&
		    strstr(grip->templ->path, "/input/grip/pose") != NULL) {
			return oxr_xdev_find_input(xdev, grip->input.name, out_input);
		}
	}
	return false;
}

static void
append_resolved_input(struct oxr_logger *log,
                      struct oxr_sink_logger *slog,
                      const struct oxr_suggested_profile *profile,
                      const struct oxr_suggested_binding *suggested_binding,
                      const char *action_name,
                      struct xrt_device *xdev,
                      const struct xrt_binding_profile *xbp,
                      const struct oxr_binding_point *binding_point,
                      struct oxr_action_attachment *act_attached,
                      struct input_storage *input_storage)
{
	enum xrt_input_name source_input = XRT_INPUT_NAME_INVALID;
	enum xrt_input_name source_dpad_activate = XRT_INPUT_NAME_INVALID;

	switch (binding_point->kind) {
	case OXR_BINDING_POINT_KIND_INPUT: source_input = binding_point->input.name; break;
	case OXR_BINDING_POINT_KIND_DPAD:
		source_input = binding_point->dpad.emulation->input;
		source_dpad_activate = binding_point->dpad.emulation->activate;
		break;
	case OXR_BINDING_POINT_KIND_OUTPUT:
	default:
		// This should never happen.
		assert(!"Unreachable");
		return;
	}

	enum xrt_input_name target_name = source_input;
	enum xrt_input_name target_dpad_activate = source_dpad_activate;

	if (xbp != NULL) {
		bool t1 = find_xdev_input_name_from_pairs(xdev, xbp, source_input, &target_name);
		/*
		 * A missing dpad activate input means the dpad is always active.
		 * find_xdev_input_name_from_pairs treats XRT_INPUT_NAME_INVALID as found.
		 */
		bool t2 = find_xdev_input_name_from_pairs(xdev, xbp, source_dpad_activate, &target_dpad_activate);

		// We couldn't find the needed inputs on the device.
		if (!t1 || !t2) {
			oxr_slog(slog, "\t\t\tRejected! (NO XRT BINDING PAIR)\n");
			return;
		}
	}

	struct xrt_input *input = NULL;
	if (!oxr_xdev_find_input(xdev, target_name, &input) &&
	    !find_generic_grip_surface_fallback(xdev, profile, binding_point, &input)) {
		oxr_slog(slog, "\t\t\tRejected! (NO XDEV INPUT)\n");
		return;
	}

	struct xrt_input *activate_input = NULL;
	if (target_dpad_activate != XRT_INPUT_NAME_INVALID &&
	    !oxr_xdev_find_input(xdev, target_dpad_activate, &activate_input)) {
		oxr_slog(slog, "\t\t\tRejected! (NO DPAD ACTIVATE INPUT)\n");
		return;
	}

	// The binding point is generated from a template, so we can use the template path.
	const char *path_string = binding_point->templ->path;

	struct oxr_input_transform *transforms = NULL;
	size_t transform_count = 0;
	enum xrt_input_type input_type = XRT_GET_INPUT_TYPE(input->name);
	XrActionType action_type = act_attached->act_ref->action_type;
	enum oxr_dpad_region dpad_region = 0;
	if (get_dpad_region_from_binding_point(binding_point, &dpad_region)) {
		const struct oxr_dpad_entry *entry = oxr_dpad_state_get( //
		    &profile->dpad_state,                                //
		    act_attached->act_set_attached->act_set_key);        //

		const struct oxr_dpad_binding_modification *dpad_binding_modification = NULL;
		find_matching_dpad(entry, binding_point->dpad.emulation->path, &dpad_binding_modification);

		enum xrt_input_name activate_name =
		    activate_input != NULL ? activate_input->name : XRT_INPUT_NAME_INVALID;
		enum xrt_input_type activate_type = XRT_GET_INPUT_TYPE(activate_name);

		bool bret = oxr_input_transform_create_chain_dpad( //
		    log,                                           //
		    slog,                                          //
		    input_type,                                    //
		    action_type,                                   //
		    path_string,                                   //
		    dpad_binding_modification,                     //
		    dpad_region,                                   //
		    activate_type,                                 //
		    activate_input,                                //
		    &transforms,                                   //
		    &transform_count);                             //
		if (!bret) {
			oxr_slog(slog, "\t\t\tRejected! (NO DPAD TRANSFORM)\n");
			return;
		}
	} else {
		bool bret = oxr_input_transform_create_chain( //
		    log,                                      //
		    slog,                                     //
		    input_type,                               //
		    action_type,                              //
		    action_name,                              //
		    binding_point,                            //
		    &transforms,                              //
		    &transform_count);                        //
		if (!bret) {
			oxr_slog(slog, "\t\t\tRejected! (NO TRANSFORM)\n");
			return;
		}
	}

	struct oxr_priority_group_state *priority_group_state = NULL;
	struct oxr_session_action_context *action_context = act_attached->act_set_attached->sess_context;
	XrResult ret = oxr_priority_group_cache_get_or_create( //
	    &action_context->priority_group_cache,             //
	    binding_point->priority_group,                     //
	    &priority_group_state);                            //
	if (ret != XR_SUCCESS) {
		oxr_input_transform_destroy(&transforms);
		oxr_slog(slog, "\t\t\tRejected! (FAILED TO ALLOCATE PRIORITY GROUP STATE)\n");
		return;
	}

	uint32_t index = input_storage->count++;
	input_storage->inputs[index] = XRT_C11_COMPOUND(struct oxr_action_input){
	    .xdev = xdev,
	    .input = input,
	    .binding_point = binding_point,
	    .priority_group_state = priority_group_state,
	    .transforms = transforms,
	    .transform_count = transform_count,
	};

	oxr_slog(slog, "\t\t\tBound input: %s\n", u_str_xrt_input_name(input->name));
}

static void
append_resolved_output(struct oxr_sink_logger *slog,
                       struct xrt_device *xdev,
                       const struct xrt_binding_profile *xbp,
                       const struct oxr_binding_point *binding_point,
                       struct output_storage *storage)
{
	enum xrt_output_name output_name = binding_point->output.name;
	if (xbp != NULL) {
		output_name = XRT_OUTPUT_NAME_INVALID;
		for (size_t i = 0; i < xbp->output_count; i++) {
			if (binding_point->output.name == xbp->outputs[i].from) {
				output_name = xbp->outputs[i].device;
				break;
			}
		}
	}

	struct xrt_output *output = NULL;
	if (output_name == XRT_OUTPUT_NAME_INVALID || !oxr_xdev_find_output(xdev, output_name, &output)) {
		oxr_slog(slog, "\t\t\tRejected! (NO XDEV OUTPUT)\n");
		return;
	}

	uint32_t index = storage->count++;
	storage->outputs[index] = XRT_C11_COMPOUND(struct oxr_action_output){
	    .xdev = xdev,
	    .name = output_name,
	    .binding_point = binding_point,
	};

	oxr_slog(slog, "\t\t\tBound output: %s\n", u_str_xrt_output_name(output_name));
}

static bool
suggested_binding_matches(const struct oxr_suggested_binding *binding,
                          const struct oxr_action_attachment *act_attached,
                          XrPath subaction_path)
{
	return binding->act_key == act_attached->act_key && binding->binding_point->subaction->path == subaction_path;
}


/*
 *
 * Exported functions.
 *
 */

void
oxr_resolver_populate_input(struct oxr_logger *log,
                            struct oxr_sink_logger *slog,
                            const struct oxr_profile_resolver_info *info,
                            struct xrt_device *xdev,
                            XrPath subaction_path,
                            struct oxr_action_attachment *act_attached,
                            struct oxr_action_cache *cache)
{
	// Must hold true
	assert(log != NULL);
	assert(slog != NULL);
	assert(info != NULL);
	assert(xdev != NULL);
	assert(act_attached != NULL);
	assert(cache != NULL);
	assert(cache->inputs == NULL);
	assert(cache->outputs == NULL);
	assert(cache->input_count == 0);
	assert(cache->output_count == 0);

	const struct oxr_suggested_profile *profile = info->profile;
	if (profile == NULL) {
		oxr_slog(slog, "\t\tNo resolved binding info for device.\n");
		return;
	}

	// Convenience.
	XrActionType action_type = act_attached->act_ref->action_type;

	// Temporary storage for the resolved inputs.
	struct input_storage storage = XRT_STRUCT_INIT;

	for (size_t i = 0; i < profile->binding_count; i++) {
		const struct oxr_suggested_binding *suggested_binding = &profile->bindings[i];
		if (!suggested_binding_matches(suggested_binding, act_attached, subaction_path)) {
			continue;
		}

		assert(suggested_binding->binding_point != NULL);
		assert(suggested_binding->binding_point->templ != NULL);
		oxr_slog(slog, "\t\tSuggested: %s\n", suggested_binding->binding_point->templ->path);

		const struct oxr_binding_point *binding_point = NULL;
		if (!resolve_binding_point(slog, info, suggested_binding, action_type, &binding_point)) {
			// Has handled the rejection logging in resolve_binding_point.
			continue;
		}

		if (storage.count >= OXR_MAX_BINDINGS_PER_ACTION) {
			oxr_slog(slog, "\t\t\tRejected! (TOO MANY INPUT BINDINGS)\n");
			continue;
		}

		append_resolved_input(           //
		    log,                         //
		    slog,                        //
		    profile,                     //
		    suggested_binding,           //
		    act_attached->act_ref->name, //
		    xdev,                        //
		    info->xbp,                   //
		    binding_point,               //
		    act_attached,                //
		    &storage);                   //
	}

	if (storage.count > 0) {
		cache->inputs = U_TYPED_ARRAY_CALLOC(struct oxr_action_input, storage.count);
		if (cache->inputs == NULL) {
			for (uint32_t i = 0; i < storage.count; i++) {
				oxr_input_transform_destroy(&storage.inputs[i].transforms);
			}

			/*
			 * This basically never happens in practice. If it does,
			 * binding debug output already carries the reason in the slog.
			 */
			oxr_slog(slog, "\t\t\tRejected! (FAILED TO ALLOCATE RESOLVED INPUTS)\n");
			return;
		}
		memcpy(cache->inputs, storage.inputs, sizeof(struct oxr_action_input) * storage.count);
		cache->current.active = true;
		cache->input_count = storage.count;
	}
}

void
oxr_resolver_populate_output(struct oxr_logger *log,
                             struct oxr_sink_logger *slog,
                             const struct oxr_profile_resolver_info *info,
                             struct xrt_device *xdev,
                             XrPath subaction_path,
                             struct oxr_action_attachment *act_attached,
                             struct oxr_action_cache *cache)
{
	// Must hold true
	assert(log != NULL);
	assert(slog != NULL);
	assert(info != NULL);
	assert(xdev != NULL);
	assert(act_attached != NULL);
	assert(cache != NULL);
	assert(cache->inputs == NULL);
	assert(cache->outputs == NULL);
	assert(cache->input_count == 0);
	assert(cache->output_count == 0);

	const struct oxr_suggested_profile *profile = info->profile;
	if (profile == NULL) {
		oxr_slog(slog, "\t\t\tRejected! (NO RESOLVED BINDING INFO)\n");
		return;
	}

	// Convenience.
	XrActionType action_type = act_attached->act_ref->action_type;

	// Temporary storage for the resolved outputs.
	struct output_storage storage = XRT_STRUCT_INIT;

	for (size_t i = 0; i < profile->binding_count; i++) {
		const struct oxr_suggested_binding *suggested_binding = &profile->bindings[i];
		if (!suggested_binding_matches(suggested_binding, act_attached, subaction_path)) {
			continue;
		}

		assert(suggested_binding->binding_point != NULL);
		assert(suggested_binding->binding_point->templ != NULL);
		oxr_slog(slog, "\t\tSuggested: %s\n", suggested_binding->binding_point->templ->path);

		const struct oxr_binding_point *binding_point = NULL;
		if (!resolve_binding_point(slog, info, suggested_binding, action_type, &binding_point)) {
			continue;
		}

		if (storage.count >= OXR_MAX_BINDINGS_PER_ACTION) {
			oxr_slog(slog, "\t\t\tRejected! (TOO MANY OUTPUT BINDINGS)\n");
			continue;
		}

		append_resolved_output(slog, xdev, info->xbp, binding_point, &storage);
	}

	if (storage.count > 0) {
		cache->outputs = U_TYPED_ARRAY_CALLOC(struct oxr_action_output, storage.count);
		if (cache->outputs == NULL) {
			/*
			 * This basically never happens in practice. If it does,
			 * binding debug output already carries the reason in the slog.
			 */
			oxr_slog(slog, "\t\t\tRejected! (FAILED TO ALLOCATE RESOLVED OUTPUTS)\n");
			return;
		}
		memcpy(cache->outputs, storage.outputs, sizeof(struct oxr_action_output) * storage.count);
		cache->current.active = true;
		cache->output_count = storage.count;
	}
}

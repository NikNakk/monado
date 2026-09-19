// Copyright 2019-2024, Collabora, Ltd.
// Copyright 2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Multi client wrapper compositor.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup comp_multi
 */

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_session.h"

#include "os/os_time.h"
#include "os/os_threading.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "util/u_wait.h"
#include "util/u_debug.h"
#include "util/u_trace_marker.h"
#include "util/u_distortion_mesh.h"

#ifdef XRT_OS_LINUX
#include "util/u_linux.h"
#endif

#include "multi/comp_multi_private.h"
#include "multi/comp_multi_interface.h"

#include <math.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_OSX
#include <unistd.h>
#endif

#ifdef XRT_GRAPHICS_SYNC_HANDLE_IS_FD
#include <unistd.h>
#endif



#ifdef XRT_OS_OSX
/*
 * Reprojection source trace.
 *
 * This records the client-side frame identity before the multi compositor
 * replaces it with the native/system compositor frame id. The renderer trace
 * below is keyed by that same system_frame_id, so the two CSVs can be joined
 * exactly without changing any IPC or compositor ABI.
 */
static FILE *g_reprojection_source_trace = NULL;
static uint64_t g_reprojection_source_rows = 0;
static bool g_reprojection_source_previous_valid = false;
static int64_t g_reprojection_source_previous_client_frame_id = -1;
static const struct multi_compositor *g_reprojection_source_previous_client = NULL;

static bool
reprojection_trace_enabled(void)
{
	const char *explicit_value = getenv("XRT_MACOS_REPROJECTION_TRACE");
	if (explicit_value != NULL && explicit_value[0] != '\0') {
		return strcmp(explicit_value, "0") != 0 && strcmp(explicit_value, "off") != 0 &&
		       strcmp(explicit_value, "false") != 0;
	}

	const char *timing_value = getenv("PSVR2_TIMING_TRACE");
	return timing_value != NULL && timing_value[0] != '\0' && strcmp(timing_value, "0") != 0 &&
	       strcmp(timing_value, "off") != 0 && strcmp(timing_value, "false") != 0;
}

static void
reprojection_source_trace_open(void)
{
	if (g_reprojection_source_trace != NULL || !reprojection_trace_enabled()) {
		return;
	}

	const char *dir = getenv("PSVR2_TIMING_TRACE_DIR");
	if (dir == NULL || dir[0] == '\0') {
		dir = "/tmp";
	}

	char path[1024];
	size_t dir_len = strlen(dir);
	const char *separator = dir_len > 0 && dir[dir_len - 1] == '/' ? "" : "/";
	snprintf(path, sizeof(path), "%s%smonado_psvr2_%d_reprojection_source.csv", dir, separator, (int)getpid());

	g_reprojection_source_trace = fopen(path, "w");
	if (g_reprojection_source_trace == NULL) {
		U_LOG_W("Could not open reprojection source trace '%s'", path);
		return;
	}

	setvbuf(g_reprojection_source_trace, NULL, _IOFBF, 128 * 1024);
	fputs("system_frame_id,system_display_time_ns,source_valid,source_changed,focused,client_frame_id,"
	      "client_display_time_ns,layer_index,layer_type,layer_timestamp_ns,view_count,"
	      "left_image_index,left_array_index,right_image_index,right_array_index,"
	      "left_src_qx,left_src_qy,left_src_qz,left_src_qw,left_src_px,left_src_py,left_src_pz,"
	      "right_src_qx,right_src_qy,right_src_qz,right_src_qw,right_src_px,right_src_py,right_src_pz\n",
	      g_reprojection_source_trace);
	fflush(g_reprojection_source_trace);
	U_LOG_I("Reprojection source trace enabled: %s", path);
}

static void
reprojection_source_trace_close(void)
{
	if (g_reprojection_source_trace == NULL) {
		return;
	}

	fflush(g_reprojection_source_trace);
	fclose(g_reprojection_source_trace);
	g_reprojection_source_trace = NULL;
	g_reprojection_source_rows = 0;
	g_reprojection_source_previous_valid = false;
	g_reprojection_source_previous_client_frame_id = -1;
	g_reprojection_source_previous_client = NULL;
}

static bool
multi_layer_is_projection(const struct multi_layer_entry *layer)
{
	return layer->data.type == XRT_LAYER_PROJECTION || layer->data.type == XRT_LAYER_PROJECTION_DEPTH;
}

static struct multi_compositor *
find_reprojection_source_client(struct multi_compositor **array, size_t count, uint32_t *out_layer_index)
{
	struct multi_compositor *fallback = NULL;
	uint32_t fallback_layer = 0;

	for (size_t k = 0; k < count; ++k) {
		struct multi_compositor *mc = array[k];
		if (mc == NULL) {
			continue;
		}

		for (uint32_t i = 0; i < mc->delivered.layer_count; ++i) {
			if (!multi_layer_is_projection(&mc->delivered.layers[i])) {
				continue;
			}

			if (mc->state.focused) {
				*out_layer_index = i;
				return mc;
			}

			if (fallback == NULL) {
				fallback = mc;
				fallback_layer = i;
			}
			break;
		}
	}

	if (fallback != NULL) {
		*out_layer_index = fallback_layer;
	}
	return fallback;
}

static const struct xrt_layer_projection_view_data *
multi_projection_views(const struct xrt_layer_data *data)
{
	if (data->type == XRT_LAYER_PROJECTION) {
		return data->proj.v;
	}
	if (data->type == XRT_LAYER_PROJECTION_DEPTH) {
		return data->depth.v;
	}
	return NULL;
}

static void
trace_reprojection_source(struct multi_compositor **array,
                          size_t count,
                          int64_t system_frame_id,
                          int64_t system_display_time_ns)
{
	reprojection_source_trace_open();
	if (g_reprojection_source_trace == NULL) {
		return;
	}

	uint32_t layer_index = 0;
	struct multi_compositor *mc = find_reprojection_source_client(array, count, &layer_index);
	if (mc == NULL) {
		fprintf(g_reprojection_source_trace,
		        "%lld,%lld,0,0,0,-1,0,0,0,0,0,0,0,0,0,"
		        "nan,nan,nan,nan,nan,nan,nan,nan,nan,nan,nan,nan,nan,nan\n",
		        (long long)system_frame_id,
		        (long long)system_display_time_ns);
		g_reprojection_source_previous_valid = false;
		goto flush_maybe;
	}

	const struct multi_layer_entry *layer = &mc->delivered.layers[layer_index];
	const struct xrt_layer_data *data = &layer->data;
	const struct xrt_layer_projection_view_data *views = multi_projection_views(data);
	if (views == NULL || data->view_count == 0) {
		return;
	}

	bool source_changed = !g_reprojection_source_previous_valid ||
	                      g_reprojection_source_previous_client != mc ||
	                      g_reprojection_source_previous_client_frame_id != mc->delivered.data.frame_id;

	const struct xrt_layer_projection_view_data *left = &views[0];
	const struct xrt_layer_projection_view_data *right = data->view_count > 1 ? &views[1] : &views[0];

	fprintf(g_reprojection_source_trace,
	        "%lld,%lld,1,%d,%d,%lld,%lld,%u,%u,%lld,%u,%u,%u,%u,%u,"
	        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
	        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
	        (long long)system_frame_id,
	        (long long)system_display_time_ns,
	        source_changed ? 1 : 0,
	        mc->state.focused ? 1 : 0,
	        (long long)mc->delivered.data.frame_id,
	        (long long)mc->delivered.data.display_time_ns,
	        layer_index,
	        (unsigned)data->type,
	        (long long)data->timestamp,
	        data->view_count,
	        left->sub.image_index,
	        left->sub.array_index,
	        right->sub.image_index,
	        right->sub.array_index,
	        left->pose.orientation.x,
	        left->pose.orientation.y,
	        left->pose.orientation.z,
	        left->pose.orientation.w,
	        left->pose.position.x,
	        left->pose.position.y,
	        left->pose.position.z,
	        right->pose.orientation.x,
	        right->pose.orientation.y,
	        right->pose.orientation.z,
	        right->pose.orientation.w,
	        right->pose.position.x,
	        right->pose.position.y,
	        right->pose.position.z);

	g_reprojection_source_previous_valid = true;
	g_reprojection_source_previous_client = mc;
	g_reprojection_source_previous_client_frame_id = mc->delivered.data.frame_id;

flush_maybe:
	g_reprojection_source_rows++;
	if ((g_reprojection_source_rows % 240) == 0) {
		fflush(g_reprojection_source_trace);
	}
}
#endif

/*
 *
 * Render thread.
 *
 */

static void
do_projection_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = layer->xdev;

	// Cast away
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	// Do not need to copy the reference, but should verify the pointers for consistency
	for (uint32_t j = 0; j < data->view_count; j++) {
		if (layer->xscs[j] == NULL) {
			U_LOG_E("Invalid swap chain for projection layer #%u!", i);
			return;
		}
	}

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for projection layer #%u!", i);
		return;
	}

	xrt_comp_layer_projection(xc, xdev, layer->xscs, data);
}

static void
do_projection_layer_depth(struct xrt_compositor *xc,
                          struct multi_compositor *mc,
                          struct multi_layer_entry *layer,
                          uint32_t i)
{
	struct xrt_device *xdev = layer->xdev;

	struct xrt_swapchain *xsc[XRT_MAX_VIEWS];
	struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS];
	// Cast away
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	for (uint32_t j = 0; j < data->view_count; j++) {
		xsc[j] = layer->xscs[j];
		d_xsc[j] = layer->xscs[j + data->view_count];

		if (xsc[j] == NULL || d_xsc[j] == NULL) {
			U_LOG_E("Invalid swap chain for projection layer #%u!", i);
			return;
		}
	}

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for projection layer #%u!", i);
		return;
	}


	xrt_comp_layer_projection_depth(xc, xdev, xsc, d_xsc, data);
}

static bool
do_single(struct xrt_compositor *xc,
          struct multi_compositor *mc,
          struct multi_layer_entry *layer,
          uint32_t i,
          const char *name,
          struct xrt_device **out_xdev,
          struct xrt_swapchain **out_xcs,
          struct xrt_layer_data **out_data)
{
	struct xrt_device *xdev = layer->xdev;
	struct xrt_swapchain *xcs = layer->xscs[0];

	if (xcs == NULL) {
		U_LOG_E("Invalid swapchain for layer #%u '%s'!", i, name);
		return false;
	}

	if (xdev == NULL) {
		U_LOG_E("Invalid xdev for layer #%u '%s'!", i, name);
		return false;
	}

	// Cast away
	struct xrt_layer_data *data = (struct xrt_layer_data *)&layer->data;

	*out_xdev = xdev;
	*out_xcs = xcs;
	*out_data = data;

	return true;
}

static void
do_quad_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "quad", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_quad(xc, xdev, xcs, data);
}

static void
do_cube_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "cube", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_cube(xc, xdev, xcs, data);
}

static void
do_cylinder_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "cylinder", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_cylinder(xc, xdev, xcs, data);
}

static void
do_equirect1_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "equirect1", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_equirect1(xc, xdev, xcs, data);
}

static void
do_equirect2_layer(struct xrt_compositor *xc, struct multi_compositor *mc, struct multi_layer_entry *layer, uint32_t i)
{
	struct xrt_device *xdev = NULL;
	struct xrt_swapchain *xcs = NULL;
	struct xrt_layer_data *data = NULL;

	if (!do_single(xc, mc, layer, i, "equirect2", &xdev, &xcs, &data)) {
		return;
	}

	xrt_comp_layer_equirect2(xc, xdev, xcs, data);
}

static int
overlay_sort_func(const void *a, const void *b)
{
	struct multi_compositor *mc_a = *(struct multi_compositor **)a;
	struct multi_compositor *mc_b = *(struct multi_compositor **)b;

	if (mc_a->state.z_order < mc_b->state.z_order) {
		return -1;
	}

	if (mc_a->state.z_order > mc_b->state.z_order) {
		return 1;
	}

	return 0;
}

static enum xrt_blend_mode
find_active_blend_mode(struct multi_compositor **overlay_sorted_clients, size_t size)
{
	if (overlay_sorted_clients == NULL)
		return XRT_BLEND_MODE_OPAQUE;

	const struct multi_compositor *first_visible = NULL;
	for (size_t k = 0; k < size; ++k) {
		const struct multi_compositor *mc = overlay_sorted_clients[k];
		assert(mc != NULL);

		// if a focused client is found just return, "first_visible" has lower priority and can be ignored.
		if (mc->state.focused) {
			assert(mc->state.visible);
			return mc->delivered.data.env_blend_mode;
		}

		if (first_visible == NULL && mc->state.visible) {
			first_visible = mc;
		}
	}
	if (first_visible != NULL)
		return first_visible->delivered.data.env_blend_mode;
	return XRT_BLEND_MODE_OPAQUE;
}

static void
transfer_layers_locked(struct multi_system_compositor *msc, int64_t display_time_ns, int64_t system_frame_id)
{
	COMP_TRACE_MARKER();

	struct xrt_compositor *xc = &msc->xcn->base;

	struct multi_compositor *array[MULTI_MAX_CLIENTS] = {0};

	// To mark latching.
	int64_t now_ns = os_monotonic_get_ns();

	size_t count = 0;
	for (size_t k = 0; k < ARRAY_SIZE(array); k++) {
		struct multi_compositor *mc = msc->clients[k];

		// Array can be empty
		if (mc == NULL) {
			continue;
		}

		// Even if it's not shown, make sure that frames are delivered.
		multi_compositor_deliver_any_frames(mc, display_time_ns);

		// None of the data in this slot is valid, don't check access it.
		if (!mc->delivered.active) {
			continue;
		}

		// The client isn't visible, do not submit it's layers.
		if (!mc->state.visible) {
			// Need to drop delivered frame as it shouldn't be reused.
			multi_compositor_retire_delivered_locked(mc, now_ns);
			continue;
		}

		// Just in case.
		if (!mc->state.session_active) {
			U_LOG_W("Session is visible but not active.");

			// Need to drop delivered frame as it shouldn't be reused.
			multi_compositor_retire_delivered_locked(mc, now_ns);
			continue;
		}

		// The list_and_timing_lock is held when callign this function.
		multi_compositor_latch_frame_locked(mc, now_ns, system_frame_id);

		array[count++] = msc->clients[k];
	}

	// Sort the stack array
	qsort(array, count, sizeof(struct multi_compositor *), overlay_sort_func);

#ifdef XRT_OS_OSX
	trace_reprojection_source(array, count, system_frame_id, display_time_ns);
#endif

	// find first (ordered by bottom to top) active client to retrieve xrt_layer_frame_data
	const enum xrt_blend_mode blend_mode = find_active_blend_mode(array, count);

	const struct xrt_layer_frame_data data = {
	    .frame_id = system_frame_id,
	    .display_time_ns = display_time_ns,
	    .env_blend_mode = blend_mode,
	};
	xrt_comp_layer_begin(xc, &data);

	// Copy all active layers.
	for (size_t k = 0; k < count; k++) {
		struct multi_compositor *mc = array[k];
		assert(mc != NULL);

		for (uint32_t i = 0; i < mc->delivered.layer_count; i++) {
			struct multi_layer_entry *layer = &mc->delivered.layers[i];

			switch (layer->data.type) {
			case XRT_LAYER_PROJECTION: do_projection_layer(xc, mc, layer, i); break;
			case XRT_LAYER_PROJECTION_DEPTH: do_projection_layer_depth(xc, mc, layer, i); break;
			case XRT_LAYER_QUAD: do_quad_layer(xc, mc, layer, i); break;
			case XRT_LAYER_CUBE: do_cube_layer(xc, mc, layer, i); break;
			case XRT_LAYER_CYLINDER: do_cylinder_layer(xc, mc, layer, i); break;
			case XRT_LAYER_EQUIRECT1: do_equirect1_layer(xc, mc, layer, i); break;
			case XRT_LAYER_EQUIRECT2: do_equirect2_layer(xc, mc, layer, i); break;
			default: U_LOG_E("Unhandled layer type '%i'!", layer->data.type); break;
			}
		}
	}
}

static void
broadcast_timings_to_clients(struct multi_system_compositor *msc, int64_t predicted_display_time_ns)
{
	COMP_TRACE_MARKER();

	os_mutex_lock(&msc->list_and_timing_lock);

	for (size_t i = 0; i < ARRAY_SIZE(msc->clients); i++) {
		struct multi_compositor *mc = msc->clients[i];
		if (mc == NULL) {
			continue;
		}

		os_mutex_lock(&mc->slot_lock);
		mc->slot_next_frame_display = predicted_display_time_ns;
		os_mutex_unlock(&mc->slot_lock);
	}

	os_mutex_unlock(&msc->list_and_timing_lock);
}

static void
broadcast_timings_to_pacers(struct multi_system_compositor *msc,
                            int64_t predicted_display_time_ns,
                            int64_t predicted_display_period_ns,
                            int64_t diff_ns)
{
	COMP_TRACE_MARKER();

	os_mutex_lock(&msc->list_and_timing_lock);

	for (size_t i = 0; i < ARRAY_SIZE(msc->clients); i++) {
		struct multi_compositor *mc = msc->clients[i];
		if (mc == NULL) {
			continue;
		}

		u_pa_info(                       //
		    mc->upa,                     //
		    predicted_display_time_ns,   //
		    predicted_display_period_ns, //
		    diff_ns);                    //

		os_mutex_lock(&mc->slot_lock);
		mc->slot_next_frame_display = predicted_display_time_ns;
		os_mutex_unlock(&mc->slot_lock);
	}

	msc->last_timings.predicted_display_time_ns = predicted_display_time_ns;
	msc->last_timings.predicted_display_period_ns = predicted_display_period_ns;
	msc->last_timings.diff_ns = diff_ns;

	os_mutex_unlock(&msc->list_and_timing_lock);
}

static void
wait_frame(struct os_precise_sleeper *sleeper, struct xrt_compositor *xc, int64_t frame_id, int64_t wake_up_time_ns)
{
	COMP_TRACE_MARKER();

	// Wait until the given wake up time.
	u_wait_until(sleeper, wake_up_time_ns);

	int64_t now_ns = os_monotonic_get_ns();

	// Signal that we woke up.
	xrt_comp_mark_frame(xc, frame_id, XRT_COMPOSITOR_FRAME_POINT_WOKE, now_ns);
}

static void
update_session_state_locked(struct multi_system_compositor *msc)
{
	struct xrt_compositor *xc = &msc->xcn->base;

	//! @todo Make this not be hardcoded.
	const struct xrt_begin_session_info begin_session_info = {
	    .view_type = XRT_VIEW_TYPE_STEREO,
	    .ext_hand_tracking_enabled = false,
	    .ext_hand_tracking_data_source_enabled = false,
	    .ext_eye_gaze_interaction_enabled = false,
	    .ext_hand_interaction_enabled = false,
	    .htc_facial_tracking_enabled = false,
	    .fb_body_tracking_enabled = false,
	    .fb_face_tracking2_enabled = false,
	    .meta_body_tracking_full_body_enabled = false,
	    .meta_body_tracking_calibration_enabled = false,
	    .android_face_tracking_enabled = false,
	};

	switch (msc->sessions.state) {
	case MULTI_SYSTEM_STATE_INIT_WARM_START:
		// Produce at least one frame on init.
		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPING;
		xrt_comp_begin_session(xc, &begin_session_info);
		U_LOG_I("Doing warm start, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_STOPPED:
		if (msc->sessions.active_count == 0) {
			break;
		}

		msc->sessions.state = MULTI_SYSTEM_STATE_RUNNING;
		xrt_comp_begin_session(xc, &begin_session_info);
		U_LOG_I("Started native session, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_RUNNING:
		if (msc->sessions.active_count > 0) {
			break;
		}

		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPING;
		U_LOG_D("Stopping native session, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_STOPPING:
		// Just in case
		if (msc->sessions.active_count > 0) {
			msc->sessions.state = MULTI_SYSTEM_STATE_RUNNING;
			U_LOG_D("Restarting native session, %u active app session(s).",
			        (uint32_t)msc->sessions.active_count);
			break;
		}

		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPED;
		xrt_comp_end_session(xc);
		U_LOG_I("Stopped native session, %u active app session(s).", (uint32_t)msc->sessions.active_count);
		break;

	case MULTI_SYSTEM_STATE_INVALID:
	default:
		U_LOG_E("Got invalid state %u", msc->sessions.state);
		msc->sessions.state = MULTI_SYSTEM_STATE_STOPPING;
		assert(false);
	}
}

static int
multi_main_loop(struct multi_system_compositor *msc)
{
	U_TRACE_SET_THREAD_NAME("Multi Client Module");
	os_thread_helper_name(&msc->oth, "Multi Client Module");

#ifdef XRT_OS_LINUX
	// Try to raise priority of this thread.
	u_linux_try_to_set_realtime_priority_on_thread(U_LOGGING_INFO, "Multi Client Module");
#endif

	struct xrt_compositor *xc = &msc->xcn->base;

	// For wait frame.
	struct os_precise_sleeper sleeper = {0};
	os_precise_sleeper_init(&sleeper);

	// Protect the thread state and the sessions state.
	os_thread_helper_lock(&msc->oth);

	while (os_thread_helper_is_running_locked(&msc->oth)) {

		// Updates msc->sessions.active depending on active client sessions.
		update_session_state_locked(msc);

		if (msc->sessions.state == MULTI_SYSTEM_STATE_STOPPED) {
			// Sleep and wait to be signaled.
			os_thread_helper_wait_locked(&msc->oth);

			// Loop back to running and session check.
			continue;
		}

		// Unlock the thread after the checks has been done.
		os_thread_helper_unlock(&msc->oth);

		int64_t frame_id = -1;
		int64_t wake_up_time_ns = 0;
		int64_t predicted_gpu_time_ns = 0;
		int64_t predicted_display_time_ns = 0;
		int64_t predicted_display_period_ns = 0;

		// Get the information for the next frame.
		xrt_comp_predict_frame(            //
		    xc,                            //
		    &frame_id,                     //
		    &wake_up_time_ns,              //
		    &predicted_gpu_time_ns,        //
		    &predicted_display_time_ns,    //
		    &predicted_display_period_ns); //

		// Do this as soon as we have the new display time.
		broadcast_timings_to_clients(msc, predicted_display_time_ns);

		// Now we can wait.
		wait_frame(&sleeper, xc, frame_id, wake_up_time_ns);

		int64_t now_ns = os_monotonic_get_ns();
		int64_t diff_ns = predicted_display_time_ns - now_ns;

		// Now we know the diff, broadcast to pacers.
		broadcast_timings_to_pacers(msc, predicted_display_time_ns, predicted_display_period_ns, diff_ns);

		xrt_comp_begin_frame(xc, frame_id);

		// Make sure that the clients doesn't go away while we transfer layers.
		os_mutex_lock(&msc->list_and_timing_lock);
		transfer_layers_locked(msc, predicted_display_time_ns, frame_id);
		os_mutex_unlock(&msc->list_and_timing_lock);

		xrt_comp_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);

		// Re-lock the thread for check in while statement.
		os_thread_helper_lock(&msc->oth);
	}

	// Clean up the sessions state.
	switch (msc->sessions.state) {
	case MULTI_SYSTEM_STATE_RUNNING:
	case MULTI_SYSTEM_STATE_STOPPING:
		U_LOG_I("Stopped native session, shutting down.");
		xrt_comp_end_session(xc);
		break;
	case MULTI_SYSTEM_STATE_STOPPED: U_LOG_I("Already stopped, nothing to clean up."); break;
	case MULTI_SYSTEM_STATE_INIT_WARM_START:
		U_LOG_I("Cleaning up from warm start state.");
		xrt_comp_end_session(xc);
		break;
	case MULTI_SYSTEM_STATE_INVALID:
		U_LOG_W("Cleaning up from invalid state.");
		// Best effort cleanup
		xrt_comp_end_session(xc);
		break;
	default: U_LOG_E("Unknown session state during cleanup: %d", msc->sessions.state); assert(false);
	}

	os_thread_helper_unlock(&msc->oth);

	os_precise_sleeper_deinit(&sleeper);

	return 0;
}

static void *
thread_func(void *ptr)
{
	return (void *)(intptr_t)multi_main_loop((struct multi_system_compositor *)ptr);
}


/*
 *
 * System multi compositor functions.
 *
 */

static xrt_result_t
system_compositor_set_state(
    struct xrt_system_compositor *xsc, struct xrt_compositor *xc, bool visible, bool focused, int64_t timestamp_ns)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	//! @todo Locking?
	if (mc->state.visible != visible || mc->state.focused != focused) {
		mc->state.visible = visible;
		mc->state.focused = focused;

		union xrt_session_event xse = XRT_STRUCT_INIT;
		xse.type = XRT_SESSION_EVENT_STATE_CHANGE;
		xse.state.visible = visible;
		xse.state.focused = focused;
		xse.state.timestamp_ns = timestamp_ns;

		return multi_compositor_push_event(mc, &xse);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
system_compositor_set_z_order(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, int64_t z_order)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	//! @todo Locking?
	mc->state.z_order = z_order;

	return XRT_SUCCESS;
}

static xrt_result_t
system_compositor_set_main_app_visibility(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, bool visible)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_OVERLAY_CHANGE;
	xse.overlay.visible = visible;

	return multi_compositor_push_event(mc, &xse);
}

static xrt_result_t
system_compositor_notify_loss_pending(struct xrt_system_compositor *xsc,
                                      struct xrt_compositor *xc,
                                      int64_t loss_time_ns)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_LOSS_PENDING;
	xse.loss_pending.loss_time_ns = loss_time_ns;

	return multi_compositor_push_event(mc, &xse);
}

static xrt_result_t
system_compositor_notify_lost(struct xrt_system_compositor *xsc, struct xrt_compositor *xc)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_LOST;

	return multi_compositor_push_event(mc, &xse);
}

static xrt_result_t
system_compositor_notify_display_refresh_changed(struct xrt_system_compositor *xsc,
                                                 struct xrt_compositor *xc,
                                                 float from_display_refresh_rate_hz,
                                                 float to_display_refresh_rate_hz)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);
	struct multi_compositor *mc = multi_compositor(xc);
	(void)msc;

	union xrt_session_event xse = XRT_STRUCT_INIT;
	xse.type = XRT_SESSION_EVENT_DISPLAY_REFRESH_RATE_CHANGE;
	xse.display.from_display_refresh_rate_hz = from_display_refresh_rate_hz;
	xse.display.to_display_refresh_rate_hz = to_display_refresh_rate_hz;

	return multi_compositor_push_event(mc, &xse);
}


/*
 *
 * System compositor functions.
 *
 */

static xrt_result_t
system_compositor_create_native_compositor(struct xrt_system_compositor *xsc,
                                           const struct xrt_session_info *xsi,
                                           struct xrt_session_event_sink *xses,
                                           struct xrt_compositor_native **out_xcn)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);

	return multi_compositor_create(msc, xsi, xses, out_xcn);
}

static void
system_compositor_destroy(struct xrt_system_compositor *xsc)
{
	struct multi_system_compositor *msc = multi_system_compositor(xsc);

	// Destroy the render thread first, destroy also stops the thread.
	os_thread_helper_destroy(&msc->oth);

#ifdef XRT_OS_OSX
	reprojection_source_trace_close();
#endif

	u_paf_destroy(&msc->upaf);

	xrt_comp_native_destroy(&msc->xcn);

	os_mutex_destroy(&msc->list_and_timing_lock);

	free(msc);
}


/*
 *
 * 'Exported' functions.
 *
 */

void
multi_system_compositor_update_session_status(struct multi_system_compositor *msc, bool active)
{
	os_thread_helper_lock(&msc->oth);

	if (active) {
		assert(msc->sessions.active_count < UINT32_MAX);
		msc->sessions.active_count++;

		// If the thread is sleeping wake it up.
		os_thread_helper_signal_locked(&msc->oth);
	} else {
		assert(msc->sessions.active_count > 0);
		msc->sessions.active_count--;
	}

	os_thread_helper_unlock(&msc->oth);
}

xrt_result_t
comp_multi_create_system_compositor(struct xrt_compositor_native *xcn,
                                    struct u_pacing_app_factory *upaf,
                                    const struct xrt_system_compositor_info *xsci,
                                    bool do_warm_start,
                                    struct xrt_system_compositor **out_xsysc)
{
	struct multi_system_compositor *msc = U_TYPED_CALLOC(struct multi_system_compositor);
	msc->base.create_native_compositor = system_compositor_create_native_compositor;
	msc->base.destroy = system_compositor_destroy;
	msc->xmcc.set_state = system_compositor_set_state;
	msc->xmcc.set_z_order = system_compositor_set_z_order;
	msc->xmcc.set_main_app_visibility = system_compositor_set_main_app_visibility;
	msc->xmcc.notify_loss_pending = system_compositor_notify_loss_pending;
	msc->xmcc.notify_lost = system_compositor_notify_lost;
	msc->xmcc.notify_display_refresh_changed = system_compositor_notify_display_refresh_changed;
	msc->base.xmcc = &msc->xmcc;
	msc->base.info = *xsci;
	msc->upaf = upaf;
	msc->xcn = xcn;
	msc->sessions.active_count = 0;
	msc->sessions.state = do_warm_start ? MULTI_SYSTEM_STATE_INIT_WARM_START : MULTI_SYSTEM_STATE_STOPPED;

	os_mutex_init(&msc->list_and_timing_lock);

	//! @todo Make the clients not go from IDLE to READY before we have completed a first frame.
	// Make sure there is at least some sort of valid frame data here.
	msc->last_timings.predicted_display_time_ns = os_monotonic_get_ns();   // As good as any time.
	msc->last_timings.predicted_display_period_ns = U_TIME_1MS_IN_NS * 16; // Just a wild guess.
	msc->last_timings.diff_ns = U_TIME_1MS_IN_NS * 5;                      // Make sure it's not zero at least.

	int ret = os_thread_helper_init(&msc->oth);
	if (ret < 0) {
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}

	os_thread_helper_start(&msc->oth, thread_func, msc);

	*out_xsysc = &msc->base;

	return XRT_SUCCESS;
}

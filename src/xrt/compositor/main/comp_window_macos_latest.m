// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental newest-frame macOS presentation wrapper.
 *
 * This diagnostic translation unit reuses the existing macOS target but adds an
 * opt-in present-worker mode which lets CAMetalLayer::nextDrawable block off the
 * compositor thread. Once a drawable becomes available, the worker atomically
 * selects the newest pending completed compositor frame and retires the older
 * active frame rather than presenting it late.
 */

/* Keep the existing implementation available as the legacy A/B path. */
#define comp_window_macos_create comp_window_macos_create_legacy
#define comp_target_factory_macos comp_target_factory_macos_legacy
#include "comp_window_macos.m"
#undef comp_target_factory_macos
#undef comp_window_macos_create

DEBUG_GET_ONCE_BOOL_OPTION(macos_present_latest_frame, "XRT_MACOS_PRESENT_LATEST_FRAME", false)

static FILE *macos_latest_drawable_trace = NULL;

static void
macos_latest_trace(const char *event,
                   uint64_t event_ns,
                   id<CAMetalDrawable> drawable,
                   uint64_t acquire_begin_ns,
                   uint64_t acquire_end_ns,
                   const struct macos_present_job *initial_job,
                   const struct macos_present_job *selected_job,
                   uint64_t superseded_frame_id)
{
	if (macos_latest_drawable_trace == NULL) {
		return;
	}

	uint64_t wait_ns = acquire_end_ns > acquire_begin_ns ? acquire_end_ns - acquire_begin_ns : 0;
	uint64_t source_age_ns =
	    selected_job != NULL && event_ns > selected_job->enqueue_ns ? event_ns - selected_job->enqueue_ns : 0;
	uint64_t initial_frame_id = initial_job != NULL ? initial_job->frame_id : 0;
	uint64_t selected_frame_id = selected_job != NULL ? selected_job->frame_id : 0;
	uint64_t selected_timeline_value = selected_job != NULL ? selected_job->timeline_value : 0;
	uint32_t selected_image_index = selected_job != NULL ? selected_job->image_index : 0;
	uint64_t selected_enqueue_ns = selected_job != NULL ? selected_job->enqueue_ns : 0;

	flockfile(macos_latest_drawable_trace);
	fprintf(macos_latest_drawable_trace,
	        "%s,%llu,%p,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%llu,%llu\n",
	        event, (unsigned long long)event_ns, (void *)drawable,
	        (unsigned long long)acquire_begin_ns, (unsigned long long)acquire_end_ns,
	        (unsigned long long)wait_ns, (unsigned long long)initial_frame_id,
	        (unsigned long long)selected_frame_id, (unsigned long long)superseded_frame_id,
	        (unsigned long long)selected_timeline_value, selected_image_index,
	        (unsigned long long)selected_enqueue_ns, (unsigned long long)source_age_ns);
	fflush(macos_latest_drawable_trace);
	funlockfile(macos_latest_drawable_trace);
}

static void
macos_latest_present_worker_run_one(struct comp_window_macos *cwm)
{
	struct macos_present_job initial_job;
	struct macos_present_job selected_job;
	bool selected_newer = false;

	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (cwm->present_worker_shutdown || !cwm->pending_present_job_valid) {
		cwm->present_worker_scheduled = false;
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		dispatch_group_leave(cwm->present_worker_group);
		return;
	}
	initial_job = cwm->pending_present_job;
	selected_job = initial_job;
	cwm->pending_present_job_valid = false;
	pthread_mutex_unlock(&cwm->present_worker_mutex);

	@autoreleasepool {
		uint64_t acquire_begin_ns = os_monotonic_get_ns();
		macos_latest_trace("acquire_begin", acquire_begin_ns, nil, acquire_begin_ns, 0, &initial_job,
		                   &initial_job, 0);
		macos_trace_present_worker(cwm, "latest_drawable_begin", &initial_job, acquire_begin_ns, 0,
		                           acquire_begin_ns, acquire_begin_ns, 0, 0, 0,
		                           debug_get_bool_option_macos_metal_shared_event_wait() &&
		                               cwm->render_complete_event != nil);

		id<CAMetalDrawable> drawable = [cwm->metal_layer nextDrawable];
		uint64_t acquire_end_ns = os_monotonic_get_ns();
		macos_trace_present_worker(cwm, "latest_drawable_end", &initial_job, acquire_end_ns, 0,
		                           acquire_begin_ns, acquire_begin_ns, acquire_end_ns, 0, 0,
		                           debug_get_bool_option_macos_metal_shared_event_wait() &&
		                               cwm->render_complete_event != nil);
		macos_latest_trace(drawable != nil ? "acquired" : "acquire_nil", acquire_end_ns, drawable,
		                   acquire_begin_ns, acquire_end_ns, &initial_job, &initial_job, 0);

		if (drawable == nil) {
			macos_retire_unpresented_job(cwm, &initial_job, "latest_drawable_error", 0);
		} else {
			/*
			 * The key difference from the legacy present worker: nextDrawable is
			 * allowed to block independently. Only after it returns do we bind the
			 * drawable to a compositor frame, selecting the newest pending frame.
			 */
			pthread_mutex_lock(&cwm->present_worker_mutex);
			if (!cwm->present_worker_shutdown && cwm->pending_present_job_valid &&
			    cwm->pending_present_job.frame_id > initial_job.frame_id) {
				selected_job = cwm->pending_present_job;
				cwm->pending_present_job_valid = false;
				selected_newer = true;
				cwm->worker_jobs_superseded++;
			}
			bool shutdown = cwm->present_worker_shutdown;
			pthread_mutex_unlock(&cwm->present_worker_mutex);

			if (shutdown) {
				macos_latest_trace("shutdown_release", os_monotonic_get_ns(), drawable, acquire_begin_ns,
				                   acquire_end_ns, &initial_job, &selected_job, 0);
				macos_retire_unpresented_job(cwm, &initial_job, "shutdown_drop", 0);
				if (selected_newer) {
					macos_retire_unpresented_job(cwm, &selected_job, "shutdown_drop", 0);
				}
			} else {
				if (selected_newer) {
					macos_latest_trace("supersede_active", os_monotonic_get_ns(), drawable,
					                   acquire_begin_ns, acquire_end_ns, &initial_job, &selected_job,
					                   initial_job.frame_id);
					macos_retire_unpresented_job(cwm, &initial_job, "active_superseded_after_drawable", 0);
				}

				macos_latest_trace("selected", os_monotonic_get_ns(), drawable, acquire_begin_ns,
				                   acquire_end_ns, &initial_job, &selected_job,
				                   selected_newer ? initial_job.frame_id : 0);

				/*
				 * Reuse the existing, well-tested Metal copy/present path by handing
				 * it the already-acquired drawable through its early-drawable slot.
				 * update_timings cannot race a second early acquisition while the
				 * present worker is enabled.
				 */
				pthread_mutex_lock(&cwm->present_worker_mutex);
				assert(cwm->prefetched_drawable == nil);
				cwm->prefetched_drawable = [drawable retain];
				cwm->prefetched_drawable_timeline_value = selected_job.timeline_value;
				cwm->prefetched_drawable_begin_ns = acquire_begin_ns;
				cwm->prefetched_drawable_end_ns = acquire_end_ns;
				pthread_mutex_unlock(&cwm->present_worker_mutex);

				VkResult ret = macos_execute_present_job(cwm, &selected_job, true);
				uint64_t execute_return_ns = os_monotonic_get_ns();
				macos_latest_trace(ret == VK_SUCCESS ? "execute_return" : "execute_error",
				                   execute_return_ns, drawable, acquire_begin_ns, acquire_end_ns,
				                   &initial_job, &selected_job,
				                   selected_newer ? initial_job.frame_id : 0);

				/* Defensive cleanup if execution failed before consuming the handoff. */
				pthread_mutex_lock(&cwm->present_worker_mutex);
				bool drawable_unconsumed = cwm->prefetched_drawable != nil;
				pthread_mutex_unlock(&cwm->present_worker_mutex);
				if (drawable_unconsumed) {
					macos_release_prefetched_drawable(cwm, "latest_execute_unconsumed_release");
				}
			}
		}
	}

	/*
	 * Keep the serial worker fair to retirement work: each iteration queues at
	 * most one successor, so source images from superseded jobs can be released.
	 */
	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (!cwm->present_worker_shutdown && cwm->pending_present_job_valid) {
		dispatch_async(cwm->present_worker_queue, ^{ macos_latest_present_worker_run_one(cwm); });
	} else {
		cwm->present_worker_scheduled = false;
		dispatch_group_leave(cwm->present_worker_group);
	}
	pthread_mutex_unlock(&cwm->present_worker_mutex);
}

static VkResult
comp_window_macos_present_latest(struct comp_target *ct,
                                 struct vk_bundle_queue *present_queue,
                                 uint32_t index,
                                 uint64_t timeline_semaphore_value,
                                 int64_t desired_present_time_ns,
                                 int64_t present_slop_ns)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	if (!cwm->async_present || !cwm->present_worker_enabled ||
	    !debug_get_bool_option_macos_present_latest_frame()) {
		return comp_window_macos_present(ct, present_queue, index, timeline_semaphore_value,
		                                 desired_present_time_ns, present_slop_ns);
	}

	struct macos_present_job job = {
	    .frame_id = ++cwm->trace_frame_id,
	    .enqueue_ns = os_monotonic_get_ns(),
	    .image_reuse_wait_ns = cwm->last_image_acquire_wait_ns,
	    .image_index = index,
	    .timeline_value = timeline_semaphore_value,
	    .desired_present_time_ns = desired_present_time_ns,
	    .present_slop_ns = present_slop_ns,
	    .present_queue = present_queue,
	};
	if (index >= ct->image_count || cwm->metal_images[index] == nil || present_queue == NULL) {
		macos_retire_unpresented_job(cwm, &job, "invalid", 0);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	struct macos_present_job superseded_job;
	bool superseded = false;
	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (cwm->present_worker_shutdown) {
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		macos_retire_unpresented_job(cwm, &job, "shutdown_drop", 0);
		return VK_ERROR_DEVICE_LOST;
	}
	if (cwm->pending_present_job_valid) {
		superseded_job = cwm->pending_present_job;
		superseded = true;
		cwm->worker_jobs_superseded++;
	}
	cwm->pending_present_job = job;
	cwm->pending_present_job_valid = true;
	cwm->worker_jobs_enqueued++;
	if (!cwm->present_worker_scheduled) {
		cwm->present_worker_scheduled = true;
		dispatch_group_enter(cwm->present_worker_group);
		dispatch_async(cwm->present_worker_queue, ^{ macos_latest_present_worker_run_one(cwm); });
	}
	pthread_mutex_unlock(&cwm->present_worker_mutex);

	if (superseded) {
		macos_retire_unpresented_job(cwm, &superseded_job, "superseded_pending", 1);
	}
	uint64_t handoff_return_ns = os_monotonic_get_ns();
	macos_trace_present_worker(cwm, "latest_enqueued", &job, handoff_return_ns, handoff_return_ns, 0,
	                           0, 0, 0, 1,
	                           debug_get_bool_option_macos_metal_shared_event_wait() &&
	                               cwm->render_complete_event != nil);
	return VK_SUCCESS;
}

static void
comp_window_macos_destroy_latest(struct comp_target *ct)
{
	if (macos_latest_drawable_trace != NULL) {
		fflush(macos_latest_drawable_trace);
		fclose(macos_latest_drawable_trace);
		macos_latest_drawable_trace = NULL;
	}
	comp_window_macos_destroy(ct);
}

struct comp_target *
comp_window_macos_create(struct comp_compositor *c)
{
	struct comp_target *ct = comp_window_macos_create_legacy(c);
	if (ct == NULL) {
		return NULL;
	}

	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	bool want_latest = debug_get_bool_option_macos_present_latest_frame();
	if (want_latest && cwm->async_present && cwm->present_worker_enabled) {
		/* Enable consumption of the worker's already-acquired drawable. */
		cwm->early_drawable_enabled = true;
		ct->present = comp_window_macos_present_latest;
		ct->destroy = comp_window_macos_destroy_latest;
		if (debug_get_bool_option_macos_psvr2_timing_trace()) {
			macos_latest_drawable_trace = macos_timing_trace_open_file(
			    "latest_drawable",
			    "event,event_ns,drawable_ptr,acquire_begin_ns,acquire_end_ns,acquire_wait_ns,"
			    "initial_frame_id,selected_frame_id,superseded_frame_id,selected_timeline_value,"
			    "selected_image_index,selected_enqueue_ns,source_age_ns");
		}
		COMP_INFO(c,
		          "macOS diagnostic: newest-frame drawable worker enabled; nextDrawable blocks off-compositor and "
		          "binds to newest pending frame after acquisition");
	} else if (want_latest) {
		COMP_WARN(c,
		          "XRT_MACOS_PRESENT_LATEST_FRAME requires XRT_MACOS_ASYNC_PRESENT=1 and "
		          "XRT_MACOS_PRESENT_WORKER=1; using legacy presentation path");
	}
	return ct;
}

static bool
create_target_latest(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	(void)ctf;
	struct comp_target *ct = comp_window_macos_create(c);
	if (ct == NULL) {
		return false;
	}
	*out_ct = ct;
	return true;
}

const struct comp_target_factory comp_target_factory_macos = {
	.name = "macOS Metal Window",
	.identifier = "macos",
	.requires_vulkan_for_create = false,
	.is_deferred = false,
	.required_instance_version = 0,
	.required_instance_extensions = NULL,
	.required_instance_extension_count = 0,
	.optional_device_extensions = macos_optional_device_extensions,
	.optional_device_extension_count = ARRAY_SIZE(macos_optional_device_extensions),
	.detect = detect,
	.create_target = create_target_latest,
};

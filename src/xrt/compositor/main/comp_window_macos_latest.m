// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental stale-frame substitution for the macOS PS VR2 present worker.
 *
 * The proven legacy worker is intentionally preserved for ordinary frames. The
 * only behavioural change in the opt-in mode below happens after a blocking
 * CAMetalLayer nextDrawable call has already completed. If that acquisition took
 * at least 1.25 display refreshes and a newer compositor frame is pending, the stale
 * active source is retired and the acquired drawable is bound to the newer frame.
 */

/* Keep the existing implementation available as the legacy A/B path. */
#define comp_window_macos_create comp_window_macos_create_legacy
#define comp_target_factory_macos comp_target_factory_macos_legacy
#include "comp_window_macos.m"
#undef comp_target_factory_macos
#undef comp_window_macos_create

struct comp_target *
comp_window_macos_create(struct comp_compositor *c);
extern const struct comp_target_factory comp_target_factory_macos;

DEBUG_GET_ONCE_BOOL_OPTION(macos_present_stale_substitute, "XRT_MACOS_PRESENT_STALE_SUBSTITUTE", false)
DEBUG_GET_ONCE_BOOL_OPTION(macos_present_immediate, "XRT_MACOS_PRESENT_IMMEDIATE", false)

static FILE *macos_stale_substitute_trace = NULL;

static void
macos_trace_stale_substitute(uint64_t event_ns,
                             uint64_t drawable_wait_ns,
                             uint64_t threshold_ns,
                             const struct macos_present_job *old_job,
                             const struct macos_present_job *new_job)
{
	if (macos_stale_substitute_trace == NULL) {
		return;
	}
	uint64_t new_source_age_ns =
	    event_ns > new_job->enqueue_ns ? event_ns - new_job->enqueue_ns : 0;
	flockfile(macos_stale_substitute_trace);
	fprintf(macos_stale_substitute_trace,
	        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%llu,%llu,%llu\n",
	        (unsigned long long)event_ns, (unsigned long long)drawable_wait_ns,
	        (unsigned long long)threshold_ns, (unsigned long long)old_job->frame_id,
	        (unsigned long long)new_job->frame_id, (unsigned long long)old_job->timeline_value,
	        (unsigned long long)new_job->timeline_value, old_job->image_index, new_job->image_index,
	        (unsigned long long)old_job->enqueue_ns, (unsigned long long)new_job->enqueue_ns,
	        (unsigned long long)new_source_age_ns);
	fflush(macos_stale_substitute_trace);
	funlockfile(macos_stale_substitute_trace);
}

static VkResult
macos_execute_present_job_stale(struct comp_window_macos *cwm,
                                const struct macos_present_job *job,
                                bool async_present)
{
	/*
	 * This specialised path is deliberately narrow. The stale substitution is
	 * only safe without a second CPU-side Vulkan wait when the render-complete
	 * timeline is exported as an MTLSharedEvent. Fall back to the legacy path for
	 * every other configuration.
	 */
	bool shared_event_wait = async_present && debug_get_bool_option_macos_metal_shared_event_wait() &&
	                         cwm->render_complete_event != nil;
	if (!async_present || !cwm->present_worker_enabled || !shared_event_wait || cwm->drawable_slot_enabled ||
	    cwm->early_drawable_enabled) {
		return macos_execute_present_job(cwm, job, async_present);
	}

	struct comp_target *ct = &cwm->base.base;
	struct macos_present_job active_job = *job;
	struct vk_bundle_queue *present_queue = active_job.present_queue;
	uint64_t frame_id = active_job.frame_id;
	uint32_t index = active_job.image_index;
	uint64_t timeline_semaphore_value = active_job.timeline_value;
	int64_t desired_present_time_ns = active_job.desired_present_time_ns;
	int64_t present_slop_ns = active_job.present_slop_ns;
	uint64_t image_reuse_wait_ns = active_job.image_reuse_wait_ns;
	uint64_t worker_start_ns = os_monotonic_get_ns();
	uint64_t next_drawable_begin_ns = 0;
	uint64_t after_drawable_ns = 0;
	uint64_t before_present_call_ns = 0;
	uint64_t after_present_call_ns = 0;
	uint64_t after_commit_ns = 0;
	uint64_t target_output_ns = 0;
	uint64_t metal_request_ns = 0;
	uint64_t host_call_ns = active_job.enqueue_ns;
	uint64_t before_vk_wait_ns = os_monotonic_get_ns();
	uint64_t after_vk_wait_ns = before_vk_wait_ns;
	const char *wait_mode = "metal_shared_event";
	double scheduled_present_host_s = 0.0;
	bool immediate_present = debug_get_bool_option_macos_present_immediate();

	macos_trace_present_worker(cwm, "worker_start", &active_job, worker_start_ns, 0, worker_start_ns,
	                           0, 0, 0, 0, true);
	assert(present_queue != NULL);
	if (index >= ct->image_count || cwm->metal_images[index] == nil) {
		macos_retire_unpresented_job(cwm, &active_job, "invalid", 0);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	@autoreleasepool {
		if (cwm->prefetched_drawable != nil) {
			macos_release_prefetched_drawable(cwm, "present_mismatch_release");
		}

		uint64_t drawable_trace_begin_ns = os_monotonic_get_ns();
		/*
		 * Trace before acquisition, but start the measured nextDrawable interval
		 * only after the trace write. This prevents stdio/flush latency from being
		 * mistaken for CAMetalLayer drawable starvation and from triggering stale
		 * substitution.
		 */
		macos_trace_present_worker(cwm, "drawable_trace_begin", &active_job, drawable_trace_begin_ns, 0,
		                           worker_start_ns, 0, 0, 0, 0, true);
		next_drawable_begin_ns = os_monotonic_get_ns();
		id<CAMetalDrawable> drawable = [cwm->metal_layer nextDrawable];
		after_drawable_ns = os_monotonic_get_ns();
		macos_trace_present_worker(cwm, "drawable_end", &active_job, after_drawable_ns, 0,
		                           worker_start_ns, next_drawable_begin_ns, after_drawable_ns, 0, 0, true);

		if (drawable == nil) {
			COMP_ERROR(ct->c, "Could not acquire a CAMetalDrawable");
			macos_retire_unpresented_job(cwm, &active_job, "drawable_error", 0);
			return VK_ERROR_OUT_OF_DATE_KHR;
		}

		uint64_t drawable_wait_ns = after_drawable_ns > next_drawable_begin_ns
		                                ? after_drawable_ns - next_drawable_begin_ns
		                                : 0;
		uint64_t stale_period_ns = cwm->display_period_ns > 0
		                               ? (uint64_t)cwm->display_period_ns
		                               : (uint64_t)ct->c->frame_interval_ns;
		uint64_t stale_threshold_ns = stale_period_ns + stale_period_ns / 4;
		struct macos_present_job stale_job;
		struct macos_present_job replacement_job;
		bool substitute = false;

		if (stale_threshold_ns > 0 && drawable_wait_ns >= stale_threshold_ns) {
			pthread_mutex_lock(&cwm->present_worker_mutex);
			if (!cwm->present_worker_shutdown && cwm->pending_present_job_valid &&
			    cwm->pending_present_job.frame_id > active_job.frame_id) {
				stale_job = active_job;
				replacement_job = cwm->pending_present_job;
				cwm->pending_present_job_valid = false;
				cwm->worker_jobs_superseded++;
				substitute = true;
			}
			pthread_mutex_unlock(&cwm->present_worker_mutex);
		}

		if (substitute) {
			uint64_t substitute_ns = os_monotonic_get_ns();
			macos_trace_stale_substitute(substitute_ns, drawable_wait_ns, stale_threshold_ns,
			                             &stale_job, &replacement_job);
			macos_trace_present_worker(cwm, "stale_substitute_old", &stale_job, substitute_ns, 0,
			                           worker_start_ns, next_drawable_begin_ns, after_drawable_ns, 0, 0, true);
			macos_trace_present_worker(cwm, "stale_substitute_new", &replacement_job, substitute_ns, 0,
			                           worker_start_ns, next_drawable_begin_ns, after_drawable_ns, 0, 0, true);

			/*
			 * Preserve the legacy worker's queue ordering. Retirement is queued on
			 * the same serial present-worker queue, so it runs immediately after this
			 * presentation job and before the next drawable acquisition. This avoids
			 * changing the useful off-compositor pacing seen in the legacy control.
			 */
			macos_retire_unpresented_job(cwm, &stale_job, "active_stale_substituted", 0);

			active_job = replacement_job;
			present_queue = active_job.present_queue;
			frame_id = active_job.frame_id;
			index = active_job.image_index;
			timeline_semaphore_value = active_job.timeline_value;
			desired_present_time_ns = active_job.desired_present_time_ns;
			present_slop_ns = active_job.present_slop_ns;
			image_reuse_wait_ns = active_job.image_reuse_wait_ns;
			host_call_ns = active_job.enqueue_ns;
		}

		if (index >= ct->image_count || cwm->metal_images[index] == nil || present_queue == NULL) {
			macos_retire_unpresented_job(cwm, &active_job, "substitute_invalid", 0);
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		id<MTLCommandBuffer> command_buffer = [cwm->present_queue commandBuffer];
		if (command_buffer == nil) {
			COMP_ERROR(ct->c, "Could not create a Metal presentation command buffer");
			macos_retire_unpresented_job(cwm, &active_job, "command_buffer_error", 0);
			return VK_ERROR_DEVICE_LOST;
		}

		[command_buffer encodeWaitForEvent:cwm->render_complete_event value:timeline_semaphore_value];
		id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
		if (blit == nil) {
			COMP_ERROR(ct->c, "Could not create Metal blit encoder");
			macos_retire_unpresented_job(cwm, &active_job, "blit_error", 0);
			return VK_ERROR_DEVICE_LOST;
		}

		MTLSize size = MTLSizeMake(ct->width, ct->height, 1);
		[blit copyFromTexture:cwm->metal_images[index]
		             sourceSlice:0
		             sourceLevel:0
		            sourceOrigin:MTLOriginMake(0, 0, 0)
		              sourceSize:size
		               toTexture:[drawable texture]
		        destinationSlice:0
		        destinationLevel:0
		       destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];

		before_present_call_ns = os_monotonic_get_ns();
		uint64_t latest_output_ns =
		    atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);
		int64_t min_lead_us = debug_get_num_option_macos_present_min_lead_us();
		if (min_lead_us < 0) {
			min_lead_us = 0;
		}
		uint64_t earliest_output_ns = before_present_call_ns + (uint64_t)min_lead_us * 1000ULL;
		if (desired_present_time_ns > 0 && (uint64_t)desired_present_time_ns > earliest_output_ns) {
			earliest_output_ns = (uint64_t)desired_present_time_ns;
		}
		target_output_ns = latest_output_ns;
		if (target_output_ns == 0) {
			target_output_ns = earliest_output_ns;
		} else if (cwm->display_period_ns > 0) {
			uint64_t period_ns = (uint64_t)cwm->display_period_ns;
			while (target_output_ns < earliest_output_ns) {
				target_output_ns += period_ns;
			}
		}

		if (cwm->presented_state != NULL) {
			struct macos_presented_state *presented_state = cwm->presented_state;
			macos_presented_state_retain(presented_state);
			FILE *trace_file = presented_state->trace_presented;
			uint64_t traced_frame_id = frame_id;
			int64_t traced_desired_present_ns = desired_present_time_ns;
			uint64_t traced_target_output_ns = target_output_ns;
			[drawable addPresentedHandler:^(id<MTLDrawable> presented_drawable) {
				double presented_time_s = [presented_drawable presentedTime];
				int64_t handler_ns = os_monotonic_get_ns();
				double host_frequency = CVGetHostClockFrequency();
				uint64_t current_host_ticks = CVGetCurrentHostTime();
				int64_t current_host_ns =
				    host_frequency > 0.0 ? (int64_t)llround((double)current_host_ticks * 1e9 / host_frequency) : 0;
				int64_t handler_offset_ns = handler_ns - current_host_ns;
				int64_t presented_host_ns =
				    presented_time_s > 0.0 ? (int64_t)llround(presented_time_s * (double)U_TIME_1S_IN_NS) : 0;
				int64_t presented_monotonic_ns =
				    presented_host_ns != 0 ? presented_host_ns + handler_offset_ns : 0;
				int64_t presented_minus_desired_ns =
				    presented_monotonic_ns != 0 ? presented_monotonic_ns - traced_desired_present_ns : 0;
				int64_t presented_minus_target_ns =
				    presented_monotonic_ns != 0 ? presented_monotonic_ns - (int64_t)traced_target_output_ns : 0;
				int64_t observed_present_offset_ns =
				    presented_monotonic_ns != 0 ? presented_monotonic_ns - traced_desired_present_ns : 0;
				if (observed_present_offset_ns > 0) {
					atomic_store_explicit(&presented_state->latest_observed_present_offset_ns,
					                      observed_present_offset_ns, memory_order_release);
					atomic_fetch_add_explicit(&presented_state->present_offset_sample_serial, 1,
					                          memory_order_release);
				}
				flockfile(trace_file);
				fprintf(trace_file,
				        "%llu,%" PRIi64 ",%" PRIi64 ",%llu,%.17g,%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 "\n",
				        (unsigned long long)traced_frame_id, handler_ns, traced_desired_present_ns,
				        (unsigned long long)traced_target_output_ns, presented_time_s, presented_monotonic_ns,
				        presented_minus_desired_ns, presented_minus_target_ns, observed_present_offset_ns);
				fflush(trace_file);
				funlockfile(trace_file);
				macos_presented_state_release(presented_state);
			}];
		}

		if (immediate_present) {
			/*
			 * Diagnostic A/B: keep the same target-output calculation and feedback,
			 * but let CAMetalLayer choose the next display opportunity instead of
			 * requesting a host-clock presentation time. A negative scheduled time in
			 * present.csv marks this path without changing the existing trace schema.
			 */
			metal_request_ns = os_monotonic_get_ns();
			scheduled_present_host_s = -1.0;
			[command_buffer presentDrawable:drawable];
		} else {
			int64_t prelatch_us = debug_get_num_option_macos_present_prelatch_us();
			if (prelatch_us < 0) {
				prelatch_us = 0;
			}
			uint64_t prelatch_ns = (uint64_t)prelatch_us * 1000ULL;
			metal_request_ns = target_output_ns > prelatch_ns ? target_output_ns - prelatch_ns : target_output_ns;
			scheduled_present_host_s = monotonic_ns_to_host_seconds(cwm, (int64_t)metal_request_ns);
			if (scheduled_present_host_s > 0.0) {
				[command_buffer presentDrawable:drawable atTime:scheduled_present_host_s];
			} else {
				[command_buffer presentDrawable:drawable];
			}
		}
		after_present_call_ns = os_monotonic_get_ns();

		uint64_t traced_frame_id = frame_id;
		uint32_t traced_index = index;
		uint64_t traced_timeline_value = timeline_semaphore_value;
		uint64_t commit_begin_ns = os_monotonic_get_ns();
		dispatch_group_t command_group = cwm->present_command_group;
		FILE *complete_trace = cwm->trace_present_complete;
		dispatch_group_enter(command_group);
		[command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed_buffer) {
			uint64_t completion_ns = os_monotonic_get_ns();
			MTLCommandBufferStatus status = [completed_buffer status];
			double completed_gpu_start_s = [completed_buffer GPUStartTime];
			double completed_gpu_end_s = [completed_buffer GPUEndTime];
			if (status == MTLCommandBufferStatusError) {
				COMP_ERROR(cwm->base.base.c, "Asynchronous Metal presentation failed: %s",
				           [[[completed_buffer error] localizedDescription] UTF8String]);
			}
			if (complete_trace != NULL) {
				flockfile(complete_trace);
				fprintf(complete_trace, "%llu,%llu,%u,%llu,%lu,%llu,%.17g,%.17g,%u\n",
				        (unsigned long long)traced_frame_id, (unsigned long long)completion_ns,
				        traced_index, (unsigned long long)traced_timeline_value, (unsigned long)status,
				        (unsigned long long)(completion_ns - commit_begin_ns), completed_gpu_start_s,
				        completed_gpu_end_s, 1u);
				funlockfile(complete_trace);
			}
			macos_release_source_image(cwm, traced_index);
			dispatch_group_leave(command_group);
		}];
		[command_buffer commit];
		after_commit_ns = os_monotonic_get_ns();

		macos_trace_present_worker(cwm, "submitted", &active_job, after_commit_ns, 0, worker_start_ns,
		                           next_drawable_begin_ns, after_drawable_ns, after_commit_ns, 0, true);

		pthread_mutex_lock(&cwm->present_worker_mutex);
		cwm->worker_jobs_submitted++;
		uint64_t worker_delay_ns =
		    worker_start_ns > active_job.enqueue_ns ? worker_start_ns - active_job.enqueue_ns : 0;
		cwm->worker_queue_delay_total_ns += worker_delay_ns;
		if (worker_delay_ns > cwm->worker_queue_delay_max_ns) {
			cwm->worker_queue_delay_max_ns = worker_delay_ns;
		}
		if (drawable_wait_ns > 5000000ULL) {
			cwm->worker_drawable_stalls++;
		}
		if (cwm->display_period_ns > 0 && drawable_wait_ns > (uint64_t)cwm->display_period_ns / 2) {
			cwm->worker_drawable_half_refresh_stalls++;
		}
		uint64_t submitted = cwm->worker_jobs_submitted;
		uint64_t superseded = cwm->worker_jobs_superseded;
		uint64_t half_refresh_stalls = cwm->worker_drawable_half_refresh_stalls;
		uint64_t stalls = cwm->worker_drawable_stalls;
		uint64_t average_delay_ns = submitted != 0 ? cwm->worker_queue_delay_total_ns / submitted : 0;
		uint64_t max_delay_ns = cwm->worker_queue_delay_max_ns;
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		if (submitted % 240 == 0) {
			COMP_INFO(ct->c,
			          "macOS present worker: submitted %llu, superseded %llu, drawable >half-refresh %llu, >5ms "
			          "%llu, queue delay avg %.3fms max %.3fms",
			          (unsigned long long)submitted, (unsigned long long)superseded,
			          (unsigned long long)half_refresh_stalls, (unsigned long long)stalls,
			          (double)average_delay_ns / 1000000.0,
			          (double)max_delay_ns / 1000000.0);
		}
	}

	if (cwm->trace_present != NULL) {
		uint64_t latest_output_ns = atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);
		flockfile(cwm->trace_present);
		fprintf(cwm->trace_present,
		        "%llu,%llu,%" PRIi64 ",%" PRIi64 ",%llu,%" PRIi64 ",%llu,%" PRIi64 ",%" PRIi64 ",%.17g,%" PRIi64 ",%u,%llu,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.17g,%.17g,%u,%u,%llu\n",
		        (unsigned long long)frame_id, (unsigned long long)host_call_ns, desired_present_time_ns,
		        desired_present_time_ns - (int64_t)host_call_ns, (unsigned long long)target_output_ns,
		        (int64_t)target_output_ns - desired_present_time_ns, (unsigned long long)metal_request_ns,
		        (int64_t)metal_request_ns - (int64_t)target_output_ns,
		        (int64_t)metal_request_ns - (int64_t)before_present_call_ns, scheduled_present_host_s,
		        present_slop_ns, index, (unsigned long long)timeline_semaphore_value, wait_mode,
		        (unsigned long long)after_vk_wait_ns, (unsigned long long)after_drawable_ns,
		        (unsigned long long)before_present_call_ns, (unsigned long long)after_present_call_ns,
		        (unsigned long long)after_commit_ns, (unsigned long long)after_commit_ns,
		        (unsigned long long)latest_output_ns, 0.0, 0.0, 1u, 1u,
		        (unsigned long long)image_reuse_wait_ns);
		cwm->trace_present_rows++;
		if (cwm->trace_present_rows % 256 == 0) {
			fflush(cwm->trace_present);
		}
		funlockfile(cwm->trace_present);
	}

	uint64_t now_ns = os_monotonic_get_ns();
	if (cwm->last_present_ns != 0 && now_ns > cwm->last_present_ns) {
		uint64_t interval_ns = now_ns - cwm->last_present_ns;
		if (cwm->display_period_ns <= 0 || interval_ns <= (uint64_t)cwm->display_period_ns * 4) {
			cwm->present_total_ns += interval_ns;
			cwm->present_sample_count++;
			if (cwm->present_min_ns == 0 || interval_ns < cwm->present_min_ns) {
				cwm->present_min_ns = interval_ns;
			}
			if (interval_ns > cwm->present_max_ns) {
				cwm->present_max_ns = interval_ns;
			}
			if (cwm->display_period_ns > 0 && interval_ns > (uint64_t)cwm->display_period_ns * 3 / 2) {
				cwm->present_missed_intervals++;
			}
		}
	}
	cwm->last_present_ns = now_ns;
	if (cwm->present_sample_count == 240) {
		double average_ms = (double)cwm->present_total_ns / (double)cwm->present_sample_count / 1000000.0;
		COMP_INFO(ct->c, "macOS present-worker completion cadence: average %.3fms, min %.3fms, max %.3fms, late %llu/240",
		          average_ms, (double)cwm->present_min_ns / 1000000.0,
		          (double)cwm->present_max_ns / 1000000.0,
		          (unsigned long long)cwm->present_missed_intervals);
		COMP_INFO(ct->c, "macOS presentation CPU waits: Vulkan %.3fms, drawable %.3fms, synchronous Metal 0.000ms",
		          (double)cwm->present_vk_wait_total_ns / 240.0 / 1000000.0,
		          (double)cwm->present_drawable_wait_total_ns / 240.0 / 1000000.0);
		cwm->present_sample_count = 0;
		cwm->present_total_ns = 0;
		cwm->present_min_ns = 0;
		cwm->present_max_ns = 0;
		cwm->present_missed_intervals = 0;
		cwm->present_vk_wait_total_ns = 0;
		cwm->present_drawable_wait_total_ns = 0;
		cwm->present_metal_wait_total_ns = 0;
	}
	return VK_SUCCESS;
}

static void
macos_present_worker_run_one_stale(struct comp_window_macos *cwm)
{
	struct macos_present_job job;
	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (cwm->present_worker_shutdown || !cwm->pending_present_job_valid) {
		cwm->present_worker_scheduled = false;
		pthread_mutex_unlock(&cwm->present_worker_mutex);
		dispatch_group_leave(cwm->present_worker_group);
		return;
	}
	job = cwm->pending_present_job;
	cwm->pending_present_job_valid = false;
	pthread_mutex_unlock(&cwm->present_worker_mutex);

	(void)macos_execute_present_job_stale(cwm, &job, true);

	pthread_mutex_lock(&cwm->present_worker_mutex);
	if (!cwm->present_worker_shutdown && cwm->pending_present_job_valid) {
		dispatch_async(cwm->present_worker_queue, ^{ macos_present_worker_run_one_stale(cwm); });
	} else {
		cwm->present_worker_scheduled = false;
		dispatch_group_leave(cwm->present_worker_group);
	}
	pthread_mutex_unlock(&cwm->present_worker_mutex);
}

static VkResult
comp_window_macos_present_stale(struct comp_target *ct,
                                struct vk_bundle_queue *present_queue,
                                uint32_t index,
                                uint64_t timeline_semaphore_value,
                                int64_t desired_present_time_ns,
                                int64_t present_slop_ns)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	if (!cwm->async_present || !cwm->present_worker_enabled || cwm->render_complete_event == nil ||
	    !debug_get_bool_option_macos_present_stale_substitute()) {
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
		dispatch_async(cwm->present_worker_queue, ^{ macos_present_worker_run_one_stale(cwm); });
	}
	pthread_mutex_unlock(&cwm->present_worker_mutex);

	if (superseded) {
		macos_retire_unpresented_job(cwm, &superseded_job, "superseded", 1);
	}
	uint64_t handoff_return_ns = os_monotonic_get_ns();
	macos_trace_present_worker(cwm, "enqueued", &job, handoff_return_ns, handoff_return_ns, 0, 0, 0, 0, 1, true);
	return VK_SUCCESS;
}

static void
comp_window_macos_destroy_stale(struct comp_target *ct)
{
	comp_window_macos_destroy(ct);
	if (macos_stale_substitute_trace != NULL) {
		fflush(macos_stale_substitute_trace);
		fclose(macos_stale_substitute_trace);
		macos_stale_substitute_trace = NULL;
	}
}

struct comp_target *
comp_window_macos_create(struct comp_compositor *c)
{
	struct comp_target *ct = comp_window_macos_create_legacy(c);
	if (ct == NULL) {
		return NULL;
	}

	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	bool want_stale_substitute = debug_get_bool_option_macos_present_stale_substitute();
	bool want_immediate_present = debug_get_bool_option_macos_present_immediate();
	if (want_stale_substitute && cwm->async_present && cwm->present_worker_enabled) {
		ct->present = comp_window_macos_present_stale;
		ct->destroy = comp_window_macos_destroy_stale;
		if (debug_get_bool_option_macos_psvr2_timing_trace()) {
			macos_stale_substitute_trace = macos_timing_trace_open_file(
			    "stale_substitute",
			    "event_ns,drawable_wait_ns,threshold_ns,old_frame_id,new_frame_id,old_timeline_value,"
			    "new_timeline_value,old_image_index,new_image_index,old_enqueue_ns,new_enqueue_ns,new_source_age_ns");
		}
		COMP_INFO(c,
		          "macOS diagnostic: legacy present-worker stale substitution enabled; normal drawable waits are "
		          "unchanged and active frames are replaced only after waits of at least 1.25 refreshes");
		if (want_immediate_present) {
			COMP_INFO(c,
			          "macOS diagnostic: immediate Metal presentation enabled; using presentDrawable: instead of "
			          "presentDrawable:atTime: on the stale-substitution path");
		}
	} else if (want_stale_substitute) {
		COMP_WARN(c,
		          "XRT_MACOS_PRESENT_STALE_SUBSTITUTE requires XRT_MACOS_ASYNC_PRESENT=1 and "
		          "XRT_MACOS_PRESENT_WORKER=1; using legacy presentation path");
	}
	if (want_immediate_present && !(want_stale_substitute && cwm->async_present && cwm->present_worker_enabled)) {
		COMP_WARN(c,
		          "XRT_MACOS_PRESENT_IMMEDIATE is currently a stale-substitution diagnostic and requires "
		          "XRT_MACOS_PRESENT_STALE_SUBSTITUTE=1, XRT_MACOS_ASYNC_PRESENT=1 and XRT_MACOS_PRESENT_WORKER=1; "
		          "using the legacy scheduled presentation path");
	}
	return ct;
}

static bool
create_target_stale(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
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
	.create_target = create_target_stale,
};
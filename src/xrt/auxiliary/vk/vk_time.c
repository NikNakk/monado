// Copyright 2022, Collabora, Ltd.
// Copyright 2024-2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vulkan timestamp helpers.
 *
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup aux_vk
 */

#include "os/os_time.h"
#include "math/m_mathinclude.h"

#include "vk/vk_helpers.h"


#if defined(VK_EXT_calibrated_timestamps) || defined(VK_KHR_calibrated_timestamps)

#if defined(XRT_OS_LINUX) || defined(XRT_OS_OSX)
#define CPU_TIME_DOMAIN VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT
#elif defined(XRT_OS_WINDOWS)
#define CPU_TIME_DOMAIN VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_EXT
#else
#error "Vulkan timestamp domain needs porting"
#endif

/*
 *
 * Helper(s)
 *
 */

uint64_t
from_host_ticks_to_host_ns(uint64_t ticks)
{
#if defined(XRT_OS_LINUX) || defined(XRT_OS_OSX)

	// No-op on Linux.
	return ticks;

#elif defined(XRT_OS_WINDOWS)

	return ticks * os_ns_per_qpc_tick_get();

#else
#error "Vulkan timestamp domain needs porting"
#endif
}

static VkResult
get_calibrated_timestamps(struct vk_bundle *vk,
                          uint32_t timestamp_count,
                          const VkCalibratedTimestampInfoEXT *timestamp_infos,
                          uint64_t *timestamps)
{
	uint64_t max_deviation;
	VkResult ret = VK_ERROR_EXTENSION_NOT_PRESENT;

#ifdef VK_KHR_calibrated_timestamps
	if (vk->has_KHR_calibrated_timestamps) {
		assert(vk->vkGetCalibratedTimestampsKHR != NULL);
		ret = vk->vkGetCalibratedTimestampsKHR( //
		    vk->device,                         // device
		    timestamp_count,                    // timestampCount
		    timestamp_infos,                    // pTimestampInfos
		    timestamps,                         // pTimestamps
		    &max_deviation);                    // pMaxDeviation
		VK_CHK_AND_RET(ret, "vkGetCalibratedTimestampsKHR");
	} else
#endif
#ifdef VK_EXT_calibrated_timestamps
	    if (vk->has_EXT_calibrated_timestamps) {
		assert(vk->vkGetCalibratedTimestampsEXT != NULL);
		ret = vk->vkGetCalibratedTimestampsEXT( //
		    vk->device,                         // device
		    timestamp_count,                    // timestampCount
		    timestamp_infos,                    // pTimestampInfos
		    timestamps,                         // pTimestamps
		    &max_deviation);                    // pMaxDeviation
		VK_CHK_AND_RET(ret, "vkGetCalibratedTimestampsEXT");
	} else
#endif
	{
		VK_ERROR(vk, "VK_{EXT,KHR}_calibrated_timestamps not enabled");
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	}

	return ret;
}

XRT_CHECK_RESULT static VkResult
get_calibrated_timestamps_other_and_cpu(struct vk_bundle *vk,
                                        VkTimeDomainEXT other_domain,
                                        const void *other_next,
                                        uint64_t out_timestamps[2])
{
	const VkCalibratedTimestampInfoEXT timestamp_info[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
	        .pNext = other_next,
	        .timeDomain = other_domain,
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT,
	        .pNext = NULL,
	        .timeDomain = CPU_TIME_DOMAIN,
	    },
	};

	// Skip error reporting here, it's done by callers.
	return get_calibrated_timestamps(vk, 2, timestamp_info, out_timestamps);
}

static void
convert_timestamps(struct vk_bundle *vk,
                   uint64_t source_calib_ns,
                   uint64_t target_calib_ns,
                   uint32_t count,
                   uint64_t *in_out_timestamps)
{
	for (uint32_t i = 0; i < count; i++) {
		// Read the source domain timestamp.
		uint64_t timestamp_source_ns = in_out_timestamps[i];

		// Calculate the target domain timestamp.
		uint64_t timestamp_target_ns = timestamp_source_ns + target_calib_ns - source_calib_ns;

		// Write out the result.
		in_out_timestamps[i] = timestamp_target_ns;
	}
}


/*
 *
 * 'Exported' function(s).
 *
 */

XRT_CHECK_RESULT VkResult
vk_convert_device_timestamps_to_host_ns(struct vk_bundle *vk, uint32_t count, uint64_t *in_out_timestamps)
{
	VkResult ret;

	uint64_t timestamps[2];
	ret = get_calibrated_timestamps_other_and_cpu(vk, VK_TIME_DOMAIN_DEVICE_EXT, NULL, timestamps);
	VK_CHK_AND_RET(ret, "get_calibrated_timestamps_other_and_cpu");

	uint64_t now_ticks = timestamps[0];
	uint64_t now_ns = from_host_ticks_to_host_ns(timestamps[1]);

	// Yupp it's floating point.
	double period = (double)vk->features.timestamp_period;
	uint64_t shift = vk->features.timestamp_valid_bits;

	for (uint32_t i = 0; i < count; i++) {
		// Read the GPU domain timestamp.
		uint64_t timestamp_gpu_ticks = in_out_timestamps[i];

		// The GPU timestamp has rolled over, move now to new epoch.
		if (timestamp_gpu_ticks > now_ticks) {
			/*
			 * If there are 64 bits of timestamps data then this
			 * code doesn't work, but we shouldn't be here then.
			 */
			assert(vk->features.timestamp_valid_bits < 64);
			now_ticks += (uint64_t)1 << shift;
		}
		assert(now_ticks > timestamp_gpu_ticks);

		/*
		 * Since these two timestamps should be close to each other and
		 * therefore a small value a double floating point value should
		 * be able to hold the value "safely".
		 */
		double diff_ticks_f = (double)(now_ticks - timestamp_gpu_ticks);

		// Convert into nanoseconds.
		int64_t diff_ns = (int64_t)floor(diff_ticks_f * period + 0.5);
		assert(diff_ns > 0);

		// And with the diff we can get the timestamp.
		uint64_t timestamp_ns = now_ns - (uint64_t)diff_ns;

		// Write out the result.
		in_out_timestamps[i] = timestamp_ns;
	}

	return VK_SUCCESS;
}

XRT_CHECK_RESULT VkResult
vk_convert_timestamps_to_host(struct vk_bundle *vk,
                              VkTimeDomainEXT source_domain,
                              uint32_t count,
                              uint64_t *in_out_timestamps)
{
	VkResult ret;

	uint64_t timestamps[2];
	ret = get_calibrated_timestamps_other_and_cpu(vk, source_domain, NULL, timestamps);
	VK_CHK_AND_RET(ret, "get_calibrated_timestamps_other_and_cpu");

	uint64_t other_ns = timestamps[0];
	uint64_t host_ns = from_host_ticks_to_host_ns(timestamps[1]);
	convert_timestamps(vk, other_ns, host_ns, count, in_out_timestamps);

	return VK_SUCCESS;
}

XRT_CHECK_RESULT VkResult
vk_convert_timestamps_from_host(struct vk_bundle *vk,
                                VkTimeDomainEXT target_domain,
                                uint32_t count,
                                uint64_t *in_out_timestamps)
{
	VkResult ret;

	uint64_t timestamps[2];
	ret = get_calibrated_timestamps_other_and_cpu(vk, target_domain, NULL, timestamps);
	VK_CHK_AND_RET(ret, "get_calibrated_timestamps_other_and_cpu");

	uint64_t other_ns = timestamps[0];
	uint64_t host_ns = from_host_ticks_to_host_ns(timestamps[1]);
	convert_timestamps(vk, host_ns, other_ns, count, in_out_timestamps);

	return VK_SUCCESS;
}

#ifdef VK_EXT_present_timing

XRT_CHECK_RESULT VkResult
vk_convert_swapchain_timestamps_to_host(struct vk_bundle *vk,
                                        VkTimeDomainEXT source_domain,
                                        const VkSwapchainCalibratedTimestampInfoEXT *swapchain,
                                        uint32_t count,
                                        uint64_t *in_out_timestamps)
{
	VkResult ret;

	uint64_t timestamps[2];
	ret = get_calibrated_timestamps_other_and_cpu(vk, source_domain, swapchain, timestamps);
	VK_CHK_AND_RET(ret, "get_calibrated_timestamps_other_and_cpu");

	uint64_t swapchain_ns = timestamps[0];
	uint64_t host_ns = from_host_ticks_to_host_ns(timestamps[1]);
	convert_timestamps(vk, swapchain_ns, host_ns, count, in_out_timestamps);

	return VK_SUCCESS;
}

XRT_CHECK_RESULT VkResult
vk_convert_host_timestamps_to_swapchain(struct vk_bundle *vk,
                                        VkTimeDomainEXT target_domain,
                                        const VkSwapchainCalibratedTimestampInfoEXT *swapchain,
                                        uint32_t count,
                                        uint64_t *in_out_timestamps)
{
	VkResult ret;

	uint64_t timestamps[2];
	ret = get_calibrated_timestamps_other_and_cpu(vk, target_domain, swapchain, timestamps);
	VK_CHK_AND_RET(ret, "get_calibrated_timestamps_other_and_cpu");

	uint64_t swapchain_ns = timestamps[0];
	uint64_t host_ns = from_host_ticks_to_host_ns(timestamps[1]);
	convert_timestamps(vk, host_ns, swapchain_ns, count, in_out_timestamps);

	return VK_SUCCESS;
}

#endif // defined(VK_EXT_present_timing)

#endif /* defined(VK_EXT_calibrated_timestamps) || defined(VK_KHR_calibrated_timestamps) */

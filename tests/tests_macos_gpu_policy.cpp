// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "multi/comp_multi_private.h"
#include "util/comp_semaphore.h"
#include "util/comp_swapchain.h"
#include "util/comp_swapchain_gpu_reuse.h"
#include "util/u_time.h"
#include "xrt/xrt_device.h"

#include <map>

namespace {
std::map<VkSemaphore, uint64_t> values;
uint64_t handles = 100;
int destroyed;
using NativeSwapchain = struct comp_swapchain;

// Minimal fake Vulkan device: timeline semaphores and image sizes only.
struct Device
{
	vk_bundle vk{};
	Device()
	{
		vk.features.timeline_semaphore = true;
		vk.has_EXT_metal_objects = true;
		vk.vkCreateSemaphore = [](VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *,
		                          VkSemaphore *out) {
			*out = reinterpret_cast<VkSemaphore>(++handles);
			values[*out] = 0;
			return VK_SUCCESS;
		};
		vk.vkDestroySemaphore = [](VkDevice, VkSemaphore sem, const VkAllocationCallbacks *) {
			values.erase(sem);
		};
		vk.vkWaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *wait, uint64_t) {
			for (uint32_t i = 0; i < wait->semaphoreCount; ++i)
				if (values[wait->pSemaphores[i]] < wait->pValues[i])
					return VK_TIMEOUT;
			return VK_SUCCESS;
		};
		vk.vkGetImageMemoryRequirements = [](VkDevice, VkImage, VkMemoryRequirements *req) {
			req->size = 16384;
		};
		vk.log_level = U_LOGGING_ERROR;
	}
	~Device()
	{
		comp_swapchain_gpu_reuse_collect(&vk, true);
	}
};
} // namespace

TEST_CASE("late completed frame retains its slot until a later readiness cutoff")
{
	struct multi_system_compositor system{};
	xrt_compositor_native native{};
	os_mutex_init(&system.list_and_timing_lock);
	os_thread_helper_init(&system.oth);
	u_pa_factory_create(&system.upaf);
	system.xcn = &native;
	system.last_timings.predicted_display_time_ns = os_monotonic_get_ns();
	system.last_timings.predicted_display_period_ns = 16000000;
	system.last_timings.diff_ns = 5000000;
	xrt_session_info info{};
	xrt_compositor_native *client = nullptr;
	REQUIRE(multi_compositor_create(&system, &info, nullptr, &client) == XRT_SUCCESS);
	auto *mc = multi_compositor(&client->base);
	int64_t frame, wake, gpu, display, period;
	REQUIRE(xrt_comp_predict_frame(&client->base, &frame, &wake, &gpu, &display, &period) == XRT_SUCCESS);
	REQUIRE(xrt_comp_mark_frame(&client->base, frame, XRT_COMPOSITOR_FRAME_POINT_WOKE, os_monotonic_get_ns()) ==
	        XRT_SUCCESS);
	REQUIRE(xrt_comp_begin_frame(&client->base, frame) == XRT_SUCCESS);
	xrt_layer_frame_data data{};
	data.frame_id = frame;
	int64_t now = os_monotonic_get_ns();
	data.display_time_ns = now;
	REQUIRE(xrt_comp_layer_begin(&client->base, &data) == XRT_SUCCESS);
	REQUIRE(xrt_comp_layer_commit(&client->base, XRT_GRAPHICS_SYNC_HANDLE_INVALID) == XRT_SUCCESS);
	os_mutex_lock(&mc->slot_lock);
	mc->scheduled.gpu_ready_ns = now + 10 * U_TIME_1MS_IN_NS;
	os_mutex_unlock(&mc->slot_lock);
	multi_compositor_deliver_any_frames(mc, now + 12 * U_TIME_1MS_IN_NS);
	CHECK(mc->scheduled.active);
	CHECK(mc->scheduled.data.frame_id == frame);
	CHECK_FALSE(mc->delivered.active);
	multi_compositor_deliver_any_frames(mc, now + 20 * U_TIME_1MS_IN_NS);
	CHECK_FALSE(mc->scheduled.active);
	CHECK(mc->delivered.active);
	CHECK(mc->delivered.data.frame_id == frame);
	xrt_comp_native_destroy(&client);
	u_paf_destroy(&system.upaf);
	os_thread_helper_destroy(&system.oth);
	os_mutex_destroy(&system.list_and_timing_lock);
}

TEST_CASE("rejected producer dependency drops the frame and releases its claims")
{
	struct multi_system_compositor system{};
	xrt_compositor_native native{};
	os_mutex_init(&system.list_and_timing_lock);
	os_thread_helper_init(&system.oth);
	u_pa_factory_create(&system.upaf);
	system.xcn = &native;
	system.last_timings.predicted_display_time_ns = os_monotonic_get_ns();
	system.last_timings.predicted_display_period_ns = 16000000;
	xrt_session_info info{};
	xrt_compositor_native *client = nullptr;
	REQUIRE(multi_compositor_create(&system, &info, nullptr, &client) == XRT_SUCCESS);
	auto *mc = multi_compositor(&client->base);

	Device device;
	Device other;
	auto *sc = new NativeSwapchain{};
	sc->vk = &device.vk;
	sc->vkic.image_count = 1;
	sc->base.base.image_count = 1;
	sc->base.base.reference.count = 1;
	sc->base.base.wait_image = [](xrt_swapchain *, int64_t, uint32_t) { return XRT_SUCCESS; };
	sc->base.base.destroy = [](xrt_swapchain *base) {
		destroyed++;
		delete reinterpret_cast<NativeSwapchain *>(base);
	};
	REQUIRE(comp_swapchain_gpu_reuse_enable(&sc->base.base) == XRT_SUCCESS);
	xrt_swapchain *xsc = &sc->base.base;

	// A semaphore from another device can never be a dependency of this swapchain.
	xrt_compositor_semaphore *foreign = nullptr;
	REQUIRE(comp_semaphore_import_metal_shared_event(&other.vk, reinterpret_cast<void *>(1), 0, &foreign) ==
	        XRT_SUCCESS);

	xrt_device xdev{};
	for (int round = 0; round < 2; round++) {
		int64_t frame, wake, gpu, display, period;
		REQUIRE(xrt_comp_predict_frame(&client->base, &frame, &wake, &gpu, &display, &period) == XRT_SUCCESS);
		REQUIRE(xrt_comp_mark_frame(&client->base, frame, XRT_COMPOSITOR_FRAME_POINT_WOKE,
		                            os_monotonic_get_ns()) == XRT_SUCCESS);
		REQUIRE(xrt_comp_begin_frame(&client->base, frame) == XRT_SUCCESS);
		xrt_layer_frame_data data{};
		data.frame_id = frame;
		data.display_time_ns = display;
		REQUIRE(xrt_comp_layer_begin(&client->base, &data) == XRT_SUCCESS);
		xrt_layer_data quad{};
		quad.type = XRT_LAYER_QUAD;
		REQUIRE(xrt_comp_layer_quad(&client->base, &xdev, xsc, &quad) == XRT_SUCCESS);
		CHECK(xsc->reference.count == 2);
		CHECK(xrt_comp_layer_commit_with_semaphore(&client->base, foreign, 1) == XRT_ERROR_VULKAN);
		CHECK(mc->progress.layer_count == 0);
		CHECK_FALSE(mc->progress.active);
		CHECK(xsc->reference.count == 1);
	}

	// No consumer claim or producer obligation is left: reclaim is immediate.
	int before = destroyed;
	xrt_swapchain_reference(&xsc, nullptr);
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	CHECK(destroyed == before + 1);

	xrt_compositor_semaphore_reference(&foreign, nullptr);
	xrt_comp_native_destroy(&client);
	u_paf_destroy(&system.upaf);
	os_thread_helper_destroy(&system.oth);
	os_mutex_destroy(&system.list_and_timing_lock);
}

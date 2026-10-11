// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "util/comp_swapchain.h"
#include "util/comp_swapchain_gpu_reuse.h"
#include "util/comp_swapchain_gpu_reuse_internal.h"
#include "util/comp_semaphore.h"
#include <map>
#include <vector>

namespace {
std::map<VkSemaphore, uint64_t> values;
uint64_t handles = 100;
int destroyed;
std::vector<uint64_t> timeouts;
using NativeSwapchain = struct comp_swapchain;
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
		vk.vkWaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *wait, uint64_t timeout) {
			timeouts.push_back(timeout);
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
	NativeSwapchain *
	swapchain()
	{
		auto *sc = new NativeSwapchain{};
		sc->vk = &vk;
		sc->vkic.image_count = 1;
		sc->base.base.image_count = 1;
		sc->base.base.reference.count = 1;
		sc->base.base.wait_image = [](xrt_swapchain *, int64_t, uint32_t) { return XRT_SUCCESS; };
		sc->base.base.destroy = [](xrt_swapchain *base) {
			destroyed++;
			delete reinterpret_cast<NativeSwapchain *>(base);
		};
		REQUIRE(comp_swapchain_gpu_reuse_enable(&sc->base.base) == XRT_SUCCESS);
		return sc;
	}
	xrt_compositor_semaphore *
	producer()
	{
		xrt_compositor_semaphore *sem = nullptr;
		REQUIRE(comp_semaphore_import_metal_shared_event(&vk, reinterpret_cast<void *>(1), 0, &sem) ==
		        XRT_SUCCESS);
		return sem;
	}
};
} // namespace
TEST_CASE("producer and consumer obligations quarantine independently and never wait at teardown")
{
	Device device;
	auto *sc = device.swapchain();
	auto *sem = device.producer();
	auto *base = &sc->base.base;
	REQUIRE(comp_swapchain_gpu_reuse_set_producer(base, 0, sem, 7) == XRT_SUCCESS);
	auto signal = comp_semaphore(sem)->semaphore;
	CHECK(xrt_swapchain_wait_image(base, 0, 0) == XRT_TIMEOUT);
	int before = destroyed;
	timeouts.clear();
	xrt_swapchain_reference(&base, nullptr);
	device.vk.vkWaitSemaphores = nullptr; // destroy itself must not call Vulkan or block
	CHECK(destroyed == before);
	device.vk.vkWaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *wait, uint64_t timeout) {
		timeouts.push_back(timeout);
		return values[wait->pSemaphores[0]] >= wait->pValues[0] ? VK_SUCCESS : VK_TIMEOUT;
	};
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	CHECK(destroyed == before);
	values[signal] = 7;
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	CHECK(destroyed == before + 1);
	for (auto timeout : timeouts)
		CHECK(timeout == 0);
	xrt_compositor_semaphore_reference(&sem, nullptr);
}
TEST_CASE("renderer preserves already satisfied producer wait and rejects future values")
{
	Device device;
	auto *sc = device.swapchain();
	auto *sem = device.producer();
	auto *base = &sc->base.base;
	REQUIRE(comp_swapchain_gpu_reuse_set_producer(base, 0, sem, 11) == XRT_SUCCESS);
	comp_layer_accum accum{};
	accum.layer_count = 1;
	accum.layers[0].sc_array[0] = base;
	accum.layers[0].data.type = XRT_LAYER_QUAD;
	accum.layers[0].data.quad.sub.image_index = 0;
	VkCommandBuffer cmd{};
	vk_submit_info_builder builder{};
	comp_swapchain_gpu_reuse_renderer_enter(&accum);
	comp_swapchain_gpu_reuse_submit_info_builder_prepare(&builder, nullptr, &cmd, 1, nullptr, nullptr);
	vk_bundle_queue queue{};
	CHECK(comp_swapchain_gpu_reuse_vk_cmd_submit_locked(&device.vk, &queue, 1, &builder.submit_info,
	                                                    VK_NULL_HANDLE) == VK_NOT_READY);
	comp_swapchain_gpu_reuse_renderer_leave(&accum);
	values[comp_semaphore(sem)->semaphore] = 11;
	comp_swapchain_gpu_reuse_renderer_enter(&accum);
	comp_swapchain_gpu_reuse_submit_info_builder_prepare(&builder, nullptr, &cmd, 1, nullptr, nullptr);
	REQUIRE(builder.submit_info.waitSemaphoreCount == 1);
	CHECK(builder.submit_info.pWaitSemaphores[0] == comp_semaphore(sem)->semaphore);
	CHECK(builder.submit_info.pWaitDstStageMask[0] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	auto *timeline = static_cast<const VkTimelineSemaphoreSubmitInfo *>(builder.submit_info.pNext);
	REQUIRE(timeline != nullptr);
	CHECK(timeline->pWaitSemaphoreValues[0] == 11);
	// Abort the prepared submit: it must roll back consumer ownership rather than invent completion.
	comp_swapchain_gpu_reuse_renderer_leave(&accum);
	xrt_swapchain_reference(&base, nullptr);
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	xrt_compositor_semaphore_reference(&sem, nullptr);
}
TEST_CASE("pending consumer completion retains storage after producer completion")
{
	Device device;
	auto *sc = device.swapchain();
	auto *base = &sc->base.base;
	comp_layer_accum accum{};
	accum.layer_count = 1;
	accum.layers[0].sc_array[0] = base;
	accum.layers[0].data.type = XRT_LAYER_QUAD;
	comp_swapchain_gpu_reuse_renderer_enter(&accum);
	VkCommandBuffer cmd{};
	vk_submit_info_builder builder{};
	comp_swapchain_gpu_reuse_submit_info_builder_prepare(&builder, nullptr, &cmd, 1, nullptr, nullptr);
	auto *timeline = static_cast<const VkTimelineSemaphoreSubmitInfo *>(builder.submit_info.pNext);
	REQUIRE(timeline != nullptr);
	VkSemaphore consumer = builder.submit_info.pSignalSemaphores[0];
	uint64_t value = timeline->pSignalSemaphoreValues[0];
	// Emulate an accepted Vulkan submission, with a not-yet-completed consumer timeline.
	vk_bundle_queue queue{};
	device.vk.vkQueueSubmit = [](VkQueue, uint32_t, const VkSubmitInfo *, VkFence) { return VK_SUCCESS; };
	os_mutex_init(&queue.mutex);
	CHECK(comp_swapchain_gpu_reuse_vk_cmd_submit_locked(&device.vk, &queue, 1, &builder.submit_info,
	                                                    VK_NULL_HANDLE) == VK_SUCCESS);
	os_mutex_destroy(&queue.mutex);
	comp_swapchain_gpu_reuse_renderer_leave(&accum);
	timeouts.clear();
	CHECK(xrt_swapchain_wait_image(base, 5000000, 0) == XRT_TIMEOUT);
	REQUIRE_FALSE(timeouts.empty());
	for (uint64_t timeout : timeouts) CHECK(timeout <= 5000000ULL);
	// An infinite wait reaches Vulkan unclamped; the IPC worker bounds its own calls.
	timeouts.clear();
	CHECK(xrt_swapchain_wait_image(base, INT64_MAX, 0) == XRT_TIMEOUT);
	REQUIRE_FALSE(timeouts.empty());
	CHECK(timeouts.back() == (uint64_t)INT64_MAX);

	int before = destroyed;
	xrt_swapchain_reference(&base, nullptr);
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	CHECK(destroyed == before);
	values[consumer] = value;
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	CHECK(destroyed == before + 1);
}
TEST_CASE("oversized storage is rejected before tracking ownership")
{
	Device device;
	NativeSwapchain sc{};
	sc.vk = &device.vk;
	sc.vkic.image_count = 1;
	sc.base.base.image_count = 1;
	sc.base.base.wait_image = [](xrt_swapchain *, int64_t, uint32_t) { return XRT_SUCCESS; };
	sc.base.base.destroy = [](xrt_swapchain *) {};
	device.vk.vkGetImageMemoryRequirements = [](VkDevice, VkImage, VkMemoryRequirements *req) {
		req->size = UINT64_C(2) * 1024 * 1024 * 1024;
	};
	CHECK(comp_swapchain_gpu_reuse_enable(&sc.base.base) == XRT_ERROR_ALLOCATION);
}
TEST_CASE("repeated failed teardown is bounded and completion restores allocation capacity")
{
	Device device;
	auto *sem = device.producer();
	int before = destroyed;
	for (unsigned i = 0; i < 256; i++) {
		auto *sc = device.swapchain();
		auto *base = &sc->base.base;
		REQUIRE(comp_swapchain_gpu_reuse_set_producer(base, 0, sem, 23) == XRT_SUCCESS);
		xrt_swapchain_reference(&base, nullptr);
	}
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	CHECK(destroyed == before);
	NativeSwapchain extra{};
	extra.vk = &device.vk;
	extra.vkic.image_count = 1;
	extra.base.base.image_count = 1;
	extra.base.base.wait_image = [](xrt_swapchain *, int64_t, uint32_t) { return XRT_SUCCESS; };
	extra.base.base.destroy = [](xrt_swapchain *) {};
	CHECK(comp_swapchain_gpu_reuse_enable(&extra.base.base) == XRT_ERROR_ALLOCATION);
	values[comp_semaphore(sem)->semaphore] = 23;
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	CHECK(destroyed == before + 256);
	auto *fresh = device.swapchain();
	auto *base = &fresh->base.base;
	xrt_swapchain_reference(&base, nullptr);
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	CHECK(destroyed == before + 257);
	xrt_compositor_semaphore_reference(&sem, nullptr);
}
TEST_CASE("pre-import client budgets retain failed storage and recover only on completion")
{
	Device device;
	auto *budget = comp_gpu_client_budget_create();
	auto *producer = device.producer();
	xrt_swapchain_create_info info{};
	info.width = info.height = 64;
	info.array_size = info.face_count = info.sample_count = info.mip_count = 1;
	info.format = VK_FORMAT_B8G8R8A8_UNORM;
	for (unsigned i = 0; i < 64; i++) {
		REQUIRE(comp_gpu_client_budget_reserve(budget, &info, 1, nullptr) == XRT_SUCCESS);
		auto *sc = device.swapchain();
		comp_gpu_client_budget_finish();
		auto *base = &sc->base.base;
		REQUIRE(comp_swapchain_gpu_reuse_set_producer(base, 0, producer, 91) == XRT_SUCCESS);
		xrt_swapchain_reference(&base, nullptr);
	}
	CHECK(comp_gpu_client_budget_reserve(budget, &info, 1, nullptr) == XRT_ERROR_ALLOCATION);
	comp_gpu_client_budget_destroy(&budget); // Retained trackers keep the owner account alive.
	auto *other = comp_gpu_client_budget_create();
	REQUIRE(comp_gpu_client_budget_reserve(other, &info, 1, nullptr) == XRT_SUCCESS);
	comp_gpu_client_budget_finish();
	comp_gpu_client_budget_destroy(&other);
	values[comp_semaphore(producer)->semaphore] = 91;
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	xrt_compositor_semaphore_reference(&producer, nullptr);
}
TEST_CASE("pre-import overflow rejection does not consume allocation capacity")
{
	auto *budget = comp_gpu_client_budget_create();
	xrt_swapchain_create_info info{};
	info.width = info.height = UINT32_MAX;
	info.array_size = info.face_count = info.sample_count = info.mip_count = 1;
	CHECK(comp_gpu_client_budget_reserve(budget, &info, 3, nullptr) == XRT_ERROR_ALLOCATION);
	info.width = info.height = 64;
	REQUIRE(comp_gpu_client_budget_reserve(budget, &info, 3, nullptr) == XRT_SUCCESS);
	comp_gpu_client_budget_finish();
	comp_gpu_client_budget_destroy(&budget);
}

TEST_CASE("device loss does not authorize reuse or resource destruction")
{
	Device device;
	auto *sc = device.swapchain();
	auto *sem = device.producer();
	auto *base = &sc->base.base;
	REQUIRE(comp_swapchain_gpu_reuse_set_producer(base, 0, sem, 73) == XRT_SUCCESS);
	auto signal = comp_semaphore(sem)->semaphore;
	device.vk.vkWaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *, uint64_t timeout) {
		timeouts.push_back(timeout);
		return VK_ERROR_DEVICE_LOST;
	};
	CHECK(xrt_swapchain_wait_image(base, 0, 0) != XRT_SUCCESS);
	int before = destroyed;
	xrt_swapchain_reference(&base, nullptr);
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	CHECK(destroyed == before);
	CHECK(comp_swapchain_gpu_reuse_device_in_use(&device.vk));
	// Restore the mock only to clean up the test. Device loss itself proved nothing.
	device.vk.vkWaitSemaphores = [](VkDevice, const VkSemaphoreWaitInfo *wait, uint64_t) {
		return values[wait->pSemaphores[0]] >= wait->pValues[0] ? VK_SUCCESS : VK_TIMEOUT;
	};
	values[signal] = 73;
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	CHECK(destroyed == before + 1);
	xrt_compositor_semaphore_reference(&sem, nullptr);
}

TEST_CASE("an outstanding producer is waited for within the caller's timeout")
{
	Device device;
	auto *sc = device.swapchain();
	auto *sem = device.producer();
	auto *base = &sc->base.base;
	REQUIRE(comp_swapchain_gpu_reuse_set_producer(base, 0, sem, 5) == XRT_SUCCESS);
	timeouts.clear();
	CHECK(xrt_swapchain_wait_image(base, 20000000, 0) == XRT_TIMEOUT);
	// Polled once, then waited with the remaining timeout rather than returning at once.
	REQUIRE(timeouts.size() >= 2);
	CHECK(timeouts[0] == 0);
	CHECK(timeouts[1] > 0);
	CHECK(timeouts[1] <= 20000000ULL);
	values[comp_semaphore(sem)->semaphore] = 5;
	CHECK(xrt_swapchain_wait_image(base, 20000000, 0) == XRT_SUCCESS);
	xrt_swapchain_reference(&base, nullptr);
	comp_swapchain_gpu_reuse_collect(&device.vk, false);
	xrt_compositor_semaphore_reference(&sem, nullptr);
}


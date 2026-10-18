// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "render_compute_test_fixture.h"
#include "render/render_output.h"
#include "fullscreen.vert.h"
#include "output.frag.h"

namespace {
struct OutputFixture
{
	Fixture f;
	std::array<Image, 3> destinations{};
	std::array<VkSemaphore, 3> complete{};
	VkShaderModule vert{}, frag{};
	render_output *output{};
	VkShaderModule
	module(const uint32_t *code, size_t size)
	{
		VkShaderModuleCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		info.codeSize = size;
		info.pCode = code;
		VkShaderModule result{};
		REQUIRE(f.vk().vkCreateShaderModule(f.vk().device, &info, nullptr, &result) == VK_SUCCESS);
		return result;
	}
	void
	init(VkFormat format = VK_FORMAT_R8G8B8A8_UNORM)
	{
		f.init(67, 39);
		auto &vk = f.vk();
		vert = module(fullscreen_vert, sizeof(fullscreen_vert));
		frag = module(output_frag, sizeof(output_frag));
		VkImageView views[3];
		for (unsigned i = 0; i < 3; i++) {
			auto &dst = destinations[i];
			dst.extent = {2 * f.width, f.height};
			// Presentation images are render targets only: no STORAGE, SAMPLED or
			// TRANSFER_DST. TRANSFER_SRC is solely for this test's readback.
			REQUIRE(vk_create_image_simple(&vk, dst.extent, format,
			                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			                                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
			                               &dst.memory, &dst.image) == VK_SUCCESS);
			REQUIRE(vk_create_view(&vk, dst.image, VK_IMAGE_VIEW_TYPE_2D, format, color_range, &dst.view) ==
			        VK_SUCCESS);
			views[i] = dst.view;
			VkSemaphoreCreateInfo info{};
			info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			REQUIRE(vk.vkCreateSemaphore(vk.device, &info, nullptr, &complete[i]) == VK_SUCCESS);
		}
		// TRANSFER_SRC lets the test read the private distortion result directly.
		output = render_output_create(&vk, vk.main_queue, vert, frag, format, {2 * f.width, f.height},
		                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		                              views, VK_IMAGE_LAYOUT_GENERAL, 3);
		REQUIRE(output != nullptr);
	}
	Image
	private_image(unsigned slot)
	{
		return {render_output_image(output, slot),
		        VK_NULL_HANDLE,
		        render_output_view(output, slot),
		        {2 * f.width, f.height}};
	}
	void
	signal(unsigned slot)
	{
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &complete[slot];
		vk_queue_lock(f.vk().main_queue);
		VkResult ret = f.vk().vkQueueSubmit(f.vk().main_queue->queue, 1, &submit, VK_NULL_HANDLE);
		vk_queue_unlock(f.vk().main_queue);
		REQUIRE(ret == VK_SUCCESS);
	}
	void
	consume(unsigned slot)
	{
		VkFenceCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		VkFence fence{};
		REQUIRE(f.vk().vkCreateFence(f.vk().device, &info, nullptr, &fence) == VK_SUCCESS);
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &complete[slot];
		submit.pWaitDstStageMask = &stage;
		vk_queue_lock(f.vk().main_queue);
		VkResult ret = f.vk().vkQueueSubmit(f.vk().main_queue->queue, 1, &submit, fence);
		vk_queue_unlock(f.vk().main_queue);
		REQUIRE(ret == VK_SUCCESS);
		REQUIRE(f.vk().vkWaitForFences(f.vk().device, 1, &fence, VK_TRUE, 1000000000ull) == VK_SUCCESS);
		f.vk().vkDestroyFence(f.vk().device, fence, nullptr);
	}
	std::vector<uint8_t>
	pixels(Image image)
	{
		auto cmd = f.begin();
		// Fixture readback uses images[8]; retain ownership even on assertion failure.
		struct Restore
		{
			Fixture &f;
			Image original;
			~Restore()
			{
				f.images[8] = original;
			}
		} restore{f, f.images[8]};
		f.images[8] = image;
		f.copy_output(cmd);
		f.finish(cmd);
		return f.snapshot();
	}
	void
	clear(unsigned slot)
	{
		auto image = private_image(slot);
		auto cmd = f.begin();
		f.barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
		          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		          VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkClearColorValue color{};
		color.float32[0] = float(32 + slot * 64) / 255;
		color.float32[1] = 80.f / 255;
		color.float32[2] = 160.f / 255;
		color.float32[3] = float(50 + slot * 80) / 255;
		f.vk().vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &color_range);
		f.barrier(cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
		f.finish(cmd);
	}
	~OutputFixture()
	{
		if (!f.vk().device)
			return;
		f.vk().vkDeviceWaitIdle(f.vk().device);
		(void)render_output_destroy(&output);
		for (unsigned i = 0; i < 3; i++) {
			if (complete[i])
				f.vk().vkDestroySemaphore(f.vk().device, complete[i], nullptr);
			if (destinations[i].view)
				f.vk().vkDestroyImageView(f.vk().device, destinations[i].view, nullptr);
			if (destinations[i].image)
				f.vk().vkDestroyImage(f.vk().device, destinations[i].image, nullptr);
			if (destinations[i].memory)
				f.vk().vkFreeMemory(f.vk().device, destinations[i].memory, nullptr);
		}
		if (vert)
			f.vk().vkDestroyShaderModule(f.vk().device, vert, nullptr);
		if (frag)
			f.vk().vkDestroyShaderModule(f.vk().device, frag, nullptr);
	}
};
} // namespace

namespace {
VKAPI_ATTR VkResult VKAPI_CALL
output_submit_oom(VkQueue, uint32_t, const VkSubmitInfo *, VkFence)
{
	return VK_ERROR_OUT_OF_HOST_MEMORY;
}
} // namespace

TEST_CASE("Output submission failure leaves the producer signal consumable", "[aux_render][gpu][output]")
{
	OutputFixture of;
	of.init();
	of.clear(0);
	of.signal(0);
	{
		struct Restore
		{
			vk_bundle &vk;
			PFN_vkQueueSubmit original;
			~Restore()
			{
				vk.vkQueueSubmit = original;
			}
		} restore{of.f.vk(), of.f.vk().vkQueueSubmit};
		of.f.vk().vkQueueSubmit = output_submit_oom;
		CHECK(render_output_present(of.output, 0, of.complete[0]) == VK_ERROR_OUT_OF_HOST_MEMORY);
	}
	of.consume(0);
	// A failed output target must be retired, even for a retryable queue OOM.
	CHECK(render_output_present(of.output, 0, of.complete[0]) == VK_ERROR_INITIALIZATION_FAILED);
	CHECK(render_output_destroy(&of.output) == VK_SUCCESS);
	CHECK(of.output == nullptr);
}

TEST_CASE("Private compute output preserves distortion pixels and alpha", "[aux_render][gpu][output]")
{
	OutputFixture of;
	of.init();
	auto &f = of.f;
	for (unsigned scenario = 0; scenario < 4; scenario++)
		for (bool alpha : {false, true}) {
			CAPTURE(scenario, alpha);
			f.config(scenario);
			auto pipeline = f.pipeline(variants[0], scenario, alpha);
			for (unsigned slot = 0; slot < 3; slot++) {
				of.clear(slot);
				Image image = of.private_image(slot);
				VkDescriptorImageInfo info{VK_NULL_HANDLE, image.view, VK_IMAGE_LAYOUT_GENERAL};
				VkWriteDescriptorSet write{};
				write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				write.dstSet = f.set;
				write.dstBinding = 2;
				write.descriptorCount = 1;
				write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
				write.pImageInfo = &info;
				f.vk().vkUpdateDescriptorSets(f.vk().device, 1, &write, 0, nullptr);
				// The production distortion shader, including timewarp/depth/foveation.
				struct Restore
				{
					Fixture &f;
					Image original;
					~Restore()
					{
						f.images[8] = original;
					}
				} restore{f, f.images[8]};
				f.images[8] = image;
				f.run(pipeline, variants[0], 1, true);
				auto expected = f.snapshot();
				of.signal(slot);
				REQUIRE(render_output_present(of.output, slot, of.complete[slot]) == VK_SUCCESS);
				of.consume(slot);
				CHECK(of.pixels(of.destinations[slot]) == expected);
			}
		}
}

TEST_CASE("Private output queues independent slots and retains blocked resources", "[aux_render][gpu][output]")
{
	OutputFixture of;
	of.init(VK_FORMAT_B8G8R8A8_UNORM);
	auto &vk = of.f.vk();
	for (unsigned i = 0; i < 3; i++) {
		of.clear(i);
		of.signal(i);
	}
	REQUIRE(vk.vkDeviceWaitIdle(vk.device) == VK_SUCCESS);
	auto create = reinterpret_cast<PFN_vkCreateEvent>(vk.vkGetDeviceProcAddr(vk.device, "vkCreateEvent"));
	auto wait = reinterpret_cast<PFN_vkCmdWaitEvents>(vk.vkGetDeviceProcAddr(vk.device, "vkCmdWaitEvents"));
	auto set = reinterpret_cast<PFN_vkSetEvent>(vk.vkGetDeviceProcAddr(vk.device, "vkSetEvent"));
	auto destroy = reinterpret_cast<PFN_vkDestroyEvent>(vk.vkGetDeviceProcAddr(vk.device, "vkDestroyEvent"));
	REQUIRE((create && wait && set && destroy));
	VkEventCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO;
	VkEvent event{};
	VkResult ret = create(vk.device, &info, nullptr, &event);
	if (ret == VK_ERROR_FEATURE_NOT_PRESENT)
		SKIP("Host events unavailable on this portability device");
	REQUIRE(ret == VK_SUCCESS);
	struct Gate
	{
		vk_bundle &vk;
		VkEvent event;
		PFN_vkSetEvent set;
		PFN_vkDestroyEvent destroy;
		~Gate()
		{
			set(vk.device, event);
			vk.vkDeviceWaitIdle(vk.device);
			destroy(vk.device, event, nullptr);
		}
	} gate{vk, event, set, destroy};
	auto cmd = of.f.begin();
	wait(cmd, 1, &event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, 0, nullptr, 0,
	     nullptr);
	REQUIRE(vk.vkEndCommandBuffer(cmd) == VK_SUCCESS);
	VkSubmitInfo submit{};
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &cmd;
	vk_queue_lock(vk.main_queue);
	ret = vk.vkQueueSubmit(vk.main_queue->queue, 1, &submit, VK_NULL_HANDLE);
	vk_queue_unlock(vk.main_queue);
	REQUIRE(ret == VK_SUCCESS);
	for (unsigned i = 0; i < 3; i++)
		REQUIRE(render_output_present(of.output, i, of.complete[i]) == VK_SUCCESS);
	CHECK(render_output_present(of.output, 0, of.complete[0]) == VK_TIMEOUT);
	auto *retained = of.output;
	CHECK(render_output_destroy(&of.output) == VK_TIMEOUT);
	CHECK(of.output == retained);
	REQUIRE(set(vk.device, event) == VK_SUCCESS);
	for (unsigned i = 0; i < 3; i++) {
		of.consume(i);
		auto pixels = of.pixels(of.destinations[i]);
		for (size_t n = 0; n < pixels.size(); n += 4) {
			REQUIRE(pixels[n] == 160);
			REQUIRE(pixels[n + 1] == 80);
			REQUIRE(pixels[n + 2] == 32 + i * 64);
			REQUIRE(pixels[n + 3] == 50 + i * 80);
		}
	}
	CHECK(render_output_destroy(&of.output) == VK_SUCCESS);
	CHECK(of.output == nullptr);
}

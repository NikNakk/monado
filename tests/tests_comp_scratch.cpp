// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "render_vulkan_test_context.h"
#include "util/comp_scratch.h"
#include "vk/vk_cmd_pool.h"

TEST_CASE("Compositor scratch images survive rendering resize and reuse", "[gpu][scratch]")
{
	MinimalVulkanContext ctx;
	if (!ctx.init(true))
		SKIP("Vulkan device unavailable");
	auto &vk = ctx.vk;
	struct Resources
	{
		vk_bundle &vk;
		comp_scratch_single_images scratch{};
		vk_cmd_pool pool{};
		render_buffer readback{};
		bool pool_ready = false;
		Resources(vk_bundle &v) : vk(v)
		{
			comp_scratch_single_images_init(&scratch);
		}
		~Resources()
		{
			vk.vkDeviceWaitIdle(vk.device);
			render_buffer_fini(&vk, &readback);
			if (pool_ready)
				vk_cmd_pool_destroy(&vk, &pool);
			comp_scratch_single_images_free(&scratch, &vk);
			comp_scratch_single_images_destroy(&scratch);
		}
	} r(vk);
	REQUIRE(vk_cmd_pool_init(&vk, &r.pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == VK_SUCCESS);
	r.pool_ready = true;
	REQUIRE(render_buffer_init(&vk, &r.readback, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
	                           128 * 128 * 4) == VK_SUCCESS);
	REQUIRE(render_buffer_map(&vk, &r.readback) == VK_SUCCESS);
	for (bool mutable_format : {false, true}) {
		for (VkExtent2D extent : {VkExtent2D{67, 39}, VkExtent2D{128, 128}, VkExtent2D{67, 39}}) {
			CAPTURE(mutable_format, extent.width, extent.height);
			auto ensure = [&]() {
				return mutable_format
				           ? comp_scratch_single_images_ensure_mutable(&r.scratch, &vk, extent)
					   : comp_scratch_single_images_ensure(&r.scratch, &vk, extent,
					                                       VK_FORMAT_R8G8B8A8_UNORM);
			};
			REQUIRE(ensure());
			auto first_image = r.scratch.images[0].image;
			REQUIRE(ensure());
			CHECK(r.scratch.images[0].image == first_image);
			for (unsigned n = 0; n < COMP_SCRATCH_NUM_IMAGES * 2; ++n) {
				uint32_t index;
				comp_scratch_single_images_get(&r.scratch, &index);
				REQUIRE(index == n % COMP_SCRATCH_NUM_IMAGES);
				auto &image = r.scratch.images[index];
				REQUIRE(image.unorm_view != VK_NULL_HANDLE);
				CHECK((image.srgb_view != VK_NULL_HANDLE) == mutable_format);
				VkCommandBuffer cmd;
				REQUIRE(vk_cmd_pool_create_and_begin_cmd_buffer(
				            &vk, &r.pool, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &cmd) ==
				        VK_SUCCESS);
				VkImageMemoryBarrier barrier{};
				barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				barrier.image = image.image;
				barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
				vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
				                        &barrier);
				VkClearColorValue color{};
				color.float32[1] = color.float32[3] = 1;
				vk.vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color,
				                        1, &barrier.subresourceRange);
				barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
				vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
				                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
				                        &barrier);
				VkBufferImageCopy copy{};
				copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
				copy.imageExtent = {extent.width, extent.height, 1};
				vk.vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				                          r.readback.buffer, 1, &copy);
				VkMemoryBarrier host{};
				host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
				host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
				vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
				                        0, 1, &host, 0, nullptr, 0, nullptr);
				REQUIRE(vk_cmd_pool_end_submit_wait_and_free_cmd_buffer(&vk, &r.pool, cmd) ==
				        VK_SUCCESS);
				auto bytes = static_cast<const uint8_t *>(r.readback.mapped);
				bool matches = true;
				for (size_t p = 0; p < extent.width * extent.height * 4; ++p)
					matches &= bytes[p] == (p % 2 ? 255 : 0);
				REQUIRE(matches);
				comp_scratch_single_images_done(&r.scratch);
			}
		}
		comp_scratch_single_images_free(&r.scratch, &vk);
		CHECK(r.scratch.images[0].image == VK_NULL_HANDLE);
	}
}

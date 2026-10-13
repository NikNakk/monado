// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "render_compute_test_fixture.h"

namespace {
// Production layer/distortion shaders with per-eye sRGB intermediate images.
struct ProjectionQuadFixture : LayerFixture
{
	Image scratch[2]{};
	VkImageView scratch_sample[2]{};
	VkSampler scratch_sampler{};
	uint32_t scratch_width{}, scratch_height{};

	void
	init_composition(uint32_t w, uint32_t h, float scale, uint32_t samplers = 2)
	{
		init(w, h);
		init_layers(samplers);
		scratch_width = uint32_t(std::lround(w * scale));
		scratch_height = uint32_t(std::lround(h * scale));
		REQUIRE(vk_create_sampler(&vk(), VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, &scratch_sampler) == VK_SUCCESS);
		for (unsigned eye = 0; eye < 2; ++eye) {
			REQUIRE(render_buffer_map(&vk(), &layer_ubo[eye]) == VK_SUCCESS);
			scratch[eye].extent = {scratch_width, scratch_height};
			REQUIRE(vk_create_image_mutable_rgba(&vk(), scratch[eye].extent,
			                                     VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
			                                     &scratch[eye].memory, &scratch[eye].image) == VK_SUCCESS);
			REQUIRE(vk_create_view_usage(&vk(), scratch[eye].image, VK_IMAGE_VIEW_TYPE_2D,
			                             VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT,
			                             color_range, &scratch[eye].view) == VK_SUCCESS);
			REQUIRE(vk_create_view_usage(&vk(), scratch[eye].image, VK_IMAGE_VIEW_TYPE_2D,
			                             VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT,
			                             color_range, &scratch_sample[eye]) == VK_SUCCESS);
			auto cmd = begin();
			barrier(cmd, scratch[eye], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
			        VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			finish(cmd);
			VkDescriptorImageInfo output{VK_NULL_HANDLE, scratch[eye].view, VK_IMAGE_LAYOUT_GENERAL};
			VkWriteDescriptorSet write{};
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.dstSet = layer_sets[eye];
			write.dstBinding = 2;
			write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			write.descriptorCount = 1;
			write.pImageInfo = &output;
			vk().vkUpdateDescriptorSets(vk().device, 1, &write, 0, nullptr);
		}
		VkDescriptorImageInfo sources[4]{};
		for (unsigned i = 0; i < 4; ++i) {
			sources[i] = {scratch_sampler, scratch_sample[i % 2], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		}
		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = set;
		write.dstBinding = 0;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.descriptorCount = 4;
		write.pImageInfo = sources;
		vk().vkUpdateDescriptorSets(vk().device, 1, &write, 0, nullptr);
	}

	void
	scene(unsigned pose, unsigned flags, unsigned scenario, unsigned quads = 1)
	{
		config(scenario);
		config_layers(quads, pose, flags);
		for (unsigned eye = 0; eye < 2; ++eye) {
			auto data = *static_cast<render_compute_layer_ubo_data *>(layer_ubo[eye].mapped);
			data.view = {0, 0, scratch_width, scratch_height};
			// Opaque background, independently alpha-blended quad.
			data.layers[0].color_scale.a = 0;
			data.layers[0].color_bias.a = 1;
			REQUIRE(render_buffer_write(&vk(), &layer_ubo[eye], &data, sizeof(data)) == VK_SUCCESS);
		}
	}

	void
	transition_scratch(VkCommandBuffer cmd, bool to_sample)
	{
		VkImageMemoryBarrier barriers[2]{};
		for (unsigned eye = 0; eye < 2; ++eye) {
			auto &b = barriers[eye];
			b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			b.image = scratch[eye].image;
			b.subresourceRange = color_range;
			b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			b.oldLayout = to_sample ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			b.newLayout = to_sample ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
			b.srcAccessMask = to_sample ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
			b.dstAccessMask = to_sample ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_SHADER_WRITE_BIT;
		}
		vk().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
	}

	double
	run_composition(VkPipeline layers, VkPipeline distortion, unsigned count, bool copy)
	{
		auto cmd = begin();
		if (queries) {
			vk().vkCmdResetQueryPool(cmd, queries, 0, 2);
			vk().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
		}
		for (unsigned i = 0; i < count; ++i) {
			vk().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layers);
			for (unsigned eye = 0; eye < 2; ++eye) {
				vk().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layer_layout, 0, 1,
				                             &layer_sets[eye], 0, nullptr);
				vk().vkCmdDispatch(cmd, (scratch_width + 7) / 8, (scratch_height + 7) / 8, 1);
			}
			transition_scratch(cmd, true);
			vk().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, distortion);
			vk().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
			vk().vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 2);
			transition_scratch(cmd, false);
			barrier(cmd, images[8], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		}
		if (queries) {
			vk().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
		}
		if (copy) {
			copy_output(cmd);
		}
		finish(cmd);
		return duration(count);
	}

	~ProjectionQuadFixture()
	{
		if (!vk().device) return;
		vk().vkDeviceWaitIdle(vk().device);
		// All pipelines are gone before either derived/base descriptor layout.
		for (auto p : pipelines) vk().vkDestroyPipeline(vk().device, p, nullptr);
		pipelines.clear();
		for (unsigned eye = 0; eye < 2; ++eye) {
			if (scratch_sample[eye]) vk().vkDestroyImageView(vk().device, scratch_sample[eye], nullptr);
			if (scratch[eye].view) vk().vkDestroyImageView(vk().device, scratch[eye].view, nullptr);
			if (scratch[eye].image) vk().vkDestroyImage(vk().device, scratch[eye].image, nullptr);
			if (scratch[eye].memory) vk().vkFreeMemory(vk().device, scratch[eye].memory, nullptr);
		}
		if (scratch_sampler) vk().vkDestroySampler(vk().device, scratch_sampler, nullptr);
	}
};
} // namespace

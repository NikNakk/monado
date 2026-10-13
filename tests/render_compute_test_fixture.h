// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

/* Offscreen correctness and GPU timing of the production distortion shader.
 * Workgroup variants differ only in their local-size declaration. */
#include "catch_amalgamated.hpp"
#include "render_vulkan_test_context.h"
#include "cache/render_shader_specialization.hpp"
#include "cache/render_shader_specialization_helpers.h"
#include "render/render_interface.h"
#include "vk/vk_cmd_pool.h"
#include "os/os_time.h"
#include "distortion_8x8.comp.h"
#include "distortion_16x8.comp.h"
#include "distortion_16x16.comp.h"
#include "layer.comp.h"
#include "render/render_quad_bounds.h"
#include "math/m_api.h"
#include "layer_defines.inc.glsl"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <memory>
#include <filesystem>

namespace {
constexpr VkMemoryPropertyFlags host_memory =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
struct Image
{
	VkImage image{};
	VkDeviceMemory memory{};
	VkImageView view{};
	VkExtent2D extent{};
};
struct Variant
{
	const char *name;
	uint32_t x, y;
	const uint32_t *code;
	size_t bytes;
};
constexpr Variant variants[] = {
    {"8x8", 8, 8, distortion_8x8, sizeof(distortion_8x8)},
    {"16x8", 16, 8, distortion_16x8, sizeof(distortion_16x8)},
    {"16x16", 16, 16, distortion_16x16, sizeof(distortion_16x16)},
};

struct Fixture
{
	MinimalVulkanContext ctx;
	vk_cmd_pool pool{};
	bool pool_ready = false;
	std::array<Image, 9> images{}; // two color sources, six distortion maps, output
	render_buffer ubo{}, readback{};
	VkSampler sampler{};
	VkDescriptorSetLayout set_layout{};
	VkPipelineLayout layout{};
	VkDescriptorPool descriptors{};
	VkDescriptorSet set{};
	VkQueryPool queries{};
	std::vector<VkShaderModule> modules;
	std::vector<VkPipeline> pipelines;
	uint32_t width{}, height{};

	vk_bundle &
	vk()
	{
		return ctx.vk;
	}
	VkCommandBuffer
	begin()
	{
		VkCommandBuffer cmd{};
		REQUIRE(vk_cmd_pool_create_and_begin_cmd_buffer(
		            &vk(), &pool, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &cmd) == VK_SUCCESS);
		return cmd;
	}
	void
	finish(VkCommandBuffer cmd)
	{
		REQUIRE(vk_cmd_pool_end_submit_wait_and_free_cmd_buffer(&vk(), &pool, cmd) == VK_SUCCESS);
	}
	void
	barrier(VkCommandBuffer cmd,
	        Image &image,
	        VkImageLayout old_layout,
	        VkImageLayout new_layout,
	        VkAccessFlags src,
	        VkAccessFlags dst,
	        VkPipelineStageFlags src_stage,
	        VkPipelineStageFlags dst_stage)
	{
		VkImageMemoryBarrier b{};
		b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		b.srcAccessMask = src;
		b.dstAccessMask = dst;
		b.oldLayout = old_layout;
		b.newLayout = new_layout;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image.image;
		b.subresourceRange = color_range;
		vk().vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
	}
	void
	create_image(Image &image, VkExtent2D extent, VkFormat format, const void *data, size_t bytes)
	{
		image.extent = extent;
		VkImageUsageFlags usage = data ? VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
		                               : VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		REQUIRE(vk_create_image_simple(&vk(), extent, format, usage, &image.memory, &image.image) ==
		        VK_SUCCESS);
		REQUIRE(vk_create_view(&vk(), image.image, VK_IMAGE_VIEW_TYPE_2D, format, color_range, &image.view) ==
		        VK_SUCCESS);
		auto cmd = begin();
		if (data) {
			render_buffer upload{};
			REQUIRE(render_buffer_init(&vk(), &upload, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host_memory,
			                           bytes) == VK_SUCCESS);
			REQUIRE(render_buffer_map_and_write(&vk(), &upload, const_cast<void *>(data), bytes) ==
			        VK_SUCCESS);
			barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
			        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			        VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy copy{};
			copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.imageExtent = {extent.width, extent.height, 1};
			vk().vkCmdCopyBufferToImage(cmd, upload.buffer, image.image,
			                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
			barrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
			        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			finish(cmd);
			render_buffer_fini(&vk(), &upload);
		} else {
			barrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
			        VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			finish(cmd);
		}
	}
	void
	init(uint32_t w, uint32_t h)
	{
		width = w;
		height = h;
		if (!ctx.init())
			SKIP("Vulkan device unavailable");
		REQUIRE(vk_cmd_pool_init(&vk(), &pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == VK_SUCCESS);
		pool_ready = true;
		VkPhysicalDeviceProperties props{};
		vk().vkGetPhysicalDeviceProperties(vk().physical_device, &props);
		std::fprintf(stderr, "GPU: %s; timestamp period %.3f ns; valid bits %u\n", props.deviceName,
		             vk().features.timestamp_period, vk().features.timestamp_valid_bits);
		// Deterministic gradients, edges and varying alpha in different eyes.
		std::vector<uint8_t> pixels(size_t(w) * h * 4);
		for (unsigned eye = 0; eye < 2; ++eye) {
			for (uint32_t y = 0; y < h; ++y)
				for (uint32_t x = 0; x < w; ++x) {
					size_t i = (size_t(y) * w + x) * 4;
					pixels[i] = uint8_t(x * 255 / (w - 1));
					pixels[i + 1] = uint8_t(y * 255 / (h - 1));
					pixels[i + 2] = ((x / 7 + y / 5 + eye) % 2) ? 240 : 8;
					pixels[i + 3] = uint8_t(32 + ((x + y + eye * 59) % 224));
				}
			create_image(images[eye], {w, h}, VK_FORMAT_R8G8B8A8_UNORM, pixels.data(), pixels.size());
		}
		// Small sampled distortion grids with distinct chromatic offsets.
		constexpr uint32_t n = 33;
		std::vector<float> grid(n * n * 2);
		for (unsigned channel = 0; channel < 3; ++channel)
			for (unsigned eye = 0; eye < 2; ++eye) {
				for (unsigned y = 0; y < n; ++y)
					for (unsigned x = 0; x < n; ++x) {
						float u = float(x) / (n - 1), v = float(y) / (n - 1);
						float dx = u - .5f, dy = v - .5f;
						float scale = 1.f + .12f * (dx * dx + dy * dy);
						grid[(y * n + x) * 2] = .5f + dx * scale + (int(channel) - 1) * .002f;
						grid[(y * n + x) * 2 + 1] = .5f + dy * scale + eye * .001f;
					}
				create_image(images[2 + channel * 2 + eye], {n, n}, VK_FORMAT_R32G32_SFLOAT,
				             grid.data(), grid.size() * sizeof(float));
			}
		create_image(images[8], {2 * w, h}, VK_FORMAT_R8G8B8A8_UNORM, nullptr, 0);
		REQUIRE(vk_create_sampler(&vk(), VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, &sampler) == VK_SUCCESS);
		REQUIRE(render_buffer_init(&vk(), &ubo, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host_memory,
		                           sizeof(render_compute_distortion_ubo_data)) == VK_SUCCESS);
		REQUIRE(render_buffer_init(&vk(), &readback, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host_memory,
		                           size_t(2 * w) * h * 4) == VK_SUCCESS);
		REQUIRE(render_buffer_map(&vk(), &readback) == VK_SUCCESS);
		VkDescriptorSetLayoutBinding bindings[] = {
		    {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		};
		VkDescriptorSetLayoutCreateInfo si{};
		si.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		si.bindingCount = 4;
		si.pBindings = bindings;
		REQUIRE(vk().vkCreateDescriptorSetLayout(vk().device, &si, nullptr, &set_layout) == VK_SUCCESS);
		VkPipelineLayoutCreateInfo li{};
		li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		li.setLayoutCount = 1;
		li.pSetLayouts = &set_layout;
		REQUIRE(vk().vkCreatePipelineLayout(vk().device, &li, nullptr, &layout) == VK_SUCCESS);
		VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10},
		                                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
		                                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
		VkDescriptorPoolCreateInfo pi{};
		pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pi.maxSets = 1;
		pi.poolSizeCount = 3;
		pi.pPoolSizes = sizes;
		REQUIRE(vk().vkCreateDescriptorPool(vk().device, &pi, nullptr, &descriptors) == VK_SUCCESS);
		VkDescriptorSetAllocateInfo ai{};
		ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		ai.descriptorPool = descriptors;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &set_layout;
		REQUIRE(vk().vkAllocateDescriptorSets(vk().device, &ai, &set) == VK_SUCCESS);
		VkDescriptorImageInfo src[4], dist[6];
		for (unsigned i = 0; i < 4; ++i)
			src[i] = {sampler, images[i % 2].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		for (unsigned i = 0; i < 6; ++i)
			dist[i] = {sampler, images[2 + i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkDescriptorImageInfo out{VK_NULL_HANDLE, images[8].view, VK_IMAGE_LAYOUT_GENERAL};
		VkDescriptorBufferInfo bi{ubo.buffer, 0, ubo.size};
		VkWriteDescriptorSet writes[4]{};
		for (unsigned i = 0; i < 4; ++i) {
			writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[i].dstSet = set;
			writes[i].dstBinding = i;
			writes[i].descriptorCount = bindings[i].descriptorCount;
			writes[i].descriptorType = bindings[i].descriptorType;
		}
		writes[0].pImageInfo = src;
		writes[1].pImageInfo = dist;
		writes[2].pImageInfo = &out;
		writes[3].pBufferInfo = &bi;
		vk().vkUpdateDescriptorSets(vk().device, 4, writes, 0, nullptr);
		if (vk().features.timestamp_valid_bits && vk().features.timestamp_compute_and_graphics) {
			VkQueryPoolCreateInfo qi{};
			qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
			qi.queryCount = 2;
			REQUIRE(vk().vkCreateQueryPool(vk().device, &qi, nullptr, &queries) == VK_SUCCESS);
		}
	}
	void
	config(unsigned scenario)
	{
		render_compute_distortion_ubo_data data{};
		for (unsigned eye = 0; eye < 2; ++eye) {
			data.views[eye] = {eye * width, 0, width, height};
			data.pre_transforms[eye] = {-1, -1, 2, 2};
			data.post_transforms[eye] = {0, 0, 1, 1};
			for (unsigned i = 0; i < 4; ++i) {
				data.transform_timewarp_scanout_begin[eye].v[i * 5] = 1;
				data.transform_timewarp_scanout_end[eye].v[i * 5] = 1;
			}
			data.transform_timewarp_scanout_begin[eye].v[12] = -.02f;
			data.transform_timewarp_scanout_end[eye].v[12] = .02f;
			if (scenario == 2) {
				data.foveation[eye].value = 1;
				data.foveation[eye].boundary_count = 129;
				for (unsigned i = 0; i < 129; ++i) {
					float t = float(i) / 128;
					data.foveation_x[eye * 33 + i / 4].v[i % 4] = t * t;
					data.foveation_y[eye * 33 + i / 4].v[i % 4] = t * t;
				}
			}
			if (scenario == 3) {
				data.has_depth[eye].value = 1;
				data.depth_post_transforms[eye] = {0, 0, 1, 1};
				data.source_uv_to_tanangle[eye] = {-1, -1, 2, 2};
				data.projection_depth[eye] = {0, 1, .1f, 100};
				data.new_origin_in_source_view_scanout_begin[eye].val.x = .003f;
				data.new_origin_in_source_view_scanout_end[eye].val.x = .006f;
			}
		}
		REQUIRE(render_buffer_write(&vk(), &ubo, &data, sizeof(data)) == VK_SUCCESS);
	}
	VkPipeline
	pipeline(const Variant &v, unsigned scenario, bool alpha, bool identity = false)
	{
		VkShaderModuleCreateInfo mi{};
		mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		mi.codeSize = v.bytes;
		mi.pCode = v.code;
		VkShaderModule module{};
		REQUIRE(vk().vkCreateShaderModule(vk().device, &mi, nullptr, &module) == VK_SUCCESS);
		modules.push_back(module);
		auto spec = render_make_distortion_spec(33, scenario != 0, 2);
		spec.preserve_source_alpha = alpha;
		spec.use_identity_distortion = identity;
		auto &entries = ShaderSpecializationMap<render_distortion_spec>::kEntries;
		VkSpecializationInfo sp{uint32_t(entries.size()), entries.data(), sizeof(spec), &spec};
		VkComputePipelineCreateInfo ci{};
		ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		ci.stage.module = module;
		ci.stage.pName = "main";
		ci.stage.pSpecializationInfo = &sp;
		ci.layout = layout;
		VkPipeline result{};
		REQUIRE(vk().vkCreateComputePipelines(vk().device, VK_NULL_HANDLE, 1, &ci, nullptr, &result) ==
		        VK_SUCCESS);
		pipelines.push_back(result);
		return result;
	}
	double
	run(VkPipeline p, const Variant &v, unsigned count, bool copy)
	{
		auto cmd = begin();
		if (queries)
			vk().vkCmdResetQueryPool(cmd, queries, 0, 2);
		vk().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
		vk().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
		if (queries)
			vk().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
		for (unsigned i = 0; i < count; ++i) {
			vk().vkCmdDispatch(cmd, (width + v.x - 1) / v.x, (height + v.y - 1) / v.y, 2);
			barrier(cmd, images[8], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		}
		if (queries)
			vk().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
		if (copy)
			copy_output(cmd);
		finish(cmd);
		return duration(count);
	}
	void
	copy_output(VkCommandBuffer cmd)
	{
		barrier(cmd, images[8], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		        VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {2 * width, height, 1};
		vk().vkCmdCopyImageToBuffer(cmd, images[8].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer,
		                            1, &region);
		VkMemoryBarrier host{};
		host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		vk().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host,
		                          0, nullptr, 0, nullptr);
		barrier(cmd, images[8], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	}
	double
	duration(unsigned count)
	{
		if (!queries)
			return 0;
		uint64_t ticks[2]{};
		REQUIRE(vk().vkGetQueryPoolResults(vk().device, queries, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t),
		                                   VK_QUERY_RESULT_64_BIT) == VK_SUCCESS);
		uint32_t bits = vk().features.timestamp_valid_bits;
		uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
		return double((ticks[1] - ticks[0]) & mask) * vk().features.timestamp_period / (1e6 * count);
	}

	std::vector<uint8_t>
	snapshot()
	{
		auto ptr = static_cast<uint8_t *>(readback.mapped);
		return {ptr, ptr + readback.size};
	}
	~Fixture()
	{
		if (!vk().device)
			return;
		vk().vkDeviceWaitIdle(vk().device);
		for (auto p : pipelines)
			vk().vkDestroyPipeline(vk().device, p, nullptr);
		for (auto m : modules)
			vk().vkDestroyShaderModule(vk().device, m, nullptr);
		if (queries)
			vk().vkDestroyQueryPool(vk().device, queries, nullptr);
		if (descriptors)
			vk().vkDestroyDescriptorPool(vk().device, descriptors, nullptr);
		if (layout)
			vk().vkDestroyPipelineLayout(vk().device, layout, nullptr);
		if (set_layout)
			vk().vkDestroyDescriptorSetLayout(vk().device, set_layout, nullptr);
		if (sampler)
			vk().vkDestroySampler(vk().device, sampler, nullptr);
		render_buffer_fini(&vk(), &ubo);
		render_buffer_fini(&vk(), &readback);
		for (auto &i : images) {
			if (i.view)
				vk().vkDestroyImageView(vk().device, i.view, nullptr);
			if (i.image)
				vk().vkDestroyImage(vk().device, i.image, nullptr);
			if (i.memory)
				vk().vkFreeMemory(vk().device, i.memory, nullptr);
		}
		if (pool_ready)
			vk_cmd_pool_destroy(&vk(), &pool);
	}
};
// Separate per-eye UBOs and descriptor sets match the layer-squasher dispatch.
struct LayerFixture : Fixture
{
	render_buffer layer_ubo[2]{}, foveation_ubo{};
	VkDescriptorSetLayout layer_set_layout{};
	VkPipelineLayout layer_layout{};
	VkDescriptorPool layer_pool{};
	VkDescriptorSet layer_sets[2]{};
	uint32_t layer_sampler_count = 2;
	void
	init_layers(uint32_t sampler_count = 2)
	{
		layer_sampler_count = sampler_count;
		REQUIRE(render_buffer_init(&vk(), &foveation_ubo, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host_memory,
		                           sizeof(render_compute_layer_foveation_ubo_data)) == VK_SUCCESS);
		render_compute_layer_foveation_ubo_data empty{};
		REQUIRE(render_buffer_write(&vk(), &foveation_ubo, &empty, sizeof(empty)) == VK_SUCCESS);
		VkDescriptorSetLayoutBinding bindings[] = {
		    {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sampler_count, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		    {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		};
		VkDescriptorSetLayoutCreateInfo si{};
		si.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		si.bindingCount = 4;
		si.pBindings = bindings;
		REQUIRE(vk().vkCreateDescriptorSetLayout(vk().device, &si, nullptr, &layer_set_layout) == VK_SUCCESS);
		VkPipelineLayoutCreateInfo li{};
		li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		li.setLayoutCount = 1;
		li.pSetLayouts = &layer_set_layout;
		REQUIRE(vk().vkCreatePipelineLayout(vk().device, &li, nullptr, &layer_layout) == VK_SUCCESS);
		VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * sampler_count},
		                                {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
		                                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4}};
		VkDescriptorPoolCreateInfo pi{};
		pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pi.maxSets = 2;
		pi.poolSizeCount = 3;
		pi.pPoolSizes = sizes;
		REQUIRE(vk().vkCreateDescriptorPool(vk().device, &pi, nullptr, &layer_pool) == VK_SUCCESS);
		for (unsigned eye = 0; eye < 2; ++eye) {
			REQUIRE(render_buffer_init(&vk(), &layer_ubo[eye], VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			                           host_memory, sizeof(render_compute_layer_ubo_data)) == VK_SUCCESS);
			VkDescriptorSetAllocateInfo ai{};
			ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
			ai.descriptorPool = layer_pool;
			ai.descriptorSetCount = 1;
			ai.pSetLayouts = &layer_set_layout;
			REQUIRE(vk().vkAllocateDescriptorSets(vk().device, &ai, &layer_sets[eye]) == VK_SUCCESS);
			std::vector<VkDescriptorImageInfo> src(sampler_count);
			for (uint32_t i = 0; i < sampler_count; ++i)
				src[i] = {sampler, images[i == 0 ? eye : 1 - eye].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
			VkDescriptorImageInfo out{VK_NULL_HANDLE, images[8].view, VK_IMAGE_LAYOUT_GENERAL};
			VkDescriptorBufferInfo ub{layer_ubo[eye].buffer, 0, layer_ubo[eye].size};
			VkDescriptorBufferInfo fb{foveation_ubo.buffer, 0, foveation_ubo.size};
			VkWriteDescriptorSet writes[4]{};
			for (unsigned i = 0; i < 4; ++i) {
				writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writes[i].dstSet = layer_sets[eye];
				writes[i].dstBinding = bindings[i].binding;
				writes[i].descriptorCount = bindings[i].descriptorCount;
				writes[i].descriptorType = bindings[i].descriptorType;
			}
			writes[0].pImageInfo = src.data();
			writes[1].pImageInfo = &out;
			writes[2].pBufferInfo = &ub;
			writes[3].pBufferInfo = &fb;
			vk().vkUpdateDescriptorSets(vk().device, 4, writes, 0, nullptr);
		}
	}
	void
	config_layers(unsigned quads, unsigned pose = 0, unsigned flags = 0)
	{
		for (unsigned eye = 0; eye < 2; ++eye) {
			render_compute_layer_ubo_data data{};
			data.view = {eye * width, 0, width, height};
			data.pre_transform = {-1, -1, 2, 2};
			if (pose == 20)
				data.pre_transform = {-.3f, -.7f, 1.6f, 2.4f};
			if (pose == 21)
				data.pre_transform = {-1, 1, 2, -2};
			data.layer_count.value = 1 + quads;
			auto &projection = data.layers[0];
			projection.layer_data.layer_type = LAYER_COMP_TYPE_PROJECTION;
			projection.post_transforms = {0, 0, 1, 1};
			projection.color_scale = {1, 1, 1, 1};
			for (unsigned i = 0; i < 4; ++i)
				projection.transforms.v[i * 5] = 1;
			for (unsigned q = 0; q < quads; ++q) {
				auto &quad = data.layers[1 + q];
				quad.layer_data.layer_type = LAYER_COMP_TYPE_QUAD;
				quad.layer_data.unpremultiplied_alpha = flags & 1;
				quad.layer_data.inverted_alpha = (flags >> 1) & 1;
				quad.image_info.color_image_index = 1;
				quad.post_transforms = {0, 0, 1, 1};
				quad.color_scale = {.7f, .8f, .9f, .6f};
				quad.color_bias = {.01f, .02f, .03f, 0};
				float angle = float(pose) * .19f;
				float cs = std::cos(angle), sn = std::sin(angle);
				float x = .03f * (int(q) - 1), z = pose == 17   ? 1.f
				                                   : pose == 19 ? -.3f
				                                                : -1.f - .1f * q;
				quad.quad_position.val = {x, 0, z};
				quad.quad_normal.val = {sn, 0, cs};
				quad.quad_extent.val = pose == 18 ? xrt_vec2{10, 10} : xrt_vec2{1.1f, 1.3f};
				// Inverse of a rigid Y rotation and translation.
				quad.inverse_quad_transform.v[0] = cs;
				quad.inverse_quad_transform.v[2] = sn;
				quad.inverse_quad_transform.v[5] = 1;
				quad.inverse_quad_transform.v[8] = -sn;
				quad.inverse_quad_transform.v[10] = cs;
				quad.inverse_quad_transform.v[15] = 1;
				quad.inverse_quad_transform.v[12] = -cs * x + sn * z;
				quad.inverse_quad_transform.v[14] = -sn * x - cs * z;
				struct xrt_matrix_4x4 plane{};
				plane.v[0] = cs;
				plane.v[2] = -sn;
				plane.v[5] = 1;
				plane.v[8] = sn;
				plane.v[10] = cs;
				plane.v[12] = x;
				plane.v[14] = z;
				plane.v[15] = 1;
				if (pose >= 25) {
					float cp = std::cos(.13f * pose), sp = std::sin(.13f * pose);
					plane.v[4] = sn * sp;
					plane.v[5] = cp;
					plane.v[6] = cs * sp;
					plane.v[8] = sn * cp;
					plane.v[9] = -sp;
					plane.v[10] = cs * cp;
					plane.v[13] = .2f;
					quad.quad_position.val.y = .2f;
					quad.quad_normal.val = {plane.v[8], plane.v[9], plane.v[10]};
				}
				math_matrix_4x4_inverse(&plane, &quad.inverse_quad_transform);
				if (pose == 22)
					quad.post_transforms = {.2f, .8f, .6f, -.6f};
				if (pose == 23)
					quad.quad_extent.val = {.001f, .001f};

				render_quad_view_bounds(&plane, &quad.quad_extent.val, &data.pre_transform, width,
				                        height, quad.mv_inverse.v);
			}
			REQUIRE(render_buffer_write(&vk(), &layer_ubo[eye], &data, sizeof(data)) == VK_SUCCESS);
		}
	}
	VkPipeline
	layer_pipeline(bool candidate, bool correction = false, bool timewarp = true,
	               bool common = false)
	{
		VkShaderModuleCreateInfo mi{};
		mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		mi.codeSize = sizeof(layer_comp);
		mi.pCode = layer_comp;
		VkShaderModule module{};
		REQUIRE(vk().vkCreateShaderModule(vk().device, &mi, nullptr, &module) == VK_SUCCESS);
		modules.push_back(module);
		auto spec = render_make_layer_spec(timewarp, correction, layer_sampler_count, RENDER_LAYER_DEFAULT_INSET_BLEND_EDGE);
		spec.use_quad_bounds = candidate;
		spec.use_common_layers = common;
		auto &entries = ShaderSpecializationMap<render_layer_spec>::kEntries;
		VkSpecializationInfo sp{uint32_t(entries.size()), entries.data(), sizeof(spec), &spec};
		VkComputePipelineCreateInfo ci{};
		ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		ci.stage.module = module;
		ci.stage.pName = "main";
		ci.stage.pSpecializationInfo = &sp;
		ci.layout = layer_layout;
		VkPipeline result{};
		REQUIRE(vk().vkCreateComputePipelines(vk().device, VK_NULL_HANDLE, 1, &ci, nullptr, &result) ==
		        VK_SUCCESS);
		pipelines.push_back(result);
		return result;
	}
	double
	run_layers(VkPipeline p, unsigned count, bool copy)
	{
		auto cmd = begin();
		if (queries)
			vk().vkCmdResetQueryPool(cmd, queries, 0, 2);
		vk().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
		if (queries)
			vk().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
		for (unsigned i = 0; i < count; ++i) {
			for (unsigned eye = 0; eye < 2; ++eye) {
				vk().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layer_layout, 0, 1,
				                             &layer_sets[eye], 0, nullptr);
				vk().vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
			}
			barrier(cmd, images[8], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
			        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		}
		if (queries)
			vk().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
		if (copy)
			copy_output(cmd);
		finish(cmd);
		return duration(count);
	}
	~LayerFixture()
	{
		if (!vk().device)
			return;
		vk().vkDeviceWaitIdle(vk().device);
		// Pipelines must be destroyed before their layouts.
		for (auto p : pipelines)
			vk().vkDestroyPipeline(vk().device, p, nullptr);
		pipelines.clear();
		if (layer_pool)
			vk().vkDestroyDescriptorPool(vk().device, layer_pool, nullptr);
		if (layer_layout)
			vk().vkDestroyPipelineLayout(vk().device, layer_layout, nullptr);
		if (layer_set_layout)
			vk().vkDestroyDescriptorSetLayout(vk().device, layer_set_layout, nullptr);
		for (auto &b : layer_ubo)
			render_buffer_fini(&vk(), &b);
		render_buffer_fini(&vk(), &foveation_ubo);
	}
};

const char *scenarios[] = {"distortion", "scanline-timewarp", "foveation", "depth"};
} // namespace

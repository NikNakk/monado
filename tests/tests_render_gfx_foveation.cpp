// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Foveated source reconstruction on the graphics compositor path.
 *
 * Draws a source image whose texels encode their own coordinates through the
 * mesh distortion and projection layer shaders, once without and once with a
 * foveation map, and checks that the map moved every sample to where the CPU
 * reference says.
 */

#include "catch_amalgamated.hpp"
#include "render_vulkan_test_context.h"

#include "math/m_api.h"
#include "render/render_interface.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"
#include "vk/vk_cmd_pool.h"
#include "xrt/xrt_device.h"

#include <cmath>
#include <cstdlib>
#include <vector>

namespace {

constexpr uint32_t kSrcSize = 256;
constexpr uint32_t kDstSize = 96;
constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_UNORM;
constexpr VkMemoryPropertyFlags kHostMemory =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
constexpr VkImageSubresourceRange kColorRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

// Different curves per axis, so a swapped axis fails.
float
map_x_sample(float t)
{
	return t * t;
}

float
map_y_sample(float t)
{
	return 0.25f * t + 0.75f * t * t * t;
}

xrt_foveation_map_data
make_map()
{
	xrt_foveation_map_data map{};
	map.enabled = 1;
	map.boundary_count = XRT_FOVEATION_MAP_BOUNDARY_COUNT;
	for (uint32_t i = 0; i < XRT_FOVEATION_MAP_BOUNDARY_COUNT; ++i) {
		const float t = float(i) / float(XRT_FOVEATION_MAP_BOUNDARY_COUNT - 1);
		map.x[i] = map_x_sample(t);
		map.y[i] = map_y_sample(t);
	}
	return map;
}

// CPU reference of foveation.inc.glsl.
float
apply_map(const float *samples, float uv)
{
	if (uv < 0.f || uv > 1.f) {
		return uv;
	}
	const float intervals = float(XRT_FOVEATION_MAP_BOUNDARY_COUNT - 1);
	const float scaled = std::fmin(uv * intervals, intervals - 1e-6f);
	const uint32_t cell = uint32_t(std::floor(scaled));
	const float local = scaled - float(cell);
	return samples[cell] + (samples[cell + 1] - samples[cell]) * local;
}

struct Image
{
	VkImage image{};
	VkDeviceMemory memory{};
	VkImageView view{};
};

struct GfxFixture
{
	MinimalVulkanContext ctx;
	vk_cmd_pool pool{};
	bool pool_ready = false;

	xrt_hmd_parts hmd{};
	xrt_device xdev{};

	render_shaders shaders{};
	bool shaders_ready = false;
	render_resources r{};
	bool resources_ready = false;

	render_gfx_render_pass rgrp{};
	render_gfx_target_resources rtr{};
	bool target_ready = false;

	Image src{}, dst{}, inset{}, filtered{};
	render_buffer readback{};

	vk_bundle &
	vk()
	{
		return ctx.vk;
	}

	VkCommandBuffer
	begin_cmd()
	{
		VkCommandBuffer cmd{};
		REQUIRE(vk_cmd_pool_create_and_begin_cmd_buffer(
		            &vk(), &pool, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, &cmd) == VK_SUCCESS);
		return cmd;
	}

	void
	finish_cmd(VkCommandBuffer cmd)
	{
		REQUIRE(vk_cmd_pool_end_submit_wait_and_free_cmd_buffer(&vk(), &pool, cmd) == VK_SUCCESS);
	}

	void
	barrier(VkCommandBuffer cmd,
	        VkImage image,
	        VkImageLayout from,
	        VkImageLayout to,
	        VkAccessFlags src_access,
	        VkAccessFlags dst_access,
	        VkPipelineStageFlags src_stage,
	        VkPipelineStageFlags dst_stage)
	{
		VkImageMemoryBarrier b{};
		b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		b.srcAccessMask = src_access;
		b.dstAccessMask = dst_access;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = kColorRange;
		vk().vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
	}

	void
	create_image(Image &image, uint32_t size, VkImageUsageFlags usage)
	{
		REQUIRE(vk_create_image_simple(&vk(), {size, size}, kFormat, usage, &image.memory, &image.image) ==
		        VK_SUCCESS);
		REQUIRE(vk_create_view(&vk(), image.image, VK_IMAGE_VIEW_TYPE_2D, kFormat, kColorRange, &image.view) ==
		        VK_SUCCESS);
	}

	void
	init()
	{
		if (!ctx.init()) {
			SKIP("Vulkan device unavailable");
		}
		REQUIRE(vk_cmd_pool_init(&vk(), &pool, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT) == VK_SUCCESS);
		pool_ready = true;

		// A two-view device without distortion: the mesh maps the target
		// straight onto the source.
		const xrt_fov fov = {-0.7f, 0.7f, 0.7f, -0.7f};
		hmd.view_count = 2;
		for (uint32_t i = 0; i < 2; ++i) {
			hmd.distortion.fov[i] = fov;
			hmd.views[i].display.w_pixels = kDstSize;
			hmd.views[i].display.h_pixels = kDstSize;
			hmd.views[i].rot = u_device_rotation_ident;
		}
		xdev.hmd = &hmd;
		u_distortion_mesh_set_none(&xdev);

		REQUIRE(render_shaders_load(&shaders, &vk()));
		shaders_ready = true;
		REQUIRE(render_resources_init(&r, &shaders, &vk(), &xdev));
		resources_ready = true;

		// Source: every texel holds its own normalized coordinate.
		create_image(src, kSrcSize, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		std::vector<uint16_t> pixels(size_t(kSrcSize) * kSrcSize * 4);
		for (uint32_t y = 0; y < kSrcSize; ++y) {
			for (uint32_t x = 0; x < kSrcSize; ++x) {
				uint16_t *p = &pixels[(size_t(y) * kSrcSize + x) * 4];
				p[0] = uint16_t(std::lround((x + 0.5) / kSrcSize * 65535.0));
				p[1] = uint16_t(std::lround((y + 0.5) / kSrcSize * 65535.0));
				p[2] = 0;
				p[3] = 65535;
			}
		}
		const VkDeviceSize bytes = pixels.size() * sizeof(uint16_t);
		render_buffer upload{};
		REQUIRE(render_buffer_init(&vk(), &upload, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kHostMemory, bytes) ==
		        VK_SUCCESS);
		REQUIRE(render_buffer_map_and_write(&vk(), &upload, pixels.data(), bytes) == VK_SUCCESS);
		VkCommandBuffer cmd = begin_cmd();
		barrier(cmd, src.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		        VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {kSrcSize, kSrcSize, 1};
		vk().vkCmdCopyBufferToImage(cmd, upload.buffer, src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		                            &copy);
		barrier(cmd, src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		finish_cmd(cmd);
		render_buffer_fini(&vk(), &upload);

		// Target, left in TRANSFER_SRC for readback.
		create_image(dst, kDstSize, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		REQUIRE(render_gfx_render_pass_init(&rgrp, &r, kFormat, VK_ATTACHMENT_LOAD_OP_CLEAR,
		                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
		REQUIRE(render_gfx_target_resources_init(&rtr, &r, &rgrp, dst.view, {kDstSize, kDstSize}));
		target_ready = true;

		REQUIRE(render_buffer_init(&vk(), &readback, VK_BUFFER_USAGE_TRANSFER_DST_BIT, kHostMemory,
		                           VkDeviceSize(kDstSize) * kDstSize * 4 * sizeof(uint16_t)) == VK_SUCCESS);
		REQUIRE(render_buffer_map(&vk(), &readback) == VK_SUCCESS);
	}

	~GfxFixture()
	{
		if (readback.buffer != VK_NULL_HANDLE) {
			render_buffer_fini(&vk(), &readback);
		}
		if (target_ready) {
			render_gfx_target_resources_fini(&rtr);
			render_gfx_render_pass_fini(&rgrp);
		}
		for (Image *image : {&src, &dst, &inset, &filtered}) {
			if (image->view != VK_NULL_HANDLE) {
				vk().vkDestroyImageView(vk().device, image->view, nullptr);
				vk().vkDestroyImage(vk().device, image->image, nullptr);
				vk().vkFreeMemory(vk().device, image->memory, nullptr);
			}
		}
		if (resources_ready) {
			render_resources_fini(&r);
		}
		if (shaders_ready) {
			render_shaders_fini(&shaders, &vk());
		}
		if (pool_ready) {
			vk_cmd_pool_destroy(&vk(), &pool);
		}
		free(hmd.distortion.mesh.vertices);
		free(hmd.distortion.mesh.indices);
	}

	// Uploads RGBA16 pixels into a new sampled image.
	void
	upload(Image &image, uint32_t size, const std::vector<uint16_t> &pixels)
	{
		create_image(image, size, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		const VkDeviceSize bytes = pixels.size() * sizeof(uint16_t);
		render_buffer staging{};
		REQUIRE(render_buffer_init(&vk(), &staging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, kHostMemory, bytes) ==
		        VK_SUCCESS);
		REQUIRE(render_buffer_map_and_write(&vk(), &staging, const_cast<uint16_t *>(pixels.data()), bytes) ==
		        VK_SUCCESS);
		VkCommandBuffer cmd = begin_cmd();
		barrier(cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		        VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {size, size, 1};
		vk().vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		                            &copy);
		barrier(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
		        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		finish_cmd(cmd);
		render_buffer_fini(&vk(), &staging);
	}

	// The filter is a pipeline specialization: rebuild the render pass and its
	// pipelines when it changes.
	void
	set_source_filter(uint32_t source_filter)
	{
		if (uint32_t(r.source_filter) == source_filter) {
			return;
		}
		render_gfx_target_resources_fini(&rtr);
		render_gfx_render_pass_fini(&rgrp);
		r.source_filter = render_source_filter(source_filter);
		REQUIRE(render_gfx_render_pass_init(&rgrp, &r, kFormat, VK_ATTACHMENT_LOAD_OP_CLEAR,
		                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL));
		REQUIRE(render_gfx_target_resources_init(&rtr, &r, &rgrp, dst.view, {kDstSize, kDstSize}));
	}

	// Mesh distortion of the source (or another image) with the quad-view
	// inset (if inset_fov is set) blended in; reads back R, G and B.
	std::vector<float>
	draw_quad(bool timewarp,
	          const xrt_fov *inset_fov,
	          VkImageView source = VK_NULL_HANDLE,
	          uint32_t source_filter = RENDER_SOURCE_FILTER_BILINEAR,
	          xrt_normalized_rect post_transform = {0.f, 0.f, 1.f, 1.f})
	{
		render_gfx render{};
		REQUIRE(render_gfx_init(&render, &r));
		REQUIRE(render_gfx_begin(&render));

		const VkSampler sampler = r.samplers.clamp_to_border_black;
		const xrt_fov fov = hmd.distortion.fov[0];
		render_gfx_mesh_ubo_data data{};
		data.vertex_rot = {{{1.f, 0.f, 0.f, 1.f}}};
		data.post_transform = post_transform;
		set_source_filter(source_filter);
		if (timewarp) {
			// Real timewarp matrices with a small yaw between scanout begin and end.
			const xrt_pose source = XRT_POSE_IDENTITY;
			xrt_pose begin = XRT_POSE_IDENTITY, end = XRT_POSE_IDENTITY;
			begin.orientation = {0.f, std::sin(.004f), 0.f, std::cos(.004f)};
			end.orientation = {0.f, std::sin(-.004f), 0.f, std::cos(-.004f)};
			render_calc_uv_to_tangent_lengths_rect(&fov, &data.pre_transform);
			render_calc_time_warp_matrix(&source, &fov, &begin, &data.transform_scanout_begin);
			render_calc_time_warp_matrix(&source, &fov, &end, &data.transform_scanout_end);
		}
		if (inset_fov != nullptr) {
			render_calc_inset_from_context_uv(&fov, inset_fov, &data.inset_from_context);
			data.inset_post_transform = {0.f, 0.f, 1.f, 1.f};
			data.has_inset.value = 1;
		}
		VkDescriptorSet set = VK_NULL_HANDLE;
		REQUIRE(render_gfx_mesh_alloc_and_write(&render, &data, nullptr, sampler,
		                                        source != VK_NULL_HANDLE ? source : src.view,
		                                        inset_fov ? sampler : VK_NULL_HANDLE,
		                                        inset_fov ? inset.view : VK_NULL_HANDLE, &set) == VK_SUCCESS);

		const VkClearColorValue clear = {{0.f, 0.f, 0.f, 1.f}};
		const render_viewport_data viewport = {0, 0, kDstSize, kDstSize};
		render_gfx_begin_target(&render, &rtr, &clear);
		render_gfx_begin_view(&render, 0, &viewport, &viewport);
		render_gfx_mesh_draw(&render, 0, set, timewarp);
		render_gfx_end_view(&render);
		render_gfx_end_target(&render);
		REQUIRE(render_gfx_end(&render));

		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &r.cmd;
		REQUIRE(vk().vkQueueSubmit(vk().main_queue->queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
		REQUIRE(vk().vkQueueWaitIdle(vk().main_queue->queue) == VK_SUCCESS);
		render_gfx_fini(&render);

		VkCommandBuffer cmd = begin_cmd();
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {kDstSize, kDstSize, 1};
		vk().vkCmdCopyImageToBuffer(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
		                            &copy);
		finish_cmd(cmd);

		const uint16_t *pixels = static_cast<const uint16_t *>(readback.mapped);
		std::vector<float> rgb(size_t(kDstSize) * kDstSize * 3);
		for (size_t i = 0; i < size_t(kDstSize) * kDstSize; ++i)
			for (int c = 0; c < 3; ++c)
				rgb[i * 3 + c] = pixels[i * 4 + c] / 65535.f;
		return rgb;
	}

	enum class Draw
	{
		Mesh,
		Projection,
	};

	// Draws the source into the whole target and reads back R and G.
	std::vector<float>
	draw(Draw what, const xrt_foveation_map_data *map)
	{
		render_gfx render{};
		REQUIRE(render_gfx_init(&render, &r));
		REQUIRE(render_gfx_begin(&render));

		const VkSampler sampler = r.samplers.clamp_to_border_black;
		const xrt_normalized_rect full = {0.f, 0.f, 1.f, 1.f};
		VkDescriptorSet set = VK_NULL_HANDLE;

		if (what == Draw::Mesh) {
			render_gfx_mesh_ubo_data data{};
			data.vertex_rot = {{{1.f, 0.f, 0.f, 1.f}}};
			data.post_transform = full;
			REQUIRE(render_gfx_mesh_alloc_and_write(&render, &data, map, sampler, src.view, VK_NULL_HANDLE,
			                                        VK_NULL_HANDLE, &set) == VK_SUCCESS);
		} else {
			// As comp_render_gfx.c's do_projection_layer, with the layer
			// at the view's own pose and fov so it fills the target.
			const xrt_fov fov = hmd.distortion.fov[0];
			render_gfx_layer_projection_data data{};
			data.post_transform = full;
			render_calc_uv_to_tangent_lengths_rect(&fov, &data.to_tangent);
			math_matrix_4x4_projection_vulkan_infinite_reverse(&fov, 0.1f, &data.mvp);
			data.color_scale = {1.f, 1.f, 1.f, 1.f};
			data.color_bias = {0.f, 0.f, 0.f, 0.f};
			REQUIRE(render_gfx_layer_projection_alloc_and_write(&render, &data, map, sampler, src.view,
			                                                    &set) == VK_SUCCESS);
		}

		const VkClearColorValue clear = {{0.f, 0.f, 0.f, 1.f}};
		const render_viewport_data viewport = {0, 0, kDstSize, kDstSize};
		render_gfx_begin_target(&render, &rtr, &clear);
		render_gfx_begin_view(&render, 0, &viewport, &viewport);
		if (what == Draw::Mesh) {
			render_gfx_mesh_draw(&render, 0, set, false);
		} else {
			render_gfx_layer_projection(&render, true, set);
		}
		render_gfx_end_view(&render);
		render_gfx_end_target(&render);
		REQUIRE(render_gfx_end(&render));

		VkSubmitInfo submit{};
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &r.cmd;
		REQUIRE(vk().vkQueueSubmit(vk().main_queue->queue, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
		REQUIRE(vk().vkQueueWaitIdle(vk().main_queue->queue) == VK_SUCCESS);
		render_gfx_fini(&render);

		VkCommandBuffer cmd = begin_cmd();
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {kDstSize, kDstSize, 1};
		vk().vkCmdCopyImageToBuffer(cmd, dst.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
		                            &copy);
		finish_cmd(cmd);

		const uint16_t *pixels = static_cast<const uint16_t *>(readback.mapped);
		std::vector<float> rg(size_t(kDstSize) * kDstSize * 2);
		for (size_t i = 0; i < size_t(kDstSize) * kDstSize; ++i) {
			rg[i * 2] = pixels[i * 4] / 65535.f;
			rg[i * 2 + 1] = pixels[i * 4 + 1] / 65535.f;
		}
		return rg;
	}
};

void
check_foveated_draw(GfxFixture &f, GfxFixture::Draw what)
{
	const xrt_foveation_map_data map = make_map();
	const std::vector<float> plain = f.draw(what, nullptr);
	const std::vector<float> foveated = f.draw(what, &map);

	// Without a map the draw samples the source linearly, so the plain
	// draw's colour is the source coordinate each pixel sampled. With the map
	// it must be that coordinate pushed through the map.
	const float tolerance = 1.5f / kSrcSize;
	float max_error = 0.f;
	float max_shift = 0.f;
	uint32_t checked = 0;
	for (uint32_t y = 1; y + 1 < kDstSize; ++y) {
		for (uint32_t x = 1; x + 1 < kDstSize; ++x) {
			const size_t i = (size_t(y) * kDstSize + x) * 2;
			const float u = plain[i];
			const float v = plain[i + 1];
			// Within half a texel of the edge the source gradient is
			// clamped, so the plain colour is not the coordinate.
			const float margin = 1.f / kSrcSize;
			if (u < margin || u > 1.f - margin || v < margin || v > 1.f - margin) {
				continue;
			}
			const float want_u = apply_map(map.x, u);
			const float want_v = apply_map(map.y, v);
			if (want_u < margin || want_u > 1.f - margin || want_v < margin || want_v > 1.f - margin) {
				continue;
			}
			max_error = std::fmax(max_error, std::fabs(foveated[i] - want_u));
			max_error = std::fmax(max_error, std::fabs(foveated[i + 1] - want_v));
			max_shift = std::fmax(max_shift, std::fabs(foveated[i] - u));
			++checked;
		}
	}

	INFO("checked " << checked << " pixels, max error " << max_error << ", max shift " << max_shift);
	CHECK(checked > kDstSize * kDstSize / 2);
	CHECK(max_error < tolerance);
	// The map must actually have moved the samples.
	CHECK(max_shift > 0.1f);
}

} // namespace

TEST_CASE("Graphics mesh distortion reconstructs foveated sources", "[aux_render][gpu][foveation]")
{
	GfxFixture f;
	f.init();
	check_foveated_draw(f, GfxFixture::Draw::Mesh);
}

TEST_CASE("Graphics projection layer reconstructs foveated sources", "[aux_render][gpu][foveation]")
{
	GfxFixture f;
	f.init();
	check_foveated_draw(f, GfxFixture::Draw::Projection);
}

TEST_CASE("Graphics draws without a map are unchanged by a disabled map", "[aux_render][gpu][foveation]")
{
	GfxFixture f;
	f.init();
	xrt_foveation_map_data disabled = make_map();
	disabled.enabled = 0;
	for (auto what : {GfxFixture::Draw::Mesh, GfxFixture::Draw::Projection}) {
		const std::vector<float> plain = f.draw(what, nullptr);
		const std::vector<float> off = f.draw(what, &disabled);
		CHECK(plain == off);
	}
}

TEST_CASE("Graphics mesh distortion blends a quad-view inset", "[aux_render][gpu]")
{
	GfxFixture f;
	f.init();
	const xrt_fov context = f.hmd.distortion.fov[0];
	const xrt_fov inset_fov = {-0.2f, 0.4f, 0.35f, -0.1f};
	xrt_normalized_rect a{};
	render_calc_inset_from_context_uv(&context, &inset_fov, &a);

	// Inset texels hold the context coordinate they show (so R and G should
	// match the plain draw) and a blue marker the source lacks.
	const uint32_t n = 128;
	std::vector<uint16_t> pixels(size_t(n) * n * 4);
	for (uint32_t y = 0; y < n; ++y)
		for (uint32_t x = 0; x < n; ++x) {
			double u = ((x + .5) / n - a.x) / a.w, v = ((y + .5) / n - a.y) / a.h;
			uint16_t *p = &pixels[(size_t(y) * n + x) * 4];
			p[0] = uint16_t(std::lround(std::fmin(std::fmax(u, 0.), 1.) * 65535.0));
			p[1] = uint16_t(std::lround(std::fmin(std::fmax(v, 0.), 1.) * 65535.0));
			p[2] = 65535;
			p[3] = 65535;
		}
	f.upload(f.inset, n, pixels);

	const double tl = std::tan(inset_fov.angle_left), tr = std::tan(inset_fov.angle_right);
	const double tu = std::tan(inset_fov.angle_up), td = std::tan(inset_fov.angle_down);
	const double cl = std::tan(context.angle_left), cr = std::tan(context.angle_right);
	const double cu = std::tan(context.angle_up), cd = std::tan(context.angle_down);
	for (bool timewarp : {false, true}) {
		CAPTURE(timewarp);
		const std::vector<float> plain = f.draw_quad(timewarp, nullptr);
		const std::vector<float> quad = f.draw_quad(timewarp, &inset_fov);
		uint32_t inside = 0, outside = 0;
		float gradient_error = 0.f, marker_error = 0.f, outside_error = 0.f;
		for (uint32_t y = 0; y < kDstSize; ++y)
			for (uint32_t x = 0; x < kDstSize; ++x) {
				// Where this pixel's direction falls in the inset, from angles only.
				double tan_x = cl + (x + .5) / kDstSize * (cr - cl);
				double tan_y = cu - (y + .5) / kDstSize * (cu - cd);
				double ix = (tan_x - tl) / (tr - tl), iy = (tu - tan_y) / (tu - td);
				size_t i = (size_t(y) * kDstSize + x) * 3;
				if (ix > .1 && ix < .9 && iy > .1 && iy < .9) {
					inside++;
					gradient_error = std::fmax(gradient_error, std::fabs(quad[i] - plain[i]));
					gradient_error =
					    std::fmax(gradient_error, std::fabs(quad[i + 1] - plain[i + 1]));
					marker_error = std::fmax(marker_error, std::fabs(quad[i + 2] - 1.f));
				} else if (ix < -.05 || ix > 1.05 || iy < -.05 || iy > 1.05) {
					outside++;
					for (int c = 0; c < 3; ++c)
						outside_error =
						    std::fmax(outside_error, std::fabs(quad[i + c] - plain[i + c]));
				}
			}
		INFO("inside " << inside << " outside " << outside << " gradient error " << gradient_error
		               << " marker error " << marker_error << " outside error " << outside_error);
		CHECK(inside > 400);
		CHECK(outside > 3000);
		CHECK(marker_error < 1e-3f);
		// Inset and source differ only by resampling: under 1.5 source texels.
		CHECK(gradient_error < 1.5f / kSrcSize);
		CHECK(outside_error == 0.f);
	}
}

TEST_CASE("Graphics mesh distortion sharpens magnified sources with Catmull-Rom", "[aux_render][gpu]")
{
	GfxFixture f;
	f.init();
	const uint32_t n = 12; // 96 / 12: 8x magnification.
	const size_t row = size_t(kDstSize) * 3;
	auto pixel = [&](const std::vector<float> &img, unsigned x, unsigned y, int c) {
		return img[y * row + size_t(x) * 3 + c];
	};

	SECTION("smooth gradients are reproduced without a shift")
	{
		std::vector<uint16_t> pixels(size_t(n) * n * 4);
		for (uint32_t y = 0; y < n; ++y)
			for (uint32_t x = 0; x < n; ++x) {
				uint16_t *p = &pixels[(size_t(y) * n + x) * 4];
				p[0] = uint16_t(x * 5000), p[1] = uint16_t(y * 5000), p[2] = 20000, p[3] = 65535;
			}
		f.upload(f.filtered, n, pixels);
		auto bilinear = f.draw_quad(false, nullptr, f.filtered.view, RENDER_SOURCE_FILTER_BILINEAR);
		auto cubic = f.draw_quad(false, nullptr, f.filtered.view, RENDER_SOURCE_FILTER_CATMULL_ROM);
		float max_diff = 0;
		for (unsigned y = 16; y < kDstSize - 16; ++y)
			for (unsigned x = 16; x < kDstSize - 16; ++x)
				for (int c = 0; c < 3; ++c)
					max_diff = std::fmax(
					    max_diff, std::fabs(pixel(bilinear, x, y, c) - pixel(cubic, x, y, c)));
		CHECK(max_diff < 0.005f);
		// Not a vacuous all-black draw.
		CHECK(pixel(cubic, kDstSize / 2, kDstSize / 2, 2) > 0.2f);
	}

	SECTION("hard edges get a steeper transition")
	{
		std::vector<uint16_t> pixels(size_t(n) * n * 4);
		for (uint32_t y = 0; y < n; ++y)
			for (uint32_t x = 0; x < n; ++x) {
				uint16_t *p = &pixels[(size_t(y) * n + x) * 4];
				p[0] = x < n / 2 ? 0 : 65535, p[1] = 30000, p[2] = 0, p[3] = 65535;
			}
		f.upload(f.filtered, n, pixels);
		auto measure = [&](const std::vector<float> &img, float &slope, unsigned &width) {
			slope = 0, width = 0;
			const unsigned y = kDstSize / 2;
			for (unsigned x = 0; x < kDstSize; ++x) {
				float v = pixel(img, x, y, 0);
				width += v > .1f && v < .9f;
				if (x > 0)
					slope = std::fmax(slope, v - pixel(img, x - 1, y, 0));
			}
		};
		float bilinear_slope, cubic_slope;
		unsigned bilinear_width, cubic_width;
		measure(f.draw_quad(false, nullptr, f.filtered.view, RENDER_SOURCE_FILTER_BILINEAR), bilinear_slope,
		        bilinear_width);
		measure(f.draw_quad(false, nullptr, f.filtered.view, RENDER_SOURCE_FILTER_CATMULL_ROM), cubic_slope,
		        cubic_width);
		CAPTURE(bilinear_slope, cubic_slope, bilinear_width, cubic_width);
		CHECK(cubic_slope > bilinear_slope * 1.15f);
		CHECK(cubic_width < bilinear_width);
	}

	SECTION("a view sharing its image never reads its neighbour")
	{
		// Left half blue, right half red; the view uses only the left half.
		const uint32_t m = 2 * n;
		std::vector<uint16_t> pixels(size_t(m) * m * 4);
		for (uint32_t y = 0; y < m; ++y)
			for (uint32_t x = 0; x < m; ++x) {
				uint16_t *p = &pixels[(size_t(y) * m + x) * 4];
				p[0] = x < n ? 0 : 65535, p[1] = 0, p[2] = x < n ? 65535 : 0, p[3] = 65535;
			}
		f.upload(f.filtered, m, pixels);
		auto cubic = f.draw_quad(false, nullptr, f.filtered.view, RENDER_SOURCE_FILTER_CATMULL_ROM,
		                         {0.f, 0.f, .5f, 1.f});
		float bleed = 0, blue = 0;
		for (unsigned y = 0; y < kDstSize; ++y)
			for (unsigned x = 0; x < kDstSize; ++x) {
				bleed = std::fmax(bleed, pixel(cubic, x, y, 0));
				blue = std::fmax(blue, pixel(cubic, x, y, 2));
			}
		CHECK(bleed == 0.f);
		CHECK(blue > .9f);
	}
}

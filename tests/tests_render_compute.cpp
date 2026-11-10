// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include <algorithm>
#include "render_compute_test_fixture.h"
#include "util/comp_projection_fast_path.h"

TEST_CASE("Compute timestamps remain ordered across repeated submissions", "[aux_render][gpu][timestamps]")
{
	Fixture f;
	f.init(67, 39);
	REQUIRE(f.queries != VK_NULL_HANDLE);
	f.config(0);
	auto pipeline = f.pipeline(variants[0], 0, false);
	for (unsigned round = 0; round < 5; ++round) {
		const uint64_t before = os_monotonic_get_ns();
		f.run(pipeline, variants[0], 4, true);
		const uint64_t after = os_monotonic_get_ns();
		uint64_t ticks[4]{};
		REQUIRE(f.vk().vkGetQueryPoolResults(
		            f.vk().device, f.queries, 0, 2, sizeof(ticks), ticks, 2 * sizeof(uint64_t),
		            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS);
		CAPTURE(round, ticks[0], ticks[2]);
		CHECK(ticks[1] != 0);
		CHECK(ticks[3] != 0);
		CHECK(ticks[0] != 0);
		CHECK(ticks[2] >= ticks[0]);
		const double duration = double(ticks[2] - ticks[0]) * f.vk().features.timestamp_period;
		CHECK(duration <= double(after - before));
	}
}

TEST_CASE("Adjacent compositor pass timestamps remain valid", "[aux_render][gpu][timestamps]")
{
	Fixture f;
	f.init(67, 39);
	REQUIRE(f.queries != VK_NULL_HANDLE);
	f.vk().vkDestroyQueryPool(f.vk().device, f.queries, nullptr);
	f.queries = VK_NULL_HANDLE;
	VkQueryPoolCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	info.queryType = VK_QUERY_TYPE_TIMESTAMP;
	info.queryCount = 6;
	REQUIRE(f.vk().vkCreateQueryPool(f.vk().device, &info, nullptr, &f.queries) == VK_SUCCESS);
	f.config(0);
	auto pipeline = f.pipeline(variants[0], 0, false);
	for (unsigned round = 0; round < 5; ++round) {
		auto cmd = f.begin();
		f.vk().vkCmdResetQueryPool(cmd, f.queries, 0, 6);
		for (unsigned query : {0u, 2u, 3u, 4u})
			f.vk().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.queries, query);
		f.vk().vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		f.vk().vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, f.layout, 0, 1, &f.set, 0, nullptr);
		f.vk().vkCmdDispatch(cmd, (f.width + 7) / 8, (f.height + 7) / 8, 2);
		for (unsigned query : {5u, 1u})
			f.vk().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, f.queries, query);
		f.finish(cmd);
		uint64_t ticks[12]{};
		REQUIRE(f.vk().vkGetQueryPoolResults(
		            f.vk().device, f.queries, 0, 6, sizeof(ticks), ticks, 2 * sizeof(uint64_t),
		            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) == VK_SUCCESS);
		uint64_t previous = 0;
		for (unsigned query : {0u, 2u, 3u, 4u, 5u, 1u}) {
			CAPTURE(round, query, ticks[query * 2], previous);
			CHECK(ticks[query * 2 + 1] != 0);
			CHECK(ticks[query * 2] != 0);
			CHECK(ticks[query * 2] >= previous);
			previous = ticks[query * 2];
		}
	}
}

TEST_CASE("Distortion workgroups preserve stereo pixels and feature paths", "[aux_render][gpu]")
{
	Fixture f;
	f.init(67, 39); // Both dimensions deliberately straddle workgroup boundaries.
	for (unsigned scenario = 0; scenario < 4; ++scenario)
		for (bool alpha : {false, true}) {
			CAPTURE(scenario, alpha);
			f.config(scenario);
			std::vector<uint8_t> reference;
			for (auto &v : variants) {
				CAPTURE(v.name);
				auto p = f.pipeline(v, scenario, alpha);
				f.run(p, v, 1, true);
				auto pixels = f.snapshot();
				if (reference.empty())
					reference = pixels;
				else
					REQUIRE(pixels == reference);
			}
			// Opaque output must cover every pixel, including partial edge workgroups.
			for (size_t i = 3; i < reference.size(); i += 4) {
				if (!alpha)
					REQUIRE(reference[i] == 255);
				else
					REQUIRE(reference[i] >= 32);
			}
			// Ensure the baseline isn't a vacuous all-black output.
			CHECK(reference[(size_t(19) * 134 + 33) * 4] > 0);
			CHECK(reference[(size_t(19) * 134 + 100) * 4] > 0);
		}
}

TEST_CASE("Identity distortion agrees with CPU color and alpha reference", "[aux_render][gpu]")
{
	Fixture f;
	f.init(67, 39);
	f.config(0);
	for (bool alpha : {false, true}) {
		auto p = f.pipeline(variants[0], 0, alpha, true);
		f.run(p, variants[0], 1, true);
		auto pixels = f.snapshot();
		unsigned max_error = 0;
		for (unsigned eye = 0; eye < 2; ++eye) {
			for (unsigned y = 0; y < f.height; ++y)
				for (unsigned x = 0; x < f.width; ++x) {
					unsigned source[] = {x * 255 / (f.width - 1), y * 255 / (f.height - 1),
					                     ((x / 7 + y / 5 + eye) % 2) ? 240u : 8u};
					size_t i = (size_t(y) * 2 * f.width + eye * f.width + x) * 4;
					for (unsigned c = 0; c < 3; ++c) {
						double linear = source[c] / 255.0;
						double srgb = linear < .0031308
						                  ? 12.92 * linear
						                  : 1.055 * std::pow(linear, 1.0 / 2.4) - .055;
						int expected = int(std::lround(srgb * 255));
						max_error = std::max(max_error,
						                     unsigned(std::abs(int(pixels[i + c]) - expected)));
					}
					unsigned expected_alpha = alpha ? 32 + ((x + y + eye * 59) % 224) : 255;
					max_error = std::max(
					    max_error, unsigned(std::abs(int(pixels[i + 3]) - int(expected_alpha))));
				}
		}
		// One code value accommodates float sampling and UNORM rounding differences.
		CHECK(max_error <= 1);
	}
}

namespace {
// Quad-view fast path: the inset images go in the depth source slots.
struct InsetSources
{
	Fixture &f;
	Image images[2]{};
	InsetSources(Fixture &fixture, uint32_t w, uint32_t h, const std::vector<uint8_t> &pixels) : f(fixture)
	{
		for (auto &image : images)
			f.create_image(image, {w, h}, VK_FORMAT_R8G8B8A8_UNORM, pixels.data(), pixels.size());
		VkDescriptorImageInfo infos[2];
		for (unsigned i = 0; i < 2; ++i)
			infos[i] = {f.sampler, images[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = f.set;
		write.dstBinding = 0;
		write.dstArrayElement = 2;
		write.descriptorCount = 2;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = infos;
		f.vk().vkUpdateDescriptorSets(f.vk().device, 1, &write, 0, nullptr);
	}
	~InsetSources()
	{
		f.vk().vkDeviceWaitIdle(f.vk().device);
		for (auto &i : images) {
			f.vk().vkDestroyImageView(f.vk().device, i.view, nullptr);
			f.vk().vkDestroyImage(f.vk().device, i.image, nullptr);
			f.vk().vkFreeMemory(f.vk().device, i.memory, nullptr);
		}
	}
};

xrt_fov
fov_degrees(float left, float right, float up, float down)
{
	const float d = float(M_PI / 180.0);
	return {left * d, right * d, up * d, down * d};
}

render_compute_distortion_ubo_data
inset_ubo(Fixture &f, bool timewarp, bool inset, const xrt_fov &context, const xrt_fov &inset_fov)
{
	render_compute_distortion_ubo_data data{};
	for (unsigned eye = 0; eye < 2; ++eye) {
		data.views[eye] = {eye * f.width, 0, f.width, f.height};
		render_calc_uv_to_tangent_lengths_rect(&context, &data.pre_transforms[eye]);
		data.post_transforms[eye] = {0, 0, 1, 1};
		// Real timewarp matrices; a small yaw so the context and inset move together.
		const xrt_pose source = XRT_POSE_IDENTITY;
		xrt_pose begin = XRT_POSE_IDENTITY, end = XRT_POSE_IDENTITY;
		if (timewarp) {
			begin.orientation = {0, std::sin(.004f), 0, std::cos(.004f)};
			end.orientation = {0, std::sin(-.004f), 0, std::cos(-.004f)};
		}
		render_calc_time_warp_matrix(&source, &context, &begin, &data.transform_timewarp_scanout_begin[eye]);
		render_calc_time_warp_matrix(&source, &context, &end, &data.transform_timewarp_scanout_end[eye]);
		data.has_depth[eye].has_inset = inset ? 1 : 0;
		render_calc_inset_from_context_uv(&context, &inset_fov, &data.inset_from_context[eye]);
		data.inset_post_transforms[eye] = {0, 0, 1, 1};
	}
	return data;
}
} // namespace

TEST_CASE("Quad-view inset blends over its context in the distortion pass", "[aux_render][gpu]")
{
	Fixture f;
	f.init(96, 80);
	// A tangent-space identity context (+-45 degrees) and an off-centre, asymmetric inset.
	const xrt_fov context = fov_degrees(-45, 45, 45, -45);
	const xrt_fov inset_fov = fov_degrees(-10, 25, 20, -5);
	xrt_normalized_rect a{};
	render_calc_inset_from_context_uv(&context, &inset_fov, &a);

	// Inset content: the context's red/green gradients resampled at the inset's
	// directions (so they should agree), with a blue marker that the context lacks.
	const uint32_t iw = 64, ih = 64;
	std::vector<uint8_t> pixels(size_t(iw) * ih * 4);
	for (uint32_t y = 0; y < ih; ++y)
		for (uint32_t x = 0; x < iw; ++x) {
			double u = ((x + .5) / iw - a.x) / a.w, v = ((y + .5) / ih - a.y) / a.h;
			double tx = u * f.width - .5, ty = v * f.height - .5;
			size_t i = (size_t(y) * iw + x) * 4;
			pixels[i] = uint8_t(std::clamp(std::lround(tx * 255 / (f.width - 1)), 0l, 255l));
			pixels[i + 1] = uint8_t(std::clamp(std::lround(ty * 255 / (f.height - 1)), 0l, 255l));
			pixels[i + 2] = 128;
			pixels[i + 3] = 255;
		}
	InsetSources sources(f, iw, ih, pixels);

	const double tl = std::tan(inset_fov.angle_left), tr = std::tan(inset_fov.angle_right);
	const double tu = std::tan(inset_fov.angle_up), td = std::tan(inset_fov.angle_down);
	for (bool timewarp : {false, true}) {
		CAPTURE(timewarp);
		auto p = f.pipeline(variants[0], timewarp ? 1 : 0, false, true);
		auto plain = inset_ubo(f, timewarp, false, context, inset_fov);
		REQUIRE(render_buffer_write(&f.vk(), &f.ubo, &plain, sizeof(plain)) == VK_SUCCESS);
		f.run(p, variants[0], 1, true);
		auto reference = f.snapshot();
		auto quad = inset_ubo(f, timewarp, true, context, inset_fov);
		REQUIRE(render_buffer_write(&f.vk(), &f.ubo, &quad, sizeof(quad)) == VK_SUCCESS);
		f.run(p, variants[0], 1, true);
		auto actual = f.snapshot();

		unsigned inside = 0, outside = 0, gradient_error = 0, marker_error = 0, outside_error = 0;
		for (unsigned eye = 0; eye < 2; ++eye)
			for (unsigned y = 0; y < f.height; ++y)
				for (unsigned x = 0; x < f.width; ++x) {
					// Where this output pixel's direction falls in the inset, from angles only.
					double tan_x = -1 + 2 * (x + .5) / f.width, tan_y = 1 - 2 * (y + .5) / f.height;
					double ix = (tan_x - tl) / (tr - tl), iy = (tu - tan_y) / (tu - td);
					size_t i = (size_t(y) * 2 * f.width + eye * f.width + x) * 4;
					auto diff = [&](unsigned c) {
						return unsigned(std::abs(int(actual[i + c]) - int(reference[i + c])));
					};
					if (ix > .1 && ix < .9 && iy > .1 && iy < .9) {
						inside++;
						gradient_error = std::max({gradient_error, diff(0), diff(1)});
						marker_error = std::max(marker_error,
						                        unsigned(std::abs(int(actual[i + 2]) - 188)));
					} else if (ix < -.05 || ix > 1.05 || iy < -.05 || iy > 1.05) {
						outside++;
						outside_error =
						    std::max({outside_error, diff(0), diff(1), diff(2), diff(3)});
					}
				}
		CAPTURE(inside, outside);
		CHECK(inside > 500);
		CHECK(outside > 5000);
		// sRGB(128/255) is 188; the gradients differ only by resampling and rounding.
		CHECK(marker_error <= 1);
		CHECK(gradient_error <= 3);
		CHECK(outside_error == 0);
	}
}

namespace {
// Replaces the two source slots (one per eye) with the given images.
struct SourceImages
{
	Fixture &f;
	Image images[2]{};
	SourceImages(Fixture &fixture, uint32_t w, uint32_t h, const std::vector<uint8_t> pixels[2]) : f(fixture)
	{
		for (unsigned i = 0; i < 2; ++i)
			f.create_image(images[i], {w, h}, VK_FORMAT_R8G8B8A8_UNORM, pixels[i].data(), pixels[i].size());
		VkDescriptorImageInfo infos[2];
		for (unsigned i = 0; i < 2; ++i)
			infos[i] = {f.sampler, images[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
		VkWriteDescriptorSet write{};
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = f.set;
		write.dstBinding = 0;
		write.dstArrayElement = 0;
		write.descriptorCount = 2;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = infos;
		f.vk().vkUpdateDescriptorSets(f.vk().device, 1, &write, 0, nullptr);
	}
	~SourceImages()
	{
		f.vk().vkDeviceWaitIdle(f.vk().device);
		for (auto &i : images) {
			f.vk().vkDestroyImageView(f.vk().device, i.view, nullptr);
			f.vk().vkDestroyImage(f.vk().device, i.image, nullptr);
			f.vk().vkFreeMemory(f.vk().device, i.memory, nullptr);
		}
	}
};

double
srgb_to_linear(uint8_t v)
{
	double c = v / 255.0;
	return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4);
}

// Identity distortion of the sources with the given filter and sub image rects.
std::vector<uint8_t>
run_filtered(Fixture &f, uint32_t filter, const xrt_normalized_rect rects[2])
{
	const xrt_fov fov = fov_degrees(-45, 45, 45, -45);
	auto data = inset_ubo(f, false, false, fov, fov);
	for (unsigned eye = 0; eye < 2; ++eye)
		data.post_transforms[eye] = rects[eye];
	REQUIRE(render_buffer_write(&f.vk(), &f.ubo, &data, sizeof(data)) == VK_SUCCESS);
	auto p = f.pipeline(variants[0], 0, false, true, filter);
	f.run(p, variants[0], 1, true);
	return f.snapshot();
}
} // namespace

TEST_CASE("Catmull-Rom source filter sharpens magnified sources in the distortion pass", "[aux_render][gpu]")
{
	Fixture f;
	f.init(96, 80);
	const uint32_t sw = 12, sh = 10; // 8x magnification.
	const xrt_normalized_rect full[2] = {{0, 0, 1, 1}, {0, 0, 1, 1}};

	SECTION("smooth gradients are reproduced without a shift")
	{
		// Red ramps along x, green along y: Catmull-Rom is exact for linear ramps.
		std::vector<uint8_t> pixels[2];
		for (auto &px : pixels) {
			px.resize(size_t(sw) * sh * 4);
			for (uint32_t y = 0; y < sh; ++y)
				for (uint32_t x = 0; x < sw; ++x) {
					uint8_t *q = &px[(size_t(y) * sw + x) * 4];
					q[0] = uint8_t(x * 20), q[1] = uint8_t(y * 25), q[2] = 64, q[3] = 255;
				}
		}
		SourceImages sources(f, sw, sh, pixels);
		auto bilinear = run_filtered(f, RENDER_SOURCE_FILTER_BILINEAR, full);
		auto cubic = run_filtered(f, RENDER_SOURCE_FILTER_CATMULL_ROM, full);
		unsigned max_diff = 0;
		// Away from the image border, where the ramp's ends are clamped differently.
		for (unsigned eye = 0; eye < 2; ++eye)
			for (unsigned y = 16; y < f.height - 16; ++y)
				for (unsigned x = 16; x < f.width - 16; ++x) {
					size_t i = (size_t(y) * 2 * f.width + eye * f.width + x) * 4;
					for (unsigned c = 0; c < 3; ++c)
						max_diff = std::max(max_diff, unsigned(std::abs(int(bilinear[i + c]) -
						                                                int(cubic[i + c]))));
				}
		CHECK(max_diff <= 2);
	}

	SECTION("hard edges get a steeper transition")
	{
		std::vector<uint8_t> pixels[2];
		for (auto &px : pixels) {
			px.resize(size_t(sw) * sh * 4);
			for (uint32_t y = 0; y < sh; ++y)
				for (uint32_t x = 0; x < sw; ++x) {
					uint8_t *q = &px[(size_t(y) * sw + x) * 4];
					q[0] = x < sw / 2 ? 0 : 255, q[1] = 128, q[2] = 0, q[3] = 255;
				}
		}
		SourceImages sources(f, sw, sh, pixels);
		auto measure = [&](const std::vector<uint8_t> &img, double &max_slope, unsigned &width) {
			max_slope = 0, width = 0;
			const unsigned y = f.height / 2;
			for (unsigned x = 0; x < f.width; ++x) {
				double v = srgb_to_linear(img[(size_t(y) * 2 * f.width + x) * 4]);
				width += v > .1 && v < .9;
				if (x > 0)
					max_slope = std::max(
					    max_slope, v - srgb_to_linear(img[(size_t(y) * 2 * f.width + x - 1) * 4]));
			}
		};
		double bilinear_slope, cubic_slope;
		unsigned bilinear_width, cubic_width;
		measure(run_filtered(f, RENDER_SOURCE_FILTER_BILINEAR, full), bilinear_slope, bilinear_width);
		measure(run_filtered(f, RENDER_SOURCE_FILTER_CATMULL_ROM, full), cubic_slope, cubic_width);
		CAPTURE(bilinear_slope, cubic_slope, bilinear_width, cubic_width);
		// Catmull-Rom's slope at a step is 1.25x bilinear's; its 10-90% width
		// (0.7 against 0.8 texels) can round to the same pixel count.
		CHECK(cubic_slope > bilinear_slope * 1.15);
		CHECK(cubic_width <= bilinear_width);
	}

	SECTION("the green-only mode sharpens green and leaves red bilinear")
	{
		std::vector<uint8_t> pixels[2];
		for (auto &px : pixels) {
			px.resize(size_t(sw) * sh * 4);
			for (uint32_t y = 0; y < sh; ++y)
				for (uint32_t x = 0; x < sw; ++x) {
					uint8_t *q = &px[(size_t(y) * sw + x) * 4];
					q[0] = q[1] = x < sw / 2 ? 0 : 255, q[2] = 0, q[3] = 255;
				}
		}
		SourceImages sources(f, sw, sh, pixels);
		auto bilinear = run_filtered(f, RENDER_SOURCE_FILTER_BILINEAR, full);
		auto green = run_filtered(f, RENDER_SOURCE_FILTER_CATMULL_ROM_GREEN, full);
		const unsigned y = f.height / 2;
		double bilinear_slope = 0, green_slope = 0;
		unsigned red_diff = 0;
		for (unsigned x = 1; x < f.width; ++x) {
			size_t i = (size_t(y) * 2 * f.width + x) * 4, j = i - 4;
			bilinear_slope =
			    std::max(bilinear_slope, srgb_to_linear(bilinear[i + 1]) - srgb_to_linear(bilinear[j + 1]));
			green_slope =
			    std::max(green_slope, srgb_to_linear(green[i + 1]) - srgb_to_linear(green[j + 1]));
			red_diff = std::max(red_diff, unsigned(std::abs(int(bilinear[i]) - int(green[i]))));
		}
		CAPTURE(bilinear_slope, green_slope);
		CHECK(green_slope > bilinear_slope * 1.15);
		CHECK(red_diff == 0);
	}

	SECTION("views sharing one image never read each other")
	{
		// One image: the left eye's half is blue, the right eye's half red.
		const uint32_t w = 2 * sw;
		std::vector<uint8_t> shared(size_t(w) * sh * 4);
		for (uint32_t y = 0; y < sh; ++y)
			for (uint32_t x = 0; x < w; ++x) {
				uint8_t *q = &shared[(size_t(y) * w + x) * 4];
				q[0] = x < sw ? 0 : 255, q[1] = 0, q[2] = x < sw ? 255 : 0, q[3] = 255;
			}
		std::vector<uint8_t> pixels[2] = {shared, shared};
		SourceImages sources(f, w, sh, pixels);
		const xrt_normalized_rect halves[2] = {{0, 0, .5f, 1}, {.5f, 0, .5f, 1}};
		auto cubic = run_filtered(f, RENDER_SOURCE_FILTER_CATMULL_ROM, halves);
		unsigned bleed = 0;
		for (unsigned eye = 0; eye < 2; ++eye)
			for (unsigned y = 0; y < f.height; ++y)
				for (unsigned x = 0; x < f.width; ++x) {
					size_t i = (size_t(y) * 2 * f.width + eye * f.width + x) * 4;
					bleed = std::max(bleed, unsigned(cubic[i + (eye == 0 ? 0 : 2)]));
				}
		CHECK(bleed == 0);
	}
}

// Quad views at PS VR2 size: the layer squasher route (context plus inset
// squashed at the 1.4x intermediate size, then distortion) against one
// distortion pass that blends the inset. Prints medians; no thresholds.
TEST_CASE("Quad-view composition GPU benchmark", "[.][aux_render][gpu][quad_benchmark]")
{
	const uint32_t out_w = 2000, out_h = 2040, scratch_w = 2800, scratch_h = 2856;
	auto median = [](std::vector<double> v) {
		std::sort(v.begin(), v.end());
		return v[v.size() / 2];
	};
	const unsigned rounds = 15, frames = 8;

	// Layer squasher: context then inset layer, per eye, at the intermediate size.
	double squash_ms = 0;
	{
		LayerFixture f;
		f.init(scratch_w, scratch_h);
		f.init_layers();
		if (!f.queries)
			SKIP("GPU timestamps unavailable");
		f.config_layers(1);
		for (unsigned eye = 0; eye < 2; ++eye) {
			render_compute_layer_ubo_data data{};
			data.view = {eye * scratch_w, 0, scratch_w, scratch_h};
			data.pre_transform = {-1, -1, 2, 2};
			data.layer_count.value = 2;
			for (unsigned l = 0; l < 2; ++l) {
				auto &layer = data.layers[l];
				layer.layer_data.layer_type = l == 0 ? LAYER_COMP_TYPE_CONTEXT : LAYER_COMP_TYPE_INSET;
				layer.image_info.color_image_index = l;
				layer.post_transforms = {0, 0, 1, 1};
				layer.color_scale = {1, 1, 1, 1};
				// The inset covers the central half in each direction.
				for (unsigned i = 0; i < 4; ++i)
					layer.transforms.v[i * 5] = (l == 1 && i < 2) ? 2.f : 1.f;
			}
			REQUIRE(render_buffer_write(&f.vk(), &f.layer_ubo[eye], &data, sizeof(data)) == VK_SUCCESS);
		}
		auto p = f.layer_pipeline(false, false, true);
		f.run_layers(p, 16, false);
		std::vector<double> ms;
		for (unsigned r = 0; r < rounds; ++r)
			ms.push_back(f.run_layers(p, frames, false));
		squash_ms = median(ms);
	}

	// Distortion: stereo (as after squashing) and quad (inset blended in).
	Fixture f;
	f.init(out_w, out_h);
	if (!f.queries)
		SKIP("GPU timestamps unavailable");
	std::vector<uint8_t> pixels(size_t(1400) * 1428 * 4, 200);
	InsetSources sources(f, 1400, 1428, pixels);
	const xrt_fov context = fov_degrees(-50, 45, 50, -52);
	const xrt_fov inset_fov = fov_degrees(-25, 22.5f, 25, -26);
	auto p = f.pipeline(variants[0], 1, false);
	double ms_by_mode[2]{};
	for (bool inset : {false, true}) {
		auto data = inset_ubo(f, true, inset, context, inset_fov);
		REQUIRE(render_buffer_write(&f.vk(), &f.ubo, &data, sizeof(data)) == VK_SUCCESS);
		f.run(p, variants[0], 16, false);
		std::vector<double> ms;
		for (unsigned r = 0; r < rounds; ++r)
			ms.push_back(f.run(p, variants[0], frames, false));
		ms_by_mode[inset] = median(ms);
	}
	std::printf(
	    "quad-view benchmark (ms per frame, both eyes): squash %u x %u x2 %.3f, "
	    "stereo distortion %u x %u x2 %.3f, squasher route %.3f, quad fast path %.3f\n",
	    scratch_w, scratch_h, squash_ms, out_w, out_h, ms_by_mode[0], squash_ms + ms_by_mode[0], ms_by_mode[1]);
}

// Source filter cost at PS VR2 output size with sources at 50% view scale
// (1400 x 1428 per eye, magnified by the distortion). Prints medians.
TEST_CASE("Source filter GPU benchmark", "[.][aux_render][gpu][source_filter_benchmark]")
{
	const uint32_t out_w = 2000, out_h = 2040, src_w = 1400, src_h = 1428;
	auto median = [](std::vector<double> v) {
		std::sort(v.begin(), v.end());
		return v[v.size() / 2];
	};
	Fixture f;
	f.init(out_w, out_h);
	if (!f.queries)
		SKIP("GPU timestamps unavailable");
	// Noise, so the extra taps miss the texture cache as real content would.
	std::vector<uint8_t> pixels[2];
	uint32_t seed = 12345;
	for (auto &px : pixels) {
		px.resize(size_t(src_w) * src_h * 4);
		for (auto &v : px) {
			seed = seed * 1664525u + 1013904223u;
			v = uint8_t(seed >> 24);
		}
	}
	SourceImages sources(f, src_w, src_h, pixels);
	InsetSources insets(f, src_w, src_h, pixels[0]);
	const xrt_fov context = fov_degrees(-50, 45, 50, -52);
	const xrt_fov inset_fov = fov_degrees(-25, 22.5f, 25, -26);
	VkPipeline ps[3] = {f.pipeline(variants[0], 1, false, false, 0), f.pipeline(variants[0], 1, false, false, 1),
	                    f.pipeline(variants[0], 1, false, false, 2)};
	double ms[2][3]{};
	for (int round = 0; round < 2; ++round) // The second round is the one reported.
		for (bool inset : {false, true})
			for (uint32_t filter : {0u, 1u, 2u}) {
				auto data = inset_ubo(f, true, inset, context, inset_fov);
				REQUIRE(render_buffer_write(&f.vk(), &f.ubo, &data, sizeof(data)) == VK_SUCCESS);
				f.run(ps[filter], variants[0], 16, false);
				std::vector<double> runs;
				for (unsigned r = 0; r < 15; ++r)
					runs.push_back(f.run(ps[filter], variants[0], 8, false));
				ms[inset][filter] = median(runs);
			}
	std::printf(
	    "source filter benchmark (ms per frame, both eyes, %ux%u from %ux%u): "
	    "stereo bilinear %.3f, Catmull-Rom %.3f, Catmull-Rom green %.3f; "
	    "quad bilinear %.3f, Catmull-Rom %.3f, Catmull-Rom green %.3f\n",
	    out_w, out_h, src_w, src_h, ms[0][0], ms[0][1], ms[0][2], ms[1][0], ms[1][1], ms[1][2]);
}

TEST_CASE("Distortion GPU workgroup benchmark", "[.][aux_render][gpu][benchmark]")
{
	const char *scenarios[] = {"distortion", "scanline-timewarp", "foveation", "depth"};
	// Explicit invocation only: -- benchmarks never impose timing thresholds on CI.
	Fixture f;
	bool use_small_extent = std::getenv("MONADO_DISTORTION_BENCH_SMALL") != nullptr;
	f.init(use_small_extent ? 1000 : 2000, use_small_extent ? 1020 : 2040);
	if (!f.queries)
		SKIP("GPU timestamps unavailable");
	const char *path = std::getenv("MONADO_DISTORTION_BENCH_CSV");
	REQUIRE(path != nullptr);
	auto close_file = [](FILE *f) { std::fclose(f); };
	std::unique_ptr<FILE, decltype(close_file)> file(std::fopen(path, "w"), close_file);
	REQUIRE(file != nullptr);
	FILE *csv = file.get();
	std::fprintf(csv, "scenario,alpha,width,height,round,workgroup,dispatches,gpu_ms\n");
	for (unsigned scenario = 0; scenario < 4; ++scenario) {
		f.config(scenario);
		std::array<VkPipeline, 3> ps{};
		std::vector<uint8_t> reference;
		for (unsigned i = 0; i < 3; ++i) {
			ps[i] = f.pipeline(variants[i], scenario, true);
			f.run(ps[i], variants[i], 16, true);
			auto pixels = f.snapshot();
			if (reference.empty())
				reference = pixels;
			else
				REQUIRE(pixels == reference);
		}
		// Forward/reverse order counterbalances thermal and ordering effects.
		for (unsigned round = 0; round < 12; ++round)
			for (unsigned j = 0; j < 3; ++j) {
				unsigned i = round % 2 ? 2 - j : j;
				double ms = f.run(ps[i], variants[i], 8, false);
				CHECK(std::isfinite(ms));
				CHECK(ms > 0);
				std::fprintf(csv, "%s,1,%u,%u,%u,%s,8,%.6f\n", scenarios[scenario], 2 * f.width,
				             f.height, round, variants[i].name, ms);
			}
	}
}

TEST_CASE("Quad bounds conservatively handle invalid and eye-plane inputs", "[aux_render]")
{
	xrt_matrix_4x4 plane{};
	for (unsigned i = 0; i < 4; ++i)
		plane.v[i * 5] = 1;
	plane.v[14] = -1;
	xrt_vec2 size{1, 1};
	xrt_normalized_rect pre{-1, -1, 2, 2};
	float bounds[4]{};
	render_quad_view_bounds(&plane, &size, &pre, 100, 100, bounds);
	CHECK(bounds[0] < .25f);
	CHECK(bounds[1] < .25f);
	CHECK(bounds[2] > .75f);
	CHECK(bounds[3] > .75f);
	for (unsigned invalid = 0; invalid < 5; ++invalid) {
		auto m = plane;
		auto extent = size;
		auto transform = pre;
		if (invalid == 0)
			m.v[14] = 0;
		if (invalid == 1)
			m.v[14] = 1;
		if (invalid == 2)
			m.v[12] = NAN;
		if (invalid == 3)
			extent.x = -1;
		if (invalid == 4)
			transform.w = 0;
		render_quad_view_bounds(&m, &extent, &transform, 100, 100, bounds);
		CHECK(bounds[0] == 0);
		CHECK(bounds[1] == 0);
		CHECK(bounds[2] == 1);
		CHECK(bounds[3] == 1);
	}
}

TEST_CASE("Quad bounds preserve layer composition", "[aux_render][gpu]")
{
	LayerFixture f;
	f.init(67, 39);
	f.init_layers();
	for (bool correction : {false, true})
		for (bool timewarp : {false, true}) {
			auto base = f.layer_pipeline(false, correction, timewarp);
			auto candidate = f.layer_pipeline(true, correction, timewarp);
			for (unsigned pose = 0; pose < 29; ++pose)
				for (unsigned flags = 0; flags < 4; ++flags) {
					CAPTURE(correction, timewarp, pose, flags);
					f.config_layers(3, pose, flags);
					f.run_layers(base, 1, true);
					auto reference = f.snapshot();
					f.run_layers(candidate, 1, true);
					auto actual = f.snapshot();
					unsigned max_error = 0;
					for (size_t i = 0; i < actual.size(); ++i)
						max_error = std::max(
						    max_error, unsigned(std::abs(int(actual[i]) - int(reference[i]))));
					REQUIRE(max_error == 0);
				}
		}
}

TEST_CASE("Layer composition GPU benchmark", "[.][aux_render][gpu][layer_benchmark]")
{
	LayerFixture f;
	bool use_small_extent = std::getenv("MONADO_DISTORTION_BENCH_SMALL") != nullptr;
	f.init(use_small_extent ? 1000 : 2000, use_small_extent ? 1020 : 2040);
	f.init_layers();
	if (!f.queries)
		SKIP("GPU timestamps unavailable");
	const char *path = std::getenv("MONADO_LAYER_BENCH_CSV");
	REQUIRE(path != nullptr);
	auto close_file = [](FILE *f) { std::fclose(f); };
	std::unique_ptr<FILE, decltype(close_file)> file(std::fopen(path, "w"), close_file);
	REQUIRE(file != nullptr);
	std::fprintf(file.get(),
	             "quads,correction,width,height,round,variant,frames,gpu_ms,host_start_ns,host_end_ns\n");
	for (bool correction : {false, true}) {
		auto base = f.layer_pipeline(false, correction);
		auto candidate = f.layer_pipeline(true, correction);
		for (unsigned quads : {0u, 1u, 4u}) {
			f.config_layers(quads);
			f.run_layers(base, 16, true);
			auto reference = f.snapshot();
			f.run_layers(candidate, 16, true);
			auto actual = f.snapshot();
			unsigned max_error = 0;
			for (size_t i = 0; i < actual.size(); ++i)
				max_error = std::max(max_error, unsigned(std::abs(int(actual[i]) - int(reference[i]))));
			REQUIRE(max_error == 0);
			for (unsigned round = 0; round < 12; ++round)
				for (unsigned j = 0; j < 2; ++j) {
					bool optimized = (round % 2 ? 1 - j : j) != 0;
					uint64_t start_ns = os_monotonic_get_ns();
					double ms = f.run_layers(optimized ? candidate : base, 8, false);
					uint64_t end_ns = os_monotonic_get_ns();
					REQUIRE(std::isfinite(ms));
					REQUIRE(ms > 0);
					std::fprintf(file.get(), "%u,%u,%u,%u,%u,%s,8,%.6f,%llu,%llu\n", quads,
					             correction ? 1 : 0, 2 * f.width, f.height, round,
					             optimized ? "candidate" : "baseline", ms,
					             (unsigned long long)start_ns, (unsigned long long)end_ns);
				}
		}
	}
}

// A separate process/device submits independently to the same physical GPU.
// This is intentionally hidden, finite and stopped by the runner's own file.
TEST_CASE("Competing GPU load", "[.][aux_render][gpu][gpu_load]")
{
	Fixture f;
	f.init(2000, 2040);
	f.config(2);
	if (!f.queries)
		SKIP("GPU timestamps unavailable");
	auto pipeline = f.pipeline(variants[0], 2, true);
	f.run(pipeline, variants[0], 16, false);
	const char *path = std::getenv("MONADO_GPU_LOAD_CSV");
	const char *stop = std::getenv("MONADO_GPU_LOAD_STOP");
	const char *mode = std::getenv("MONADO_GPU_LOAD_MODE");
	REQUIRE(path != nullptr);
	REQUIRE(stop != nullptr);
	REQUIRE(mode != nullptr);
	bool paced = std::strcmp(mode, "paced") == 0;
	REQUIRE((paced || std::strcmp(mode, "saturated") == 0));
	auto close_file = [](FILE *f) { std::fclose(f); };
	std::unique_ptr<FILE, decltype(close_file)> file(std::fopen(path, "w"), close_file);
	REQUIRE(file != nullptr);
	std::fprintf(file.get(), "batch,dispatches,host_start_ns,host_end_ns,gpu_batch_ms\n");
	std::fflush(file.get());
	uint64_t start_ns = os_monotonic_get_ns();
	unsigned seconds = 30;
	if (const char *duration = std::getenv("MONADO_GPU_LOAD_SECONDS")) {
		char *end = nullptr;
		unsigned long parsed = std::strtoul(duration, &end, 10);
		REQUIRE(end != duration);
		REQUIRE(*end == '\0');
		REQUIRE(parsed >= 1);
		REQUIRE(parsed <= 180);
		seconds = static_cast<unsigned>(parsed);
	}
	uint64_t deadline_ns = start_ns + uint64_t(seconds) * 1000000000ull;
	unsigned batch = 0;
	while (uint64_t(os_monotonic_get_ns()) < deadline_ns && !std::filesystem::exists(stop)) {
		uint64_t before = os_monotonic_get_ns();
		unsigned count = paced ? 3 : 4;
		double ms = f.run(pipeline, variants[0], count, false) * count;
		uint64_t after = os_monotonic_get_ns();
		REQUIRE(std::isfinite(ms));
		REQUIRE(ms > 0);
		std::fprintf(file.get(), "%u,%u,%llu,%llu,%.6f\n", batch++, count, (unsigned long long)before,
		             (unsigned long long)after, ms);
		std::fflush(file.get());
		// Nominal ~4 ms work every 8.33 ms; under contention a batch may
		// overrun. Never accumulate catch-up submissions or claim a fixed share.
		uint64_t now = os_monotonic_get_ns();
		if (paced && now < before + 8333333ull)
			os_nanosleep(before + 8333333ull - now);
	}
	REQUIRE(batch > 0);
}

TEST_CASE("Projection fast-path diagnostic identifies all policy gates", "[aux_render]")
{
	xrt_layer_data layer{};
	layer.type = XRT_LAYER_PROJECTION;
	auto reasons = [&](unsigned count, bool passthrough = false, bool peek = false, bool mirror = false,
	                   bool disabled = false) {
		return comp_projection_fast_path_rejections(count, count ? &layer : nullptr, passthrough, peek, mirror,
		                                            disabled);
	};
	CHECK(reasons(1) == 0);
	layer.flags = XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	CHECK(reasons(1) == 0); // Alpha handling is already supported.
	layer.flags = (xrt_layer_composition_flags)(layer.flags | XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT);
	CHECK(reasons(1) == 0);
	layer.flags = (xrt_layer_composition_flags)0;
	CHECK(reasons(0) == COMP_FAST_PATH_LAYER_COUNT);
	CHECK(reasons(2) == COMP_FAST_PATH_LAYER_COUNT);
	CHECK(reasons(1, true) == COMP_FAST_PATH_PASSTHROUGH);
	CHECK(reasons(1, false, true) == COMP_FAST_PATH_PEEK);
	CHECK(reasons(1, false, false, true) == COMP_FAST_PATH_DEBUG_MIRROR);
	CHECK(reasons(1, false, false, false, true) == COMP_FAST_PATH_DEBUG_DISABLED);
	CHECK(reasons(1, true, true, true, true) == (COMP_FAST_PATH_PASSTHROUGH | COMP_FAST_PATH_PEEK |
	                                             COMP_FAST_PATH_DEBUG_MIRROR | COMP_FAST_PATH_DEBUG_DISABLED));
	layer.flags = XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE;
	CHECK(reasons(1) == COMP_FAST_PATH_COLOR_SCALE_BIAS);
	layer.flags = (xrt_layer_composition_flags)0;
	layer.proj.chroma_key.curve = .5f;
	CHECK(reasons(1) == COMP_FAST_PATH_CHROMA_KEY);
	layer.proj.chroma_key.curve = 0;
	layer.proj.v[0].foveation.enabled = 1;
	CHECK(reasons(1) == 0); // Supported by distortion-only rendering.
	layer.type = XRT_LAYER_PROJECTION_DEPTH;
	layer.depth.chroma_key.curve = .5f;
	CHECK(reasons(1) == COMP_FAST_PATH_CHROMA_KEY);
	layer.depth.chroma_key.curve = 0;
	CHECK(reasons(1) == 0);
	layer.type = XRT_LAYER_QUAD;
	CHECK(reasons(1) == COMP_FAST_PATH_LAYER_TYPE);
}

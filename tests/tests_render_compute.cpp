// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "render_compute_test_fixture.h"
#include "util/comp_projection_fast_path.h"

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

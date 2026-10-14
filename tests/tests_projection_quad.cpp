// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "projection_quad_test_fixture.h"
#include <algorithm>

TEST_CASE("Projection quad two-pass rendering preserves bounded stereo pixels", "[aux_render][gpu]")
{
	for (float scale : {1.f, 1.4f}) {
		ProjectionQuadFixture f;
		f.init_composition(67, 39, scale);
		for (bool timewarp : {false, true}) {
			auto original = f.layer_pipeline(false, true, timewarp);
			auto bounded = f.layer_pipeline(true, true, timewarp);
			for (bool alpha : {false, true}) {
				auto dist = f.pipeline(variants[0], timewarp ? 1 : 0, alpha);
				for (unsigned pose : {0u, 3u, 17u, 19u, 20u, 21u}) {
					for (unsigned flags : {0u, 1u, 3u}) {
						CAPTURE(scale, timewarp, alpha, pose, flags);
						f.scene(pose, flags, timewarp ? 1 : 0);
						f.run_composition(original, dist, 1, true);
						auto pixels = f.snapshot();
						f.run_composition(bounded, dist, 1, true);
						REQUIRE(f.snapshot() == pixels);
						bool has_rgb = false;
						for (size_t i = 0; i < pixels.size(); ++i)
							has_rgb |= i % 4 != 3 && pixels[i] != 0;
						CHECK(has_rgb);
						if (!alpha) {
							for (size_t i = 3; i < pixels.size(); i += 4)
								REQUIRE(pixels[i] == 255);
						}
					}
				}
			}
		}
	}
}

TEST_CASE("Projection quad full pipeline GPU benchmark", "[.][gpu][projection_quad_benchmark]")
{
	const char *path = std::getenv("MONADO_PROJECTION_QUAD_CSV");
	REQUIRE(path != nullptr);
	auto close_file = [](FILE *f) { std::fclose(f); };
	std::unique_ptr<FILE, decltype(close_file)> file(std::fopen(path, "w"), close_file);
	REQUIRE(file != nullptr);
	std::fprintf(file.get(), "scale,round,variant,total_ms,host_start_ns,host_end_ns\n");
	for (float scale : {1.f, 1.4f}) {
		ProjectionQuadFixture f;
		f.init_composition(2000, 2040, scale);
		if (!f.queries)
			SKIP("GPU timestamps unavailable");
		f.scene(0, 1, 1);
		auto original = f.layer_pipeline(false, true, true);
		auto bounded = f.layer_pipeline(true, true, true);
		auto dist = f.pipeline(variants[0], 1, false);
		f.run_composition(original, dist, 4, false);
		f.run_composition(bounded, dist, 4, false);
		for (unsigned round = 0; round < 12; ++round) {
			for (unsigned n = 0; n < 2; ++n) {
				bool candidate = (round % 2 ? 1 - n : n) != 0;
				uint64_t begin_ns = os_monotonic_get_ns();
				double elapsed = f.run_composition(candidate ? bounded : original, dist, 6, false);
				uint64_t end_ns = os_monotonic_get_ns();
				std::fprintf(file.get(), "%.2f,%u,%s,%.9f,%llu,%llu\n", scale, round,
				             candidate ? "bounds" : "baseline", elapsed, (unsigned long long)begin_ns,
				             (unsigned long long)end_ns);
				std::fflush(file.get());
			}
		}
	}
}

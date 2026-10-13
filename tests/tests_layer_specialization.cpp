// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "projection_quad_test_fixture.h"
#include "render/render_layer_specialization.h"
#include <limits>

TEST_CASE("Common projection quad shader preserves final stereo pixels", "[aux_render][gpu][layer_specialization]")
{
	for (float scale : {1.f, 1.4f}) {
		ProjectionQuadFixture f;
		f.init_composition(67, 39, scale, 16);
		for (bool timewarp : {false, true}) {
			for (bool correction : {false, true}) {
				auto baseline = f.layer_pipeline(true, correction, timewarp);
				auto candidate = f.layer_pipeline(true, correction, timewarp, true);
				for (bool alpha : {false, true}) {
					auto distortion = f.pipeline(variants[0], timewarp ? 1 : 0, alpha);
					for (unsigned quads : {1u, 4u}) {
						for (unsigned pose : {0u, 3u, 17u, 19u, 20u, 21u, 22u, 23u, 25u, 28u}) {
							for (unsigned flags : {0u, 1u, 2u, 3u}) {
								CAPTURE(scale, timewarp, correction, alpha, quads, pose, flags);
								f.scene(pose, flags, timewarp ? 1 : 0, quads);
								f.run_composition(baseline, distortion, 1, true);
								auto pixels = f.snapshot();
								f.run_composition(candidate, distortion, 1, true);
								REQUIRE(f.snapshot() == pixels);
							}
						}
					}
				}
			}
		}
	}
}

TEST_CASE("Common projection quad pipeline GPU benchmark", "[.][gpu][layer_specialization_benchmark]")
{
	const char *path = std::getenv("MONADO_LAYER_SPECIAL_CSV"); REQUIRE(path);
	std::unique_ptr<FILE, decltype(&std::fclose)> file(std::fopen(path, "w"), &std::fclose);
	REQUIRE(file);
	std::fprintf(file.get(), "scale,quads,round,variant,total_ms,host_start_ns,host_end_ns\n");
	for (float scale : {1.f, 1.4f}) {
		ProjectionQuadFixture f;
		f.init_composition(2000, 2040, scale, 16);
		if (!f.queries) SKIP("GPU timestamps unavailable");
		auto baseline = f.layer_pipeline(true, true, true);
		auto candidate = f.layer_pipeline(true, true, true, true);
		auto distortion = f.pipeline(variants[0], 1, false);
		for (unsigned quads : {1u, 4u}) {
			f.scene(0, 1, 1, quads);
			f.run_composition(baseline, distortion, 8, false);
			f.run_composition(candidate, distortion, 8, false);
			for (unsigned round = 0; round < 12; ++round) {
				for (unsigned n = 0; n < 2; ++n) {
					bool special = (round % 2 ? 1 - n : n) != 0;
					uint64_t begin = os_monotonic_get_ns();
					double ms = f.run_composition(special ? candidate : baseline, distortion, 6, false);
					uint64_t end = os_monotonic_get_ns();
					std::fprintf(file.get(), "%.2f,%u,%u,%s,%.9f,%llu,%llu\n", scale, quads, round,
					             special ? "specialized" : "baseline", ms, (unsigned long long)begin, (unsigned long long)end);
					std::fflush(file.get());
				}
			}
		}
	}
}

TEST_CASE("Common layer eligibility rejects unsupported shader inputs", "[aux_render]")
{
	render_compute_layer_ubo_data data{};
	render_compute_layer_foveation_ubo_data foveation{};
	data.layer_count.value = 2;
	data.layers[0].layer_data.layer_type = LAYER_COMP_TYPE_PROJECTION;
	data.layers[1].layer_data.layer_type = LAYER_COMP_TYPE_QUAD;
	REQUIRE(render_layer_common_eligible(&data, &foveation));
	for (uint32_t count : {0u, 1u, RENDER_MAX_LAYERS + 1u}) {
		data.layer_count.value = count;
		CHECK_FALSE(render_layer_common_eligible(&data, &foveation));
	}
	data.layer_count.value = 2;
	for (uint32_t type : {LAYER_COMP_TYPE_NOOP, LAYER_COMP_TYPE_CYLINDER, LAYER_COMP_TYPE_EQUIRECT2,
	                      LAYER_COMP_TYPE_CONTEXT, LAYER_COMP_TYPE_INSET, LAYER_COMP_TYPE_PROJECTION}) {
		data.layers[1].layer_data.layer_type = type;
		CHECK_FALSE(render_layer_common_eligible(&data, &foveation));
	}
	data.layers[1].layer_data.layer_type = LAYER_COMP_TYPE_QUAD;
	data.layers[0].layer_data.layer_type = LAYER_COMP_TYPE_QUAD;
	CHECK_FALSE(render_layer_common_eligible(&data, &foveation));
	data.layers[0].layer_data.layer_type = LAYER_COMP_TYPE_PROJECTION;
	for (unsigned i : {0u, 1u}) {
		data.layers[i].image_info.has_depth = 1;
		CHECK_FALSE(render_layer_common_eligible(&data, &foveation));
		data.layers[i].image_info.has_depth = 0;
		foveation.layer_slot[i] = 1;
		CHECK_FALSE(render_layer_common_eligible(&data, &foveation));
		foveation.layer_slot[i] = 0;
		for (float curve : {.5f, std::numeric_limits<float>::quiet_NaN()}) {
			data.layers[i].chroma_key.curve = curve;
			CHECK_FALSE(render_layer_common_eligible(&data, &foveation));
		}
		data.layers[i].chroma_key.curve = 0;
	}
	CHECK(render_layer_common_eligible(&data, &foveation));
}

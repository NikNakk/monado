<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Compositor GPU efficiency — 2026-10-13

Reduce GPU work without changing resolution, chromatic correction, scanline
timewarp, filtering, quantization or blending. Keep the existing CA-copy
presentation and bounded submission flow. Two shader improvements are retained
**opt-in, default off**; neither is a demonstrated fix for WindowServer bursts.
Linux runtime selection remains unchanged.

Measurements use Apple M5/MoltenVK, PS VR2 at 4000x2040/120 Hz, and base commit
`00bde2f05` plus the then-uncommitted changes. They **predate** the subsequently
committed Metal GPU priority option. Claude's separate native UE priority trial
is documented in [timing diagnostics](macos-psvr2-timing-diagnostics.md).
Do not combine the reported gains or infer game performance from synthetic load.
Raw captures and removed prototypes are not tracked; cleanup preserved a local
archive at `/tmp/monado-gpu-cleanup-cd592fb91`. Temporary files are not durable
project evidence; the conclusions and limitations below are the retained record.

## Retained improvements

| Change | Evidence | Gate / default |
| --- | --- | --- |
| Quad bounds | Conservative per-eye UV rectangle rejects impossible quad hits before ray/plane arithmetic. Exact pixel checks include alpha, flipped UVs, backfaces, tiny quads and eye-plane crossings. Composition-only savings ~8% with one quad / ~17% with four idle, ~12% with four under contention. Two-pass savings ~3–6% idle / ~2–4% loaded. | `XRT_MACOS_COMPOSITOR_QUAD_BOUNDS=1`, default off; visual acceptance pending. |
| Common layer specialization | Production projection-plus-quads shader preserves all 1,280 tested final images. Whole-pipeline saving ~18–23% idle / ~14–21% loaded offscreen; synthetic loaded headset cadence improves ~93–95 → ~103–104 Hz. | `XRT_MACOS_COMPOSITOR_COMMON_LAYERS=1`, default off; [eligibility and client applicability](macos-common-layer-specialization.md). |
| Buffered GPU duration / pass profiling | Whole-frame duration uses retired queries even without calibrated host timestamps. Optional extra brackets separate composition/distortion. Results are read after the existing fence; no new CPU wait. | `PSVR2_TIMING_TRACE=1`; extra brackets require `XRT_MACOS_GPU_PASS_TIMING=1`, default off. |
| Instruments copy labels | `MonadoCopy frame=… timeline=… image=…` joins Metal copy commands to Monado work. Trace frame IDs and renderer timeline values differ. | Existing `XRT_MACOS_SIGNPOST=1`; inactive otherwise. |

The original 8x8 workgroup is retained. Distortion-only idle baseline medians
are ~0.653 ms ordinary, 0.873 ms scanline timewarp, 1.371 ms foveation and
1.163 ms synthetic depth. Offscreen maps/sources are deterministic synthetic
inputs; the depth case uses the bound color image's red channel.

## Rejected experiments

| Experiment | Result and decision |
| --- | --- |
| 16x8 / 16x16 workgroups; explicit green-alpha sample reuse | No consistent gain; timewarp/depth often regress. Keep 8x8 and original sampling. Workgroup comparisons remain test-only diagnostics. |
| Direct drawable, avoiding final copy | Offscreen GPU-stage saving ~36–42% idle / 8–13% loaded. Opportunistic prefetch does not sustain direct use; waiting drops physical cadence to ~72 Hz. Remove implementation and diagnostic probes. |
| Callback-owned direct output, preferred latency 1 versus 2 | Direct use is sustained and GPU work shrinks, but idle cadence is ~110–115 Hz and loaded ~85–89 Hz versus copy controls ~119 and ~97–104 Hz. Both latency settings have the same observed one-refresh deadline horizon. Remove backend. |
| Earlier retirement / explicit present handler | Twenty one-queue four-quad captures still lose loaded cadence with opaque driven layers (~83–90 Hz). Non-opaque layers reach ~109–111 Hz but add ~7–8 ms commit-to-display buffering. Remove backend; keep copy. |
| Two renderer resource sets on one queue | Removes CPU fence waiting, but saturated cadence falls ~98–104 → ~76 Hz and buffering gains a refresh. Instruments producer queue delay rises ~0.58 → ~4.35 ms. Remove prototype; preserve backpressure and source-reuse protection. |
| Projection/quad fusion by direct source sampling | Skips intermediate quantization/filtering: 54 of 72 cases fail the one-code-value gate, maximum error 169/255. Idle performance worsens; a loaded ~5–6% gain at scale 1.4 fails quality. Remove candidate. |
| Lossless float32 distortion-map packing | Red/green RGBA32 plus blue RG32 preserves pixels but regresses ordinary distortion ~6–9% and timewarp ~2–8%; foveation unchanged, depth gain unreproduced. Remove candidate; no precision reduction or green-coordinate reuse. |

Closing the lid removes light-stage late presentations in a standalone native
Metal/event/Vulkan probe. A heavier producer instead reaches ~93 Hz with the
panel off versus ~119 Hz on, where the presentation horizon gains a refresh.
This demonstrates display-topology/policy influence, not exact WindowServer
latch attribution or a validated replacement compositor.

## Pass and Instruments findings

With four quads, bounds and normal runtime scale, diagnostic median brackets
are ~2.76–2.82 ms composition and ~1.29–1.30 ms distortion idle; under saturated
load they are ~2.09–2.15 and ~3.59 ms, with ~1.63–1.75 ms outside brackets.
Separate medians are not additive. Queries alter timing: idle whole-frame
intervals shift ~4.63–4.86 → ~4.19–4.27 ms while cadence stays near 120 Hz.
Use profiling for diagnosis, and query-off controls for performance claims.

Instruments resolves present API calls, GPU execution and queue delays. Explicit
present handlers call the API about 1 µs after entry, yet commit-to-call can
include ~7.8 ms awaiting producer completion. Private WindowServer acceptance
and exact shared-event signal times remain unavailable. One direct trace loses
63 signpost messages; it is diagnostic rather than the acceptance comparison.
The final-copy GPU execution (~0.58 ms) is smaller than observed consumer queue
wait (~6.39 ms) under contention; removing the copy alone does not remove that
scheduling delay. Counter/encoder classification is workload-specific.

## Reproduce retained checks

Build in the existing `build/arm64` directory; run `tests_render_compute` for
pixel regressions. Hidden timing cases never impose CI performance thresholds:

| Test tag / runner | Purpose |
| --- | --- |
| `[benchmark]`, `summarize-distortion-benchmark.py` | Distortion workgroups / feature-path GPU cost; `MONADO_DISTORTION_BENCH_CSV`. |
| `[layer_benchmark]`, `run-compositor-gpu-contention.py` | Bounds composition cost with independent idle/paced/saturated load. |
| `run-projection-quad-benchmark.py` | Bounds through the complete production two-pass pipeline, scales 1/1.4. |
| `run-layer-specialization-benchmark.py` | Generic versus common-layer shader with bounds enabled in both. |
| `run-headset-gpu-load.py`, `run-compositor-pass-profile.py` | Current CA-copy physical cadence / optional pass profiling with four quads. |
| `run-common-layer-headset.py` | Projection-plus-four-quads runtime comparison; verifies actual common dispatch. |
| `analyze-compositor-copy-instruments.py` | Analyze exported Instruments commands using copy labels and workload-specific encoders. |

Pass an output directory **outside the checkout**, e.g. `--output /tmp/new-run`.
Runners bound and stop load workers, require same-GPU metadata, and validate
steady physical callbacks / competing-load coverage. GPU timestamps measure
elapsed execution and contention, not exclusive GPU occupancy. Batch tails are
not individual frame p95/p99; synthetic workloads are not games. Headset captures
hold display-awake assertions, which do not override proximity firmware.

## Cleanup validation

After removing the rejected probes, the full arm64 build and **all 73 CTests**
pass. The bounds-only intermediate commit also passes compute/cache tests; the
profiling intermediate step builds the compositor and GPU-sync client. x86_64
`comp_main`, `tests_render_compute` and `tests_render_pipeline_cache` build.
The previously observed full x86 service build limitation is the unrelated Rift
builder `constellation_tracker` configuration error. Linux CI is not claimed.
Retained Python tools compile, their CLIs load, and the cleaned contention runner
passes an idle/paced/saturated smoke. No new hardware-performance claim is made
by cleanup; the source changes preserve prior tested rendering behavior.

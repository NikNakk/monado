<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Three versus four service Metal images, 2026-10-12

The opt-in three-image path shows no additional starvation or material pacing
penalty in this bounded native offscreen workload. SteamVR and physical headset
acceptability remain untested. Keep the four-image default and the compatibility
mode opt-in until those trials are recorded.

The subsequent [production SteamVR-backend GPU test](macos-steamvr-resolve-copy-test.md)
exports all three resolve images across Windows processes and reduces the
backend's per-eye copies from 36 to zero over 18 stereo frames. Actual SteamVR
deployment and headset acceptance still require the
[hardware tests](macos-gpu-sync-hardware-tests.md).

This concerns **client swapchain images**, not display drawables. The
CAMetalLayer drawable pool already uses three. With
`XRT_MACOS_METAL_THREE_IMAGE_SWAPCHAINS=1`, the service enables latest-completed
scheduling as well as allocating three images. Producer readiness, source-image
claims and observed consumer GPU completion still guard reuse. The comparison
therefore tests the whole compatibility mode, not an isolated allocation count.

## Workload and evidence

Runtime source: `cf117ba24`, with the new test fixture and runner working-tree
changes. Runtime synchronization and presentation code were unchanged. Artifact
hashes and the working-tree diff hash are retained in the capture manifest.
Raw recordings are outside Git at `/tmp/macos-three-image-comparison-20261012`.

The native Metal client allocates a 1024×1024 two-slice BGRA array through the
production shared-Metal service path. Simple 2D BGRA IOSurface swapchains already
have three images by default and would make an unsuitable count comparison.
The fixture asserts the enumerated count is exactly three or four, clears both
slices on the real producer queue and alternates the slice submitted as a quad.
Separate 240-frame correctness runs verify the service scheduler marker and
composed Vulkan readbacks of the expected panel in both eyes. These constant
colour samples establish import/composition, not per-frame stale-image detection;
the M1–M5 patterned reuse tests remain separate evidence.

Four timing runs use four–three–three–four order, 1200 frames each, approximately
20 seconds per run. Diagnostic pixel readbacks and per-image logging are disabled
for these measurements. CSV timing remains enabled identically in both modes.
The simulated HMD/debug-image compositor runs at 60 Hz with its normal render
scale. This is a small producer workload, not GPU saturation or SteamVR Home.
All recorded Foundation thermal samples were nominal, low-power mode was off,
and Game Mode was not requested. CPU/GPU frequencies were not measured.
The first 30 client frames are excluded from timing distributions.

| Run | Images | Submitted Hz | Fresh latch Hz | Acquire/wait p95 / p99 (µs) | End-frame entry→latch p50 / p99 (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| A1 | 4 | 60.008 | 54.401 | 249 / 285 | 14.411 / 17.285 |
| B1 | 3 | 59.988 | 54.966 | 244 / 278 | 14.395 / 17.267 |
| B2 | 3 | 59.999 | 53.116 | 251 / 291 | 14.301 / 17.231 |
| A2 | 4 | 59.955 | 52.192 | 113 / 177 | 14.388 / 17.145 |

Fresh-latch gaps reached approximately 33.36 ms in all four runs; both modes
reprojected some frames. Three images did not introduce a prolonged starvation
episode. Acquire/wait maxima were 355, 357, 834 and 429 µs in run order.
The last four-image run had a 9.386 ms previous-fence wait (9.540 ms draw
dispatch); the three-image maxima were 5 and 14 µs. Its other CPU medians also
fell, so nominal thermal state does not establish identical scheduling/frequency.
Do not infer either a three-image improvement or a universal performance cost
from this small sample. GPU pixel runs and all four timing clients passed;
the existing IPC session/scheduler lifecycle test also passed.

## Reproduction and headset gate

From the Monado checkout, after an incremental native build:

```sh
cmake --build build/arm64 --target tests_macos_service_gpu_sync --parallel 4
python3 scripts/macos/run-three-image-comparison.py \
  --output /tmp/monado-three-image-offscreen-new
```

The runner creates fresh private launchd/socket fixtures, checks actual image
counts and correctness, records binary hashes, and writes a compact
`comparison.json` with acquisition, readiness, renderer and fresh-latch timing.
Use a new output directory for each comparison.

Hardware runs are user-operated. Close other headset owners, stop the installed
Monado service if necessary, then run the same comparison wearing the headset:

```sh
python3 scripts/macos/run-three-image-comparison.py \
  --headset --frames 2400 --output /tmp/monado-three-image-headset-new
```

Record the commit and whether the panel remains stable, including head motion,
black frames, checkerboards and stalls. The wrapper rejects an unavailable
headset instead of accepting an offscreen fallback. This native test still
does not exercise the Windows/DXMT producer, SteamVR resolve handles, workload
or Game Mode. Follow the separate
[SteamVR trial](macos-psvr2-timing-diagnostics.md#three-image-steamvr-compatibility-prepared-2026-10-10)
after the native headset check. Compare fixed-allocation copying and actual
three-image zero-copy with their actual transport markers; changing both
transport and scheduler measures a deployment choice rather than the isolated
image-count cost.

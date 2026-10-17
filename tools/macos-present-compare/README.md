<!-- SPDX-License-Identifier: BSL-1.0 -->

# macOS presentation comparison

Standalone experiment comparing Monado's Vulkan-to-Metal presentation architecture
with Vulkan WSI through `VK_EXT_metal_surface`. It does not link Monado or OpenXR,
load headset drivers, change display modes, or modify the integration branch.

| Mode | Path |
| --- | --- |
| `native` | Vulkan renders into an IOSurface-backed image; exported timeline/shared event orders a Metal blit into a `CAMetalDrawable`; a separate presenter takes the newest pending frame after drawable acquisition. |
| `wsi-copy` | Vulkan renders into the same kind of IOSurface-backed image, copies into a Vulkan swapchain image, then presents using FIFO. |
| `wsi-direct` | Vulkan renders directly into a Vulkan swapchain image, then presents using FIFO. |

All modes use the same fragment shader, visible frame ID barcode, extent, BGRA8
UNORM format, sRGB layer color space, display-link pacing and three CPU frame slots.
Native requests three drawables. WSI requests at least three swapchain images,
subject to surface limits, and records the actual count. The copied paths retain
three offscreen images. All rendering uses Vulkan, including the native mode.

This is a model of the presentation architecture, not a bit-for-bit extraction of
`comp_window_macos.m`. In particular, it has no separate drawable prefetcher,
Monado pacing/pose prediction, Mach time-constraint policy, distortion, client IPC,
Game Mode activation, remote layer hosting, or passthrough. A promising result
needs a follow-up using Monado's real distortion workload.

## Build and smoke test

Requires macOS 14+, Xcode command-line tools, Vulkan headers, Vulkan loader,
MoltenVK and `glslc`. Homebrew installations under `/opt/homebrew` are the default;
set `VULKAN_PREFIX` for another prefix, and optionally `GLSLC` and `BUILD_DIR`.
A nondefault build directory must also be passed with `--shader-dir` at runtime.

```sh
cd tools/macos-present-compare
sh build.sh
.build/present-compare --list-displays
.build/present-compare --mode native --policy immediate \
  --display-name 'Built-in Retina Display' --windowed --frames 120 \
  --trace /tmp/native-smoke.csv
.build/present-compare --mode wsi-copy --policy immediate \
  --display-name 'Built-in Retina Display' --windowed --frames 120 \
  --trace /tmp/wsi-copy-smoke.csv
.build/present-compare --mode wsi-direct --policy immediate \
  --display-name 'Built-in Retina Display' --windowed --frames 120 \
  --trace /tmp/wsi-direct-smoke.csv
```

Display names are exact, with no fallback to an unrelated display. Normal runs
cover the selected display with a borderless window; `--windowed` uses a small
window instead. The shader alternates brightness and shows a 24-bit frame barcode
across the top (least significant bit on the left) plus a moving red marker.
These help external recording identify displayed frames; this tool does not
perform optical validation or measure motion-to-photon latency.

## Repeated headset comparison

Run captures serially, with other rendering workloads held constant. A matrix
uses cyclic ordering across repeats to reduce order effects, stores the exact
commands, hashes the executable and sources, and preserves per-run logs. Output
must be a new directory. Raw captures and build products are ignored by Git.

```sh
python3 run_matrix.py --output captures/baseline --frames 2700 --repeats 3
python3 analyze.py --warmup 120 captures/baseline/*.csv
```

At 90 Hz, 2700 submitted frames take approximately 30 seconds plus setup/drain;
at 120 Hz use 3600 frames for a similar duration. Backpressure or delayed frames
can extend wall time. Check each metadata file for the actual refresh rate,
extent, image count, GPU, driver version, timing extensions, and result status.
The tool does not select 120 Hz automatically.

For timing-policy comparison, keep absolute timed presentation distinct from
minimum-duration presentation:

```sh
python3 run_matrix.py --output captures/timed --policy timed --frames 2700
python3 run_matrix.py --output captures/minimum-vs-timed \
  --policy timed --native-minimum --frames 2700
```

`immediate` means no requested presentation timestamp; **WSI still uses FIFO**,
not `VK_PRESENT_MODE_IMMEDIATE_KHR`. Native `timed` uses Metal `atTime:`; WSI
`timed` requires `VK_GOOGLE_display_timing`. Both request the CVDisplayLink output
time plus `--offset-ms`. Native `minimum` uses `afterMinimumDuration:` (default
8 ms); it has no exact Vulkan counterpart. Do not describe minimum-vs-timed as
an API-only comparison.

## Deadline and recovery experiments

Use the same burn count across modes, but measure the resulting GPU time rather
than treating the count as milliseconds. Sweep small to larger counts first;
stop before routinely exceeding the refresh budget. Burn work affects the visible
output and cannot be trivially removed as dead shader code.

```sh
python3 run_matrix.py --output captures/burn32 --burn 32 --frames 900 --repeats 2
python3 run_matrix.py --output captures/burn128 --burn 128 --frames 900 --repeats 2
python3 run_matrix.py --output captures/delay \
  --delay-ms 20 --delay-every 90 --frames 900 --repeats 2
```

The CPU delay occurs after a display tick is selected and before acquisition /
encoding. Tick coalescing and native pending-frame supersession are separately
observable. Native's mailbox can replace unpresented pending frames while its
presenter waits for a drawable; WSI uses its normal FIFO/acquisition backpressure.
The different buffering policies are intentional, and must be considered when
interpreting recovery results. This harness does not inject an OS drawable stall;
GPU/CPU delay tests exercise pressure but cannot prove behaviour under every
WindowServer stall.

## Measurements and limitations

CSV rows stay in memory until capture/drain completes; no per-frame file writes.
Vulkan query results are read after a reused slot's GPU fence, without a per-frame
CPU wait for the work just submitted. Metal source slots are reusable only after
the blit command buffer completes. WSI render-finished semaphores are per
swapchain image and reused on image reacquisition.

- `generated_s` is when the producer selects a display-link tick, not pose sampling.
- `target_s` is the selected CVDisplayLink output timestamp, not necessarily the
  next achievable latch deadline.
- `acquire_*` measures native `nextDrawable` or `vkAcquireNextImageKHR`. MoltenVK
  may obtain the actual drawable later during encoding/submission; compare CPU
  submit/encoding gaps too, not acquire time alone. Native acquisition can begin
  before the selected frame is generated because of mailbox replacement.
- `render_gpu_ms` measures the Vulkan render interval; `post_render_gpu_ms`
  includes the final barriers and, in `wsi-copy`, the Vulkan copy.
- `metal_command_gpu_ms` includes native presentation command-buffer work and
  possible dependency waiting. It is **not** an isolated blit duration, so adding
  it to Vulkan render time can double-count overlapping intervals.
- `gpu_inflight_at_record` is a fence-based snapshot of GPU submissions, including
  the frame about to be recorded. It is not WindowServer queue depth.
- Native display feedback is `MTLDrawable.presentedTime`; WSI feedback comes from
  `VK_GOOGLE_display_timing` when available. Missing feedback stays missing.
  `superseded` means the native mailbox explicitly replaced a frame; `submitted`
  at the end can mean missing/late feedback and is not a confirmed dropped frame.
- Long observed intervals / estimated unobserved refreshes describe gaps in the
  feedback stream. They can include missing callbacks as well as missed presents.
  Inspect coverage and frame IDs before assigning causality.
- Frame-age and target-error summaries for WSI require `--trust-wsi-clock`.
  First verify that the installed MoltenVK reports `presentedTime * 1e9` in its
  GOOGLE timing path and that the values are plausible against host timestamps.
  The analyzer applies the plausibility check even after opt-in. The tested
  MoltenVK 1.4.2 source does this in
  [MVKImage.mm](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/MoltenVK/MoltenVK/GPUObjects/MVKImage.mm).
  Zero presentation timestamps can be substituted internally by MoltenVK; this
  harness cannot distinguish that fallback from actual display feedback.
- Native host timestamps use `CACurrentMediaTime`; CV host ticks are divided by
  `CVGetHostClockFrequency`. GPU timestamps are used only for durations, never
  compared to host clocks. No hardware display-latency claim follows from them.
- Warm-up exclusion is by submitted frame ID, default first 120 frames. Missing
  feedback and superseded frames remain in the coverage denominator.
- The last one or two presentation callbacks may not arrive. The probe allows a
  one-second feedback grace period, then reports their actual recorded status.
  Small callback state is kept alive until process exit; Vulkan resources are
  explicitly released after draining. Killing a run loses its buffered CSV.

Choose a path using repeated cadence, frame-age tails, feedback coverage,
backpressure/recovery and GPU cost together. Average FPS is insufficient.

## Verification

```sh
python3 -B -m unittest -v test_analysis.py
spirv-val .build/vert.spv
spirv-val .build/frag.spv
MTL_DEBUG_LAYER=1 .build/present-compare --mode native --policy immediate \
  --windowed --display-name 'Built-in Retina Display' --frames 120 --trace /tmp/metal-check.csv
```

`--validation` requires the optional `VK_LAYER_KHRONOS_validation` installation
and fails if it is unavailable. Run validation separately from measurements.
Builds use `-Wall -Wextra -Werror`. For formatting this Objective-C++ source, use
the repository `.clang-format` with its `Language: Cpp` selector changed to
`Language: ObjC` in a temporary configuration; all other rules stay identical.

Recorded captures and conclusions are in [RESULTS.md](RESULTS.md).

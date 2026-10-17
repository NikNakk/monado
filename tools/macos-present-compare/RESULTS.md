<!-- SPDX-License-Identifier: BSL-1.0 -->

# Presentation experiment — 15 October 2026

Direct Vulkan WSI is a viable candidate for a Monado experiment. In the matched
absolute-timestamp runs, its p99 software frame age was approximately 38.8 ms,
essentially matching the native Metal bridge. It also eliminates the explicit
final image copy. The untimed baseline favoured the native latest-frame worker;
presentation policy and buffering materially affect the result. These captures
do not establish a universally faster API or justify replacing the integration
presenter yet.

## Configuration and evidence

- Apple M5, macOS 26.7.1 (25G241), MoltenVK 1.4.2, Apple clang 21.0.0.
- Connected PS VR2 display, 4000 × 2040 pixels, nominal 89.9102 Hz.
- The existing display mode was retained. **120 Hz was not tested.**
- Three drawables / three actual WSI swapchain images, three CPU frame slots.
- BGRA8 UNORM, sRGB layer, identical Vulkan fragment workload; both copied paths use IOSurface-backed offscreen images.
- 33 serial headset captures: nine baseline runs of 2700 frames; six runs of 900 frames for each other condition.
- First 120 submitted frames excluded from each summary. Orders rotate native → copied WSI → direct WSI between repeats.
- Each capture directory contains CSVs, metadata, logs, an exact-command/source/binary hash manifest and a JSON summary. Raw captures are retained locally and ignored by Git.
- Captures were made on an active desktop, without pinned GPU clocks or control over every background task. Ranges below are repeat-to-repeat observations, not confidence intervals.

WSI host-age comparisons were explicitly enabled after checking the installed
version's [MoltenVK 1.4.2 presentation implementation](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/MoltenVK/MoltenVK/GPUObjects/MVKImage.mm):
its display-feedback path reports Metal `presentedTime` in nanoseconds. All
summarized presentation timestamps passed the analyzer's host-clock plausibility
check. MoltenVK can internally substitute a timestamp if Metal reports zero;
we cannot independently identify those substitutions.

## Baseline: same workload, no requested timestamp

WSI remains FIFO; “immediate” here means absence of a requested timestamp.
Each cell gives the range across three repeats. Frame age is generation of a
synthetic frame to reported presentation, **not motion-to-photon latency**.

| Path | Observed Hz | p99 frame age (ms) | p99 submit call (ms) | Presented / superseded after warm-up |
| --- | ---: | ---: | ---: | ---: |
| native | 89.35–89.70 | 38.88–38.90 | 0.46–0.50 | 7716 / 24 |
| wsi-copy | 89.36–89.74 | 49.97–54.10 | 12.92–15.42 | 7740 / 0 |
| wsi-direct | 89.22–89.81 | 49.94–50.02 | 10.99–13.44 | 7740 / 0 |

Native kept a shorter frame-age tail in this baseline, but explicitly superseded
24 pending frames across the three runs. WSI retained frames and sometimes
blocked in submission for a refresh or longer. This is a comparison of the
implemented buffering policies as well as API paths; it does not prove that WSI
inherently requires the extra refresh. Acquire time alone misses stalls that
MoltenVK can defer until command encoding/submission.

In copied WSI, median post-render GPU time was 1.13–1.14 ms, versus roughly
0.006 ms in direct WSI. This interval includes barriers as well as the copy.
Native Metal command-buffer time includes dependency waiting, so it cannot be
used as an isolated copy cost or simply added to Vulkan render time.

## Policy and disturbance checks

Each cell is the range of per-run p99 frame ages (ms) across two repeats.

| Condition | Native | WSI copy | WSI direct |
| --- | ---: | ---: | ---: |
| 32 fragment iterations, immediate | 38.87–38.89 | 38.10–49.93 | 48.02–49.63 |
| 20 ms delay every 90 frames, immediate | 48.01–49.23 | 38.88–49.65 | 49.65–49.78 |
| Matched absolute timestamps | 38.75–38.87 | 49.55–49.84 | 38.82–38.86 |
| Native minimum 8 ms / WSI timed | 38.87–38.88 | 27.72–38.64 | 38.71–48.03 |

- **Matched timed presentation:** direct WSI matched native at about 38.8 ms p99 in both repeats. The WSI-copy path retained a longer tail in that group.
- **GPU pressure:** 32 shader iterations increased measured render durations; the age results overlapped more than in the baseline and did not establish a universal winner.
- **Periodic CPU delay:** a 20 ms delay every 90 submitted frames exercised tick coalescing and recovery. Native did not consistently have the best tail. This delays the common producer; it does not isolate a blocked `nextDrawable` acquisition while a producer continues undisturbed.
- **Minimum duration:** the native 8 ms minimum-duration policy is included separately. It has no exact WSI counterpart. Some WSI runs settled at lower age plateaus, reinforcing the role of queue state / phase; two short repeats do not establish reliable control over that plateau.
- No missing-feedback row in these post-warm-up captures was reclassified as a confirmed drop. The JSON summaries retain statuses, coalesced ticks and interval gaps.

## Decision and next useful experiment

Keep the working integration presenter while testing a temporary WSI target with
Monado's real distortion workload. Start with direct WSI and timed presentation,
and compare against production native presentation using the same headset mode
and pose-prediction timestamps. A useful control would limit CPU frames in flight
while retaining three swapchain images, to distinguish buffering from drawable
pool starvation.

This standalone test does not include Monado scheduling, its drawable prefetcher,
pose prediction, OpenXR clients, Game Mode, cross-process layer hosting or
passthrough. It measures software presentation feedback; no optical timing or
subjective headset-smoothness assessment was performed. The 90 Hz result must
be repeated at 120 Hz before drawing conclusions about the production target.

## Verification

- Warning-clean optimized Objective-C++ build with `-Wall -Wextra -Werror`.
- Both generated SPIR-V shaders pass `spirv-val`.
- All three paths pass short windowed runs with Metal API validation enabled.
- Four analysis regression tests pass, including missing feedback, rejected clocks and incomplete captures.
- Argument rejection checked for invalid modes, invalid numeric input and mismatched delay controls.
- All 33 headset runs exited successfully. Source/executable hashes, frame sequences, monotonic selected ticks and nonnegative GPU durations verified against every manifest/CSV.
- clang-format 23.1.3 passes with the repository configuration's language selector adapted to Objective-C; other formatting rules unchanged. Whitespace checks pass.
- **Vulkan validation remains incomplete:** `VK_LAYER_KHRONOS_validation` is not installed; requesting it fails cleanly with `VK_ERROR_LAYER_NOT_PRESENT`. Metal validation does not replace Vulkan synchronization validation.

## Capture directories

- [baseline-90hz](captures/baseline-90hz/manifest.json) — [summary](captures/baseline-90hz/summary.json).
- [gpu-burn32-90hz](captures/gpu-burn32-90hz/manifest.json) — [summary](captures/gpu-burn32-90hz/summary.json).
- [cpu-delay20-90hz](captures/cpu-delay20-90hz/manifest.json) — [summary](captures/cpu-delay20-90hz/summary.json).
- [timed-90hz](captures/timed-90hz/manifest.json) — [summary](captures/timed-90hz/summary.json).
- [minimum-vs-timed-90hz](captures/minimum-vs-timed-90hz/manifest.json) — [summary](captures/minimum-vs-timed-90hz/summary.json).

Regenerate a summary with:

```sh
python3 analyze.py --trust-wsi-clock --json captures/baseline-90hz/*.csv
```

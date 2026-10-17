> **Review follow-up:** the native-wrapper `XRT_MACOS_WSI` prototype and credit
> limit are now removed. Only `XRT_MACOS_WSI_GENERIC` remains executable.
> Commands and flags below describe historical captures; use
> [the current migration and validation note](macos-wsi-migration.md) for new runs.

> **Historical evidence:** captures and performance conclusions in this report use
> the older `08619006b` native base. They do not compare WSI with the latest
> `macos-upstream-clean`. See [the migration record](macos-wsi-migration.md)
> for the current source, preserved native behavior and validation status.

<!-- SPDX-License-Identifier: BSL-1.0 -->

# macOS Vulkan WSI experiment

**Branch:** `experiment/macos-wsi` based on `macos-upstream-clean` (`08619006b`).
**Date:** 15 October 2026.

**Follow-up complete:** 40 normal-policy load captures, 16 separate attribution
captures and eight Metal-validation captures. WSI matches baseline latency and
preserves more compositor cadence under the heaviest GPU load in these runs.
One CPU-stall tail regressed; total GPU-cost superiority remains unproven.
See the follow-up tables below and Claude review dispositions.

Direct Vulkan WSI now runs Monado's real compute distortion/timewarp compositor
with native Metal OpenXR clients on PS VR2. In this test it delivered broadly
comparable presentation timing to the native Metal presenter at both 90 and
120 Hz display modes. It did not demonstrate a decisive latency or total GPU-cost
advantage. Keep native as the default while investigating the remaining tradeoffs.

## Implementation

- `XRT_MACOS_WSI=1` selects the experimental `macos-wsi` factory; default native and hosted-client selection retain their existing paths.
- WSI reuses the existing macOS frontend, physical refresh switching, CVDisplayLink, fake compositor pacer and its presentation-offset feedback. It delegates image creation/acquisition and presentation to `comp_target_swapchain` through `VK_EXT_metal_surface`.
- The real Vulkan compute compositor writes directly into swapchain images. There is no final full-frame Vulkan-to-Metal blit in this target.
- MoltenVK GOOGLE presentation timestamps are converted between Metal host time and Monado monotonic time in both directions.
- `XRT_COMPOSITOR_PREFERRED_IMAGE_COUNT=3` retains three swapchain images. The capture verifies that actual count.
- `XRT_MACOS_WSI_MAX_PENDING=0|2|3` optionally bounds outstanding FIFO presentations using GOOGLE feedback. This is **not** two/three concurrent GPU submissions: the existing renderer waits for the preceding render fence. A newer FIFO feedback ID retires older credits without manufacturing their individual presentation timestamps.
- A common opt-in `compositor_gpu` trace records the same Vulkan query interval for both targets.
- WSI is scoped to the ordinary service/window comparison. Hosted layers, Game Mode and the Metal passthrough pass are outside this experiment.

## Necessary ownership fix

The first native and WSI pilots both crashed on application shutdown. Objective-C
zombie diagnostics identified `MTLTextureDescriptorInternal` being released after
deallocation. The native Metal service swapchain path obtained an autoreleased
descriptor but explicitly released it on success/error paths. The experiment
adds the missing retain, matching those releases. Both paths then shut down
cleanly, including all repeated captures. This fix is already present on the current integration line, as confirmed by
Claude’s review; this experiment ports it onto its older upstreaming base. It is
in `comp_metal_service_swapchain.m` and should be reviewed separately from WSI.

## Controlled comparison

- Apple M5, macOS 26.7.1, MoltenVK 1.4.2, optimized `RelWithDebInfo` build.
- PS VR2 USB driver and its native 6DoF path; captured pose queries report valid/tracked orientation and position throughout the analyzed windows. This is a flags check, not an independent tracking-accuracy measurement.
- Same `psvr2-openxr-test` application, 862 world-locked boxes/detail panels, native Metal client, two 2800×2856 eye images, 4000×2040 compositor target.
- Service composition, compute distortion and timewarp enabled; cameras, eye gaze, passthrough and Game Mode client compositing disabled.
- Actual display-link periods verified at approximately 89.91 and 119.88 Hz, with physical display modes independently checked after each run.
- Two rotated-order repeats per condition and display mode: **20 runs**, 15 seconds after session readiness each. First two seconds after valid projection content excluded from analysis.
- Native production-style policy uses an 8 ms minimum duration. `native-timed` uses the existing absolute-time Metal path. WSI uses FIFO with timed presentation through the stock swapchain helper. Their final timestamp-selection policies are not identical, although the pacer/tracking workload is shared.
- Performance captures use buffered traces and no Metal validation. Short validation captures are separate. Background workloads and GPU clocks were not pinned.

Rows below show repeat-to-repeat ranges. “Sample → present” is the compositor's
late-render sample timestamp to reported display presentation; it is **not**
application input latency or motion-to-photon latency. Prediction error joins
actual presentation to the same frame's predicted display time. Cadence is
observed feedback cadence, with coverage shown separately.

| Display mode | Target | Observed Hz | Feedback coverage (%) | p50 sample → present (ms) | p95 sample → present (ms) | p99 sample → present (ms) | p99 absolute prediction error (ms) |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 90 Hz | native | 88.99–89.06 | 98.97–99.05 | 25.09–25.09 | 25.17–25.17 | 36.16–36.17 | 11.12–11.12 |
| 90 Hz | native-timed | 88.37–89.60 | 98.28–99.66 | 25.09–25.09 | 25.17–25.18 | 25.19–36.23 | 2.39–11.12 |
| 90 Hz | wsi | 89.07–89.22 | 100.00–100.00 | 25.01–25.01 | 25.12–25.12 | 36.12–36.12 | 10.50–10.94 |
| 90 Hz | wsi2 | 71.35–75.13 | 100.00–100.00 | 13.93–24.82 | 25.08–25.08 | 25.11–25.13 | 11.11–11.12 |
| 90 Hz | wsi3 | 89.22–89.45 | 99.91–100.00 | 25.00–25.00 | 25.12–25.12 | 36.09–36.17 | 11.12–11.12 |
| 120 Hz | native | 115.10–116.05 | 96.02–96.81 | 19.55–19.55 | 27.81–27.89 | 27.94–27.96 | 8.33–8.34 |
| 120 Hz | native-timed | 115.51–116.81 | 96.22–97.57 | 19.52–19.55 | 19.63–27.84 | 27.91–27.93 | 8.32–8.34 |
| 120 Hz | wsi | 115.26–116.35 | 99.93–100.00 | 19.47–19.47 | 27.80–27.81 | 27.86–27.90 | 8.28–8.34 |
| 120 Hz | wsi2 | 99.70–101.23 | 100.00–100.00 | 11.17–11.18 | 19.53–19.53 | 19.56–19.57 | 8.05–8.24 |
| 120 Hz | wsi3 | 114.14–116.04 | 99.93–99.93 | 19.47–19.48 | 27.82–27.84 | 27.88–27.88 | 8.34–8.34 |

### What the comparison establishes

1. Direct WSI is viable in the actual runtime, including native Metal client swapchains, distortion/timewarp and the 120 Hz display mode. At 120 Hz, uncapped WSI and native both delivered roughly 115–116 observed presents/s in these diagnostic runs; neither was a perfect locked 119.88 presents/s.
2. Uncapped WSI and the three-presentation credit limit were close to native in p99 sample-to-presentation and prediction error. There is no clear latency win in this workload.
3. The two-presentation credit limit reduced the sample-to-presentation tail by approximately one refresh, but cadence fell to roughly 71–75/s at 90 Hz and 100–101/s at 120 Hz. It is not a suitable default based on this implementation/test.
4. Native timed presentation showed one lower-latency 90 Hz run that did not repeat consistently. Queue phase and scheduling remain material; these short repeats do not establish control over the better plateau.
5. The client-frame map separately records reused application frames. The compositor cadence figures are not application-rendering FPS.

## GPU timing interpretation

| Display mode | Native median Vulkan query span (ms) | WSI median Vulkan query span (ms) |
| --- | ---: | ---: |
| 90 Hz | 3.88–4.00 | 3.41–3.51 |
| 120 Hz | 2.75–2.81 | 4.35–4.37 |

These spans do **not** establish total GPU work or a WSI saving. The render query
starts at `TOP_OF_PIPE` and ends at `BOTTOM_OF_PIPE`; the swapchain acquire
semaphore is waited at the compute stage, so WSI's span can include acquisition
dependency stalls. Native's final Metal blit is outside its Vulkan query span.
The 120 Hz WSI span was higher, while the 90 Hz span was lower. An isolated
compute/copy measurement or Metal System Trace is needed before attributing
either difference to GPU work or bandwidth.

## Validation and state restoration

- All 20 timing runs: client exit 0, service exit 0, expected PS VR2 hardware and image geometry.
- Native and WSI at both refresh modes: four separate short runs with Metal API validation, clean exits.
- 35 existing CTest tests pass. Three initially required desktop/shared-memory permissions and passed when rerun with that access.
- Four new analysis tests pass: frame-ID joins/missing feedback, credit-limit rejection, refresh mismatch and failed-client/missing-projection rejection.
- Build is warning-clean after excluding the unrelated optional Euroc driver with existing format warnings. The build uses Homebrew Python because the manifest generator needs Python 3.10+.
- Changed C/C++/Objective-C lines and the new helper formatted with the exact CI pin, clang-format 23.1.1. Whitespace checks pass. No CMake source files changed.
- Vulkan synchronization validation remains unavailable locally because `VK_LAYER_KHRONOS_validation` is not installed; Metal API validation is not a substitute.
- The original launchd job was re-registered and the original 90 Hz display mode restored after both timing and validation matrices. Installed runtime files were not modified.

## Reproduce

From this worktree:

```sh
cmake -S . -B build/wsi -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_FIND_FRAMEWORK=LAST -DPython3_EXECUTABLE=/opt/homebrew/bin/python3 \
  -DXRT_BUILD_DRIVER_PSVR2=ON -DXRT_BUILD_DRIVER_PSSENSE=OFF \
  -DXRT_BUILD_DRIVER_QWERTY=OFF -DXRT_BUILD_DRIVER_EUROC=OFF \
  -DXRT_HAVE_OPENGL=OFF -DXRT_HAVE_OPENGLES=OFF \
  -DXRT_FEATURE_WINDOW_PEEK=OFF -DXRT_MODULE_MONADO_GUI=OFF \
  -DXRT_FEATURE_DEBUG_GUI=OFF -DXRT_FEATURE_MACOS_TIMING_DIAGNOSTICS=ON
cmake --build build/wsi --target monado-service openxr_monado psvr2-openxr-test --parallel
xcrun clang -O2 -Wall -Wextra -Werror scripts/macos/wsi-experiment/display_mode.m \
  -framework AppKit -framework CoreGraphics -o build/wsi/display-mode
python3 scripts/macos/wsi-experiment/run.py --build build/wsi \
  --display-helper build/wsi/display-mode --output build/wsi/captures \
  --seconds 15 --repeats 2
python3 scripts/macos/wsi-experiment/analyze.py build/wsi/captures/manifest.json --json
```

The runner requires normal desktop/launchd access and an idle existing Monado
service. It temporarily unregisters the standard idle service job, registers its
own job using the built executable and private Unix socket directory, runs the
application, and restores the job/display in `finally`. It refuses to interrupt
an already running normal service. Output directories must be new. Interrupting
or killing the runner before its cleanup completes can require manual restoration;
its manifest records the original mode and restore results.

The limit controls outstanding presentations, not swapchain image count. For a
short focused test, use `--conditions native wsi --hz 120 --repeats 1`.
Use `--metal-validation` for separate validation captures.

## Local evidence

- [Timing manifest](/Users/nickkennedy/Code/monado-upstreaming/build/review/macos-wsi-captures/comparison/manifest.json), [joined summaries](/Users/nickkennedy/Code/monado-upstreaming/build/review/macos-wsi-captures/comparison/summary.json), [exact measured core patch](/Users/nickkennedy/Code/monado-upstreaming/build/review/macos-wsi-captures/comparison/source.diff).
- [Metal validation manifest](/Users/nickkennedy/Code/monado-upstreaming/build/review/macos-wsi-captures/metal-validation/manifest.json).
- [Build log](/Users/nickkennedy/Code/monado-upstreaming/build/review/macos-wsi-build.log), [CTest log](/Users/nickkennedy/Code/monado-upstreaming/build/review/macos-wsi-ctest.log), [desktop rerun](/Users/nickkennedy/Code/monado-upstreaming/build/review/macos-wsi-ctest-desktop.log).

Raw traces remain outside the worktree in the build/review directory. The native
integration branch is preserved. The prototype remains uncommitted for review; no
merge request, installed runtime replacement or default-presenter change was made.

## Next decision

WSI is now a credible simpler presentation backend to discuss upstream. It is not
a proven performance upgrade. Resolve the acquire-wait/GPU timing attribution and
validate presentation visually under motion before choosing it for the first
upstream runtime milestone. Keep the native implementation available for direct
comparison and for later Game Mode/hosting work.

## Follow-up: correctness, load and GPU attribution

This follow-up incorporates Claude’s 15 October review, read from
`/Users/nickkennedy/Code/monado-2/build/review-macos-wsi-experiment.md`.

- Add p50 and p95 to the original results above. Median sample-to-present latency
  is approximately 25.0 ms at 90 Hz and 19.5 ms at 120 Hz for both ordinary targets.
- Record `vkAcquireNextImageKHR` duration and result/image index separately from
  the experimental presentation-credit wait. Native `nextDrawable` duration is
  taken from the existing worker trace; it blocks a different thread.
- Record effective CAMetalLayer settings after MoltenVK creates its swapchain.
- Run a lighter GPU scene (24 submitted instances), periodic 20 ms application
  scheduling stalls every 90 app frames, and four/sixteen fully cleared scene
  passes per eye. Repeated clears prevent depth rejection from removing the load.
  The light control still builds the full CPU instance list; it isolates GPU load,
  not all application scheduling cost. CPU stalls are injected sleeps, not a CPU
  saturation benchmark. These workloads exercise native Metal clients only.
- Keep normal-policy performance captures separate from attribution and validation.
  `XRT_MACOS_GPU_ATTRIBUTION=1` gates Vulkan’s initial TOP_OF_PIPE timestamp on
  acquisition and waits for native Vulkan rendering on the CPU before submitting
  the final Metal command. This changes scheduling and belongs only in diagnostics.
  Native’s frame-joined sum includes that final Metal command interval. GPU spans
  can still include scheduling/contention; they are not GPU occupancy or power.
- Reject invalid acquire images/results, reversed display frame order, negative
  GPU intervals, missing expected load, GPU/runtime/Metal errors and failed
  application exits. Join prediction, Vulkan queries and native completions by
  frame ID. These checks cannot prove pixel equality or synchronization correctness
  in every schedule. Metal validation does not replace Vulkan sync validation.

The current integration timing ledger records a lid-closed control removing
WindowServer presentation bursts when the built-in display stopped refreshing
(`macos-psvr2-timing-diagnostics.md`, “Run 3: magic window off, then lid closed”).
These runs do not control the lid or built-in display. Shared 120 Hz misses must
not be attributed to a particular presenter without that control.

A separate minimal WSI target using the generic swapchain pacer remains a future
comparison. This prototype deliberately retains the native frontend/pacer to
compare presentation with shared timing policy. It is not the proposed final
upstream patch. Alternative image-availability credit schemes, hosted Game Mode,
passthrough and pixel/visual comparisons remain unvalidated here.

The independent descriptor patch is saved locally as
`build/review/metal-descriptor-ownership-fix.patch`. Diagnostic renderer changes,
test-app load controls and the eventual small target should be separate review units.

### Normal-policy load results

Forty completed runs: two rotated target-order repeats × two display modes ×
five loads × two presenters. Each runs for 10 seconds after session readiness;
analysis discards two seconds after first valid projection content. This is a
short repeated comparison, not evidence for small differences. Workload settings,
GPU/driver state, application reuse and phase effects remain relevant.

Values below are the range across the two repeats. All units are milliseconds
except observed Hz and reused application-frame percentage.

| Hz | Load | Target | Observed Hz | Sample→present p50 | p95 | p99 | Reused app frames (%) |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 90 | baseline | native | 89.17–89.66 | 25.08–25.10 | 25.17–25.17 | 25.19–36.16 | 21.22–22.91 |
| 90 | baseline | wsi | 89.17–89.41 | 25.01–25.02 | 25.12–25.14 | 36.10–36.15 | 21.84–22.08 |
| 90 | light | native | 88.43–89.66 | 25.10–25.10 | 25.18–25.18 | 25.20–36.23 | 21.93–24.20 |
| 90 | light | wsi | 89.54–89.79 | 25.00–25.01 | 25.10–25.12 | 25.15–35.89 | 21.64–22.38 |
| 90 | cpu | native | 89.16–89.79 | 25.10–25.10 | 25.17–25.18 | 25.19–36.18 | 20.86–25.69 |
| 90 | cpu | wsi | 88.05–89.91 | 25.01–25.02 | 25.13–30.45 | 25.15–47.17 | 21.80–24.28 |
| 90 | gpu4 | native | 89.41–89.66 | 25.11–25.12 | 25.17–25.18 | 25.19–25.19 | 24.09–24.20 |
| 90 | gpu4 | wsi | 89.54–89.66 | 25.04–25.04 | 25.12–25.12 | 25.14–36.09 | 18.31–18.92 |
| 90 | gpu16 | native | 83.89–85.94 | 25.11–25.14 | 36.21–36.27 | 36.27–36.31 | 11.94–17.44 |
| 90 | gpu16 | wsi | 89.29–89.66 | 25.04–25.04 | 25.14–25.15 | 25.17–36.20 | 21.53–25.42 |
| 120 | baseline | native | 115.67–117.12 | 19.56–19.56 | 19.63–27.86 | 27.95–27.96 | 26.82–27.70 |
| 120 | baseline | wsi | 116.16–116.37 | 19.49–19.49 | 27.83–27.84 | 27.89–27.90 | 23.24–23.66 |
| 120 | light | native | 115.87–118.02 | 19.55–19.55 | 19.63–27.84 | 27.93–27.95 | 25.18–25.95 |
| 120 | light | wsi | 115.92–116.52 | 19.48–19.49 | 27.82–27.83 | 27.86–27.89 | 23.48–24.20 |
| 120 | cpu | native | 115.50–117.53 | 19.55–19.55 | 19.63–27.89 | 27.92–27.94 | 25.55–26.52 |
| 120 | cpu | wsi | 115.87–115.91 | 19.48–19.49 | 27.82–27.82 | 27.89–27.89 | 20.62–23.35 |
| 120 | gpu4 | native | 116.04–116.29 | 19.57–19.57 | 27.86–27.87 | 27.94–27.94 | 26.76–27.63 |
| 120 | gpu4 | wsi | 116.49–117.50 | 19.49–19.50 | 27.75–27.81 | 27.88–27.89 | 20.34–21.11 |
| 120 | gpu16 | native | 100.38–103.56 | 19.60–19.61 | 27.95–27.96 | 36.25–36.27 | 61.19–61.66 |
| 120 | gpu16 | wsi | 115.28–116.40 | 19.49–19.50 | 27.82–27.85 | 27.90–27.93 | 54.64–54.68 |

The GPU load was observed in the application logs: baseline batch mean GPU times
were approximately 1.2–1.7 ms; sixteen-pass means approximately 9.3–11.1 ms. These
are means of 240-command batches, not a distribution of individual frame durations.
The lighter GPU control did not eliminate application-frame reuse, so baseline
reuse cannot simply be attributed to scene GPU cost.

Under sixteen-pass load, WSI preserved more compositor presents in both repeats
at both refresh modes. At 120 Hz it also reused fewer application frames. At
90 Hz it reused *more* application frames despite better compositor cadence;
WSI therefore does not demonstrate uniformly better application throughput.
One 90 Hz CPU-stall WSI run had a 47.17 ms p99 sample age, worse than either
native CPU-stall repeat (25.19–36.18 ms). A universal tail-latency win is not supported.

All forty runs passed the frame-order, renderer-timeline identity, GPU completion,
clock, refresh, load-presence and error-log checks. Pose flags were valid/tracked
throughout the analyzed windows; this is not a tracking accuracy measurement.
WSI acquisition p99 was about 0.02–0.03 ms and its largest individual acquire was
0.151 ms across the matrix. Native worker `nextDrawable` p99 ranged roughly
11–22 ms at 90 Hz and 16.5–18 ms at 120 Hz. The latter blocks the presentation
worker, so the two durations are not equivalent compositor-thread stall metrics.
These runs do not test native's optional drawable slot.

WSI's effective layer after swapchain creation logged `framebufferOnly=0`,
`displaySync=1`, `transaction=0`, `drawables=3`, `timeout=1`, `format=80`
(BGRA8Unorm), and `kCGColorSpaceSRGB`, matching the native layer initialization.
That checks configuration, not pixel equality or the displayed colour response.

The first attempted matrix was interrupted before its final 90 Hz capture could
record exit metadata. All formal results above use the complete fresh `load-v2`
matrix. An earlier capture also overlapped the 1.27-second regression suite;
it is excluded. No regression builds/tests overlapped the formal matrix.

Local evidence: `build/review/macos-wsi-captures/load-v2/manifest.json`,
`source.diff`, `summary.json` and each run's raw traces/application/service logs.

### Separate attribution results

Sixteen additional captures used `--gpu-attribution`: two repeats × native/WSI ×
90/120 Hz × baseline/sixteen-pass load. Normal performance findings above use
neither this scheduling control nor Metal validation. Native completion traces
confirm no GPU shared-event wait was encoded in these attribution commands.

The ranges below compare per-run medians, in milliseconds. “Joined” is native
Vulkan span plus final Metal-command span **for the same frame ID**, or WSI's
Vulkan span. It is not the sum of separate medians, nor total GPU cost per
rendered frame: superseded native frames have Vulkan work without a matching
presentation command and are absent from this joined subset. The JSON retains
all sample counts and component distributions.

| Hz | Load | Target | Vulkan span p50 | Final Metal command p50 | Joined span p50 |
| --- | --- | --- | ---: | ---: | ---: |
| 90 | baseline | native | 3.668–3.773 | 1.107–1.233 | 5.025–5.070 |
| 90 | baseline | wsi | 3.458–3.575 | — | 3.458–3.575 |
| 90 | gpu16 | native | 9.231–9.648 | 0.959–1.335 | 9.974–10.413 |
| 90 | gpu16 | wsi | 5.503–6.197 | — | 5.503–6.197 |
| 120 | baseline | native | 2.851–2.892 | 0.993–1.026 | 3.853–3.867 |
| 120 | baseline | wsi | 4.160–4.371 | — | 4.160–4.371 |
| 120 | gpu16 | native | 1.941–2.022 | 0.536–0.546 | 4.361–4.505 |
| 120 | gpu16 | wsi | 2.979–3.196 | — | 2.979–3.196 |

Including the native Metal command does not establish a universal WSI GPU-cost
win. WSI's measured baseline joined span is lower at 90 Hz and higher at 120 Hz.
The 120 Hz WSI Vulkan span remains around 4.2–4.4 ms after moving acquisition's
wait stage, close to the earlier unmodified measurements. The experiment therefore
does not establish acquire waiting as the principal source of that difference.
Under heavy load the attribution control changes scheduling substantially; it
must not be used to replace the normal-mode cadence/latency comparison.

There is an additional instrumentation limit: MoltenVK 1.4.2 does not forward
the requested timestamp stage directly through `MVKCmdWriteTimestamp::encode`.
Its encoder handles timestamp counter sampling according to Metal capabilities,
including deferred sampling passes on tile GPUs. Consequently, forcing the
acquire-wait stage is a useful scheduling control, not proof of a precisely
isolated compute-kernel interval. See the primary source:
[MVKCmdQueries.mm](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/MoltenVK/MoltenVK/Commands/MVKCmdQueries.mm#L68-L88)
and [MVKCommandBuffer.mm](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm#L1128-L1183).
Metal System Trace or explicit Metal counter instrumentation would be needed to
attribute occupancy/bandwidth and explain the remaining refresh-dependent gap.

Local evidence: `build/review/macos-wsi-captures/attribution-v2/manifest.json`,
`source.diff`, `summary.json` and each run's traces. All sixteen captures passed
the analysis checks; there is still no Vulkan synchronization-validation layer.

### Final verification and reproduction

- All 64 follow-up captures passed automated analysis; all eight validation runs
  logged Metal API Validation as enabled. No GPU/runtime/validation errors were
  detected. This is bounded evidence, not a full graphics/synchronization proof.
- Final warning-clean full build and **35/35 CTest tests** pass. The **10 analysis
  tests** include failing-command, invalid-acquire, trace-ID mismatch, reversed
  presentation and missing-load negative controls.
- clang-format **23.1.1** ran on changed source ranges; final dry-run/Werror checks
  passed, including the new helper. Whitespace checks pass. No CMake source changed.
- Binary hashes in every final matrix match the measured executables/runtime;
  tracked source patches match the final code. Runner and final analysis scripts
  are preserved with hashes in `build/review/macos-wsi-captures/tooling/` and
  `follow-up-evidence.json`. Analysis evolved after capture; the saved analyzer is
  the exact final version used to validate the three matrices.
- Final independent state check: original launchd job registered and idle,
  experiment job absent, original 4000×2040 **90 Hz** mode restored. Installed
  runtime files and integration branch were not changed. Work remains uncommitted.

Use the same build/display helper as the earlier reproduction instructions:

```sh
python3 scripts/macos/wsi-experiment/run.py --build build/wsi \
  --display-helper build/wsi/display-mode --output build/wsi/load-captures \
  --seconds 10 --repeats 2 --hz 90 120 --conditions native wsi \
  --loads baseline light cpu gpu4 gpu16
python3 scripts/macos/wsi-experiment/run.py --build build/wsi \
  --display-helper build/wsi/display-mode --output build/wsi/attribution-captures \
  --seconds 10 --repeats 2 --conditions native wsi --loads baseline gpu16 \
  --gpu-attribution
python3 scripts/macos/wsi-experiment/run.py --build build/wsi \
  --display-helper build/wsi/display-mode --output build/wsi/validation-captures \
  --seconds 4 --repeats 1 --conditions native wsi --loads cpu gpu16 --metal-validation
python3 scripts/macos/wsi-experiment/analyze.py build/wsi/load-captures/manifest.json --json
```

These are sequential matrices; do not run regression tests or another GPU workload
alongside the performance captures. Background OS activity and GPU clocks remain
uncontrolled. For a lid-closed comparison, explicitly arrange/record display state
first; the runner itself does not manage the lid or disable the built-in display.

**Next:** user visual and lid-closed controls, then a separate minimal WSI target
using generic swapchain pacing, measured with the same load harness. Do not infer
hosted Game Mode, passthrough, pixel equivalence, Vulkan-client contention or a
working two-presentation policy from these service/Metal-client results.

## Subsequent minimal-target comparison

The separate generic-pacing target and its default/headroom/fallback comparisons
are recorded in [macos-wsi-generic-experiment.md](macos-wsi-generic-experiment.md).
That follow-up implements the minimal-target investigation proposed above; the
original native integration default is retained.

## CVDisplayLink follow-up

The optional small-target CVDisplayLink source and separate offset-calibration
comparison are documented in [the CVDisplayLink experiment](macos-wsi-cv-experiment.md).
It includes 36 repeated performance captures and eight Metal-validation captures;
prediction accuracy improves, while throughput and latency tails remain mixed.

## Interleaved native comparison

The [matched native comparison](macos-wsi-native-comparison.md) adds 36 interleaved
performance captures, four CPU-load validation captures and client-frame readiness
joins. The 120 Hz GPU-load presentation gain reproduces, while the 90 Hz case
shows fewer distinct client frames despite higher presentation cadence.

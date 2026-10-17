> **Review follow-up:** the native-wrapper `XRT_MACOS_WSI` prototype and credit
> limit are now removed. Only `XRT_MACOS_WSI_GENERIC` remains executable.
> Commands and flags below describe historical captures; use
> [the current migration and validation note](macos-wsi-migration.md) for new runs.

> **Historical evidence:** captures and performance conclusions in this report use
> the older `08619006b` native base. They do not compare WSI with the latest
> `macos-upstream-clean`. See [the migration record](macos-wsi-migration.md)
> for the current source, preserved native behavior and validation status.

<!-- SPDX-License-Identifier: BSL-1.0 -->
# Separate macOS WSI target with generic display-timing pacing

Date: 15 October 2026. Branch: `experiment/macos-wsi`, base `08619006b`.
**Result:** the separate target works, but the stock GOOGLE feedback pacer did
not match the native-paced paths. The generic helper’s existing fallback pacer
recovered near-display cadence in single-repeat controls without CVDisplayLink.
Keep native as the integration default; the fallback mode is a candidate for the
small initial upstream target, subject to visual checks and more repeated tests.

This is a local, uncommitted experiment; no runtime installation or upstream MR
is part of this work.

## Implementation and scope

`comp_window_macos_wsi.m` is separate from the native Metal presenter and the
first WSI prototype. It reuses `comp_macos_frontend` for the AppKit window/layer,
creates a `VK_EXT_metal_surface`, and initializes `comp_target_swapchain` with
`COMP_TARGET_USE_DISPLAY_IF_AVAILABLE`. The standard helper creates the display-
timing pacer, predicts frames, marks CPU/GPU timing, consumes GOOGLE presentation
feedback and presents the FIFO swapchain. By default there is no CVDisplayLink pacer,
native offset filter, feedback-draining wrapper, presentation credit scheme,
Metal copy or native presenter worker in this target.

The prototype's wrappers around create/acquire/present/GPU-info only log passive
measurements. The shared helper has an optional feedback observer called **after**
`u_pc_info`; the observer records normalized timestamps without driving the pacer.
These diagnostics should be separated from a future upstream target submission.

### Clock bridge

MoltenVK presentation time and runtime monotonic time need an absolute-time
conversion. `comp_target_swapchain` now has an explicit clock offset, defaulting
to zero for all existing targets. It translates the requested absolute deadline
into API time and translates desired/actual/earliest feedback back into runtime
time. Durations (`presentMargin` and present slop) are not rebased.

The new target samples a fixed bridge at initialization. It must remain fixed
for the surface lifetime: recomputing it with nanosecond sampling jitter would
break the standard pacer's exact requested-deadline assertion. Analysis checks
that every feedback desired time plus the submitted slop exactly equals the
original deadline. Unit tests cover zero/immediate timestamps and round trips
with large positive/negative offsets, including present slop. Sleep/resume with a
clock-domain change requires recreation and is not validated in this experiment.

### Display modes and policy

The ordinary window frontend still targets PS VR2. This target reads the existing
physical display mode and sets the initial frame-period estimate from it. The
runner switches the PS VR2 mode using the small CoreGraphics helper before
starting a generic-target capture; the target does not import the native
presenter's refresh-switching code. The helper preserves geometry and restores
the exact original display mode afterward.

All compared paths use 4000×2040 output, three images and the same native Metal
OpenXR client with two 2800×2856 eye images. The runner pins the stock generic
configuration: display timing enabled, 30% maximum compositor-time budget, 1 ms
margin and 4 ms present-to-display prediction offset. Native and the first WSI
prototype retain their previously measured policies. A lower sample age with
fewer presents is a tradeoff, not a latency-only win. The prediction models and
CPU scheduling differ between these complete targets.

The generic period trace is `vkGetRefreshCycleDurationGOOGLE`, **not** a fabricated
CVDisplayLink trace. On MoltenVK this query derives the nominal period from the
CoreGraphics display mode, whereas the native/prototype traces use measured
CVDisplayLink periods. Independent CoreGraphics mode checks and actual feedback
cadence are retained.

## Capture design

- Three targets: `native`, `wsi` (shared-native-pacer prototype), `wsi-generic`.
- 90 and 120 Hz; baseline and sixteen fully cleared scene passes per eye.
- Three rotated-order repeats, 15 seconds after session readiness each: 36 runs.
- First two seconds after valid application projection layers excluded.
- Four separate generic Metal-validation runs, baseline/heavy at each refresh.
- Four separate fallback-mode Metal-validation runs.
- Four separate one-repeat 70% generic-headroom controls, 12 seconds each, to
  distinguish an existing configuration knob from a change of pacing algorithm.
- Four additional one-repeat standard-fallback controls, using the same target
  with `XRT_COMPOSITOR_USE_DISPLAY_TIMING=0`; no CVDisplayLink or target code change.
- All traces include exact code patches, new source files, binary hashes, options,
  display mode, exit and restore metadata. No builds or regression tests overlap
  the formal performance matrix. Background OS activity/GPU clocks remain uncontrolled.

The first pilot exposed a new-target mistake: AppKit window showing was called
from the compositor thread. It was moved into initialization, matching the native
frontend contract. A later pilot had a mismatched diagnostic CSV header, and the
first comparison was deliberately stopped before fixing the clock invariant.
Those captures are excluded; their logs remain local. The final pilot checks
exact deadline round trips before the formal matrix starts.

## Remaining controls

Visible correctness, pixel equality, lid-closed behavior, hosted Game Mode,
passthrough, Vulkan-client workloads and sleep/resume remain untested here.
A connected built-in display can affect WindowServer presentation cadence; the
existing integration ledger has a user-run lid-closed control that removed bursts.
Do not attribute shared 120 Hz misses solely to a presenter or pacer.

## User visual comparison

After building this worktree and the display helper, run the same runner with
`--hz 120 --conditions native wsi wsi-generic --loads baseline --repeats 1
--seconds 60`. Each condition prints its name. Compare stereo alignment, colours,
black/stale frames, distortion and world stability during slow and fast head
rotation. Then repeat with a deliberately arranged/recorded lid-closed setup.
The runner restores the service/display; it does not manage the lid or certify
what was visible inside the headset. The installed runtime remains separate.

## Default comparison results

Ranges across three runs per cell. Latency here is compositor sample→reported
presentation, not motion-to-photon latency. Faster sample age at a lower cadence
must be considered alongside missing refreshes and application-frame reuse.

| Hz | Load | Target | Observed presents/s | Sample age p50 (ms) | p95 (ms) | p99 (ms) | Reused app frames (%) |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 90 | baseline | native | 88.06–89.37 | 25.09–25.10 | 25.17–25.19 | 36.06–36.23 | 21.35–22.94 |
| 90 | baseline | wsi | 88.38–89.53 | 25.01–25.02 | 25.12–25.14 | 36.04–36.22 | 21.13–21.88 |
| 90 | baseline | wsi-generic | 64.25–64.59 | 15.21–15.21 | 26.35–26.37 | 26.41–26.44 | 11.86–15.10 |
| 90 | gpu16 | native | 82.01–83.38 | 25.13–25.15 | 36.27–36.29 | 36.30–36.31 | 14.39–16.84 |
| 90 | gpu16 | wsi | 88.30–88.76 | 25.04–25.05 | 25.15–25.16 | 36.23–47.22 | 21.16–37.23 |
| 90 | gpu16 | wsi-generic | 63.52–64.04 | 15.27–15.27 | 26.38–26.45 | 26.45–26.49 | 20.51–24.94 |
| 120 | baseline | native | 115.05–115.95 | 19.55–19.55 | 27.85–27.89 | 27.93–27.95 | 25.40–26.19 |
| 120 | baseline | wsi | 113.67–117.05 | 19.49–19.50 | 27.81–27.86 | 27.88–27.92 | 20.74–24.48 |
| 120 | baseline | wsi-generic | 88.23–89.22 | 11.59–11.61 | 11.75–19.91 | 20.03–20.04 | 14.89–17.99 |
| 120 | gpu16 | native | 100.28–102.04 | 19.60–19.61 | 27.95–27.96 | 36.25–36.27 | 60.34–61.48 |
| 120 | gpu16 | wsi | 114.79–116.33 | 19.49–19.50 | 27.81–27.85 | 27.90–27.92 | 55.74–55.91 |
| 120 | gpu16 | wsi-generic | 87.50–88.72 | 11.62–11.67 | 19.81–19.96 | 20.06–20.08 | 50.74–52.88 |

The stock generic display-timing path consistently loses cadence at both refresh
rates. Its lower sample age does not reproduce the original WSI prototype’s
combination of timing and cadence. The native-paced WSI prototype again preserves
more cadence under heavy GPU load, but one 90 Hz WSI heavy-load run has a roughly
47 ms tail. No universal latency-tail superiority is established.

## Configuration controls on the same minimal target

These are single-repeat, 12-second diagnostic controls, not three-repeat
performance claims. No Metal validation was enabled for these timing captures.

| Mode | Hz | Load | Observed presents/s | Sample age p50 (ms) | p99 (ms) |
| --- | ---: | --- | ---: | ---: | ---: |
| GOOGLE pacer, 70% budget | 90 | baseline | 67.21 | 19.64 | 30.61 |
| GOOGLE pacer, 70% budget | 90 | gpu16 | 67.03 | 19.73 | 30.85 |
| GOOGLE pacer, 70% budget | 120 | baseline | 89.63 | 14.96 | 23.19 |
| GOOGLE pacer, 70% budget | 120 | gpu16 | 89.74 | 14.95 | 15.12 |
| Generic fallback, no CVDisplayLink | 90 | baseline | 89.51 | 25.84 | 32.51 |
| Generic fallback, no CVDisplayLink | 90 | gpu16 | 89.01 | 25.91 | 42.01 |
| Generic fallback, no CVDisplayLink | 120 | baseline | 116.73 | 19.37 | 32.57 |
| Generic fallback, no CVDisplayLink | 120 | gpu16 | 114.01 | 20.21 | 32.59 |

Increasing the compositor budget to 70% does not restore display-rate cadence and
increases median sample age. The generic fallback restores roughly 89/s at 90 Hz
and 114–117/s at 120 Hz without CVDisplayLink. This narrows the observed issue to
the display-timing pacing path in this configuration, rather than requiring the
large native presenter just to maintain cadence. The controls do not identify
the root cause, establish equal tail latency, or validate Game Mode.

For the fallback control, the target’s experiment banner still describes its
default display-timing policy. The recorded option `XRT_COMPOSITOR_USE_DISPLAY_TIMING=0`
and the generic helper’s actual “non-feedback version” creation log identify the
selected fallback; the banner alone must not be used to infer the active pacer.
The target itself remains configured to use display timing when enabled/available.

### Feedback interpretation

MoltenVK 1.4.2 estimates `earliestPresentTime` using actual presentation time and
estimates `presentMargin` from actual minus begin-presentation time because Metal
does not expose those exact details. Its refresh-cycle query uses the nominal
CoreGraphics mode. See
[MVKSwapchain.mm](https://github.com/KhronosGroup/MoltenVK/blob/v1.4.2/MoltenVK/MoltenVK/GPUObjects/MVKSwapchain.mm#L169-L250).

The new traces retain earliest time and presentation margin. At 90 Hz the stock
pacer sees median margins around 14–15 ms, while its configured target margin is
1 ms. Monado’s generic pacer adjusts its compositor budget using that margin and
missed-presentation information. Those estimated inputs are a plausible area to
investigate; this experiment does not prove they caused the cadence loss. The
70% and fallback controls are scheduling experiments, not validation of driver
feedback semantics or total GPU cost.

## Validation and evidence

- All 36 default comparison captures passed the strict analysis checks, including
  exact generic deadline round trips, clock ordering, acquire results/index bounds,
  frame order, load presence and GPU/runtime error-log checks.
- Both four-run configuration controls passed the same analysis checks.
- Four stock-pacer and four fallback-pacer Metal API validation captures cover
  baseline/heavy GPU load at both refresh rates. Activation is checked separately
  from timing results. Vulkan synchronization validation remains unavailable.
- Final full build and **36/36 CTest tests** pass, including the new clock-domain
  regression test. **11 analysis tests** pass, including a one-nanosecond deadline
  drift negative control. No visual/pixel equality claim follows from these tests.
- clang-format 23.1.1 ran on changed ranges and all new C/Objective-C/C++ files;
  final dry-run/Werror checks pass. Changed CMake files were formatted and checked
  using cmake-format 0.6.13 with `.cmake-format.py`; whitespace checks pass.
- The new Objective-C file also passes a standalone Wall/Wextra/Werror syntax
  check with the existing headers’ unused-parameter exemption.
- Exact measured tracked patches, new source snapshots, binary hashes, runner
  environment, physical display mode and restore results are in each manifest.
  Final analysis tools and an evidence index are preserved separately.
- Work is uncommitted in the isolated managed worktree. Installed runtime files
  and the integration branch are preserved. The original idle service registration
  and exact original 90 Hz mode are restored after each matrix.

Local evidence directories beneath
`/Users/nickkennedy/Code/monado-upstreaming/build/review/macos-wsi-captures/`:
`generic-comparison-v2`, `generic-validation`, `generic-headroom`,
`generic-fake-control`, `generic-fake-validation`. Each has `manifest.json`,
`source.diff`, `source-untracked/`, `summary.json` and raw per-run logs/traces.
The evidence index is `generic-evidence.json` with final tools in `generic-tooling/`.

## Reproduce

Use the build described in [the original experiment](macos-wsi-experiment.md),
rebuilding this worktree and its updated display helper first:

```sh
cmake --build build/wsi --parallel
xcrun clang -O2 -Wall -Wextra -Werror scripts/macos/wsi-experiment/display_mode.m \
  -framework AppKit -framework CoreGraphics -o build/wsi/display-mode
python3 scripts/macos/wsi-experiment/run.py --build build/wsi \
  --display-helper build/wsi/display-mode --output build/wsi/generic-comparison \
  --seconds 15 --repeats 3 --hz 90 120 --conditions native wsi wsi-generic \
  --loads baseline gpu16
python3 scripts/macos/wsi-experiment/run.py --build build/wsi \
  --display-helper build/wsi/display-mode --output build/wsi/generic-fallback \
  --seconds 12 --repeats 1 --hz 90 120 --conditions wsi-generic \
  --loads baseline gpu16 --generic-fake-pacing
python3 scripts/macos/wsi-experiment/analyze.py build/wsi/generic-comparison/manifest.json --json
```

`--generic-max-comp-percent 70` selects the headroom control;
`--metal-validation` selects validation-only captures. For the visual test above,
repeat with `--generic-fake-pacing` to compare the recovered-cadence mode. Start a
fresh capture after arranging lid-closed state, rather than using sleep/resume
inside a run with a fixed clock bridge.

The next implementation choice is whether to submit an initial small WSI target
with the generic fallback pacer while investigating feedback quality separately.
First obtain the user’s visual/lid-closed observations and repeat the fallback
controls. Do not promote the stock feedback pacer based on this experiment.

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

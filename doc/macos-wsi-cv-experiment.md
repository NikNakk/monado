> **Review follow-up:** the native-wrapper `XRT_MACOS_WSI` prototype and credit
> limit are now removed. Only `XRT_MACOS_WSI_GENERIC` remains executable.
> Commands and flags below describe historical captures; use
> [the current migration and validation note](macos-wsi-migration.md) for new runs.

> **Historical evidence:** captures and performance conclusions in this report use
> the older `08619006b` native base. They do not compare WSI with the latest
> `macos-upstream-clean`. See [the migration record](macos-wsi-migration.md)
> for the current source, preserved native behavior and validation status.

<!-- SPDX-License-Identifier: BSL-1.0 -->
# CVDisplayLink timing for the separate macOS WSI target

Date: 15 October 2026. Branch `experiment/macos-wsi`, base `08619006b`.
Local experiment; native presentation remains the integration default.

## What is reused

The small `comp_window_macos_wsi.m` target still uses `VK_EXT_metal_surface`
and the generic FIFO swapchain helper for rendering/presentation. Two optional
settings reuse the native target's timing approach:

- `XRT_MACOS_WSI_GENERIC_CV=1`: create CVDisplayLink for the frontend's selected
  display, convert host timestamps into runtime monotonic time and project the
  output timestamp back to the most recent refresh boundary. Publish only that
  boundary atomically; consume it on the compositor thread via
  `u_pc_update_vblank_from_display_control`. This selects the existing
  `u_pc_fake` implementation explicitly, because the adaptive GOOGLE pacer
  ignores that update callback. The callback does no file I/O or Vulkan work.
- `XRT_MACOS_WSI_GENERIC_CALIBRATE=1` (requires CV): separately enable the native
  approach's 1/8 EMA of actual presentation minus the original desired deadline.
  Match feedback by frame ID in a bounded history; reject nonpositive offsets
  and offsets beyond four nominal periods. After eight accepted samples, feed
  the filter into `u_pc_update_present_offset`. Preserve the exact original
  deadline rather than the GOOGLE deadline adjusted by present slop.

Both options default off. No changes to either shared pacing implementation,
no native presenter worker, final Metal blit, presentation credits or deliberate
queue draining are introduced. CVDisplayLink is stopped/released before target
state is freed. The period is the CV nominal-period query, with phase updates
from timestamped callbacks; this does not dynamically retune the pacer's period.
Display-mode changes and sleep/resume require target recreation in this experiment.

The generic feedback observer remains passive in the first two controls. With
calibration enabled it updates only prediction offset, not compositor wake-up
budget. Calibration is active even without tracing. The fixed Vulkan/runtime
clock bridge and exact deadline round-trip checks remain in place.

## Controlled comparison

Runner conditions:

| Condition | Pacer | Phase source | Offset calibration |
| --- | --- | --- | --- |
| `wsi-generic --generic-fake-pacing` | Standard fallback | Initial monotonic anchor | Off |
| `wsi-generic-cv` | Standard fallback | CVDisplayLink | Off |
| `wsi-generic-calibrated` | Standard fallback | CVDisplayLink | On |

Use `--generic-fake-pacing` for the entire matrix so the first row cannot
silently select the adaptive feedback pacer. The CV rows force fallback pacing
independently of that flag. All rows retain the same generic FIFO presentation,
three images, full-resolution PS VR2 output and Metal OpenXR client. The runner
switches/independently checks 90/120 Hz and restores the original exact mode and
launchd registration in `finally`.

A short 90 Hz pilot precedes three rotated-order repeats at 90/120 Hz with
baseline and sixteen scene passes per eye. Each formal capture lasts 12 seconds
from session readiness, excluding two seconds after the first valid projection
layer. Eight separate Metal-validation captures cover both CV modes, both rates
and both loads. No builds or regression tests run during the formal matrix.

The analyzer checks positive, advancing CV phase samples in the past, the selected
fallback pacer, matching refresh, calibration activation and the complete filter
recurrence/bounds, exact Vulkan deadline round trips, presentation ordering,
GPU completion/query validity and the expected injected workload. The CV trace
is separate from MoltenVK's nominal `refresh` query trace.

## Interpretation limits

Prediction error here compares the compositor's prior predicted timestamp with
later MoltenVK/Metal presentation feedback, matched by frame ID. It is not a
measurement of photon emission, motion-to-photon latency or visible correctness.
The prediction recorded before feedback is used for each error calculation;
calibration cannot rewrite that frame's recorded prediction. Offset filtering
can improve prediction without shortening the rendering-to-presentation queue.

Visible presentation, pixel equality, lid-closed behavior, hosted Game Mode,
passthrough, Vulkan-client stress, variable refresh and sleep/resume remain
outside this run. Background OS activity is uncontrolled. A display-link callback
is a platform timing signal, not proof of exclusive/direct display ownership.

## Results

All 36 formal performance captures and eight separate Metal-validation captures
passed strict analysis. All eight validation service logs confirm Metal API
validation was enabled. The three-run ranges below keep run-to-run variability
visible; they are not confidence intervals. `gpu16` is sixteen scene passes per
eye. Sample age is compositor rendering sample to reported actual presentation.

| Hz / load | Mode | Presents/s | Sample age p50 / p99 (ms) | Absolute prediction error p50 / p95 / p99 (ms) |
| --- | --- | --- | --- | --- |
| 90 / baseline | Fallback | 88.606–89.215 | 25.534–27.056 / 36.603–37.954 | 18.831–20.243 / 24.552–24.985 / 29.812–31.167 |
| 90 / baseline | + CV + calibration | 88.914–89.612 | 25.061–25.071 / 25.229–36.172 | 0.001–0.001 / 0.738–3.690 / 11.122–11.123 |
| 90 / baseline | + CV | 88.319–89.711 | 25.058–25.063 / 25.206–36.295 | 18.291–18.293 / 18.323–18.325 / 18.325–29.446 |
| 90 / gpu16 | Fallback | 88.513–89.710 | 25.757–27.494 / 32.699–38.279 | 18.982–20.683 / 24.799–25.144 / 25.194–31.482 |
| 90 / gpu16 | + CV + calibration | 87.524–89.512 | 25.086–25.093 / 36.148–47.309 | 0.001–0.001 / 1.585–7.591 / 7.322–16.452 |
| 90 / gpu16 | + CV | 88.122–89.312 | 25.088–25.095 / 36.178–47.349 | 18.292–18.294 / 18.323–18.326 / 29.406–40.564 |
| 120 / baseline | Fallback | 114.210–115.702 | 19.442–20.514 / 30.517–32.370 | 12.661–13.729 / 18.673–24.209 / 23.721–25.107 |
| 120 / baseline | + CV + calibration | 113.560–116.275 | 19.537–19.541 / 27.926–27.956 | 0.001–0.016 / 5.212–6.308 / 8.230–8.342 |
| 120 / baseline | + CV | 115.477–117.375 | 19.536–19.549 / 27.920–27.953 | 12.730–12.735 / 21.065–21.083 / 21.072–21.090 |
| 120 / gpu16 | Fallback | 114.162–116.570 | 18.430–19.705 / 29.075–31.053 | 11.614–12.912 / 21.050–23.368 / 22.218–24.235 |
| 120 / gpu16 | + CV + calibration | 113.110–116.366 | 19.543–19.551 / 27.965–27.998 | 0.001–0.084 / 5.299–7.203 / 8.314–8.342 |
| 120 / gpu16 | + CV | 116.004–119.079 | 19.540–19.544 / 27.879–27.972 | 12.731–12.737 / 12.762–21.067 / 21.076–21.105 |

The useful result is prediction accuracy, not a demonstrated speed increase:

- CV alone stabilizes the phase and median sample age across repeats, but retains
  the standard fallback's default present-to-display offset. Its median absolute
  prediction error remains about 18.29 ms at 90 Hz and 12.73 ms at 120 Hz.
- CV plus calibration removes most of this systematic offset. Across twelve
  calibrated runs, median absolute prediction error is 0.0007–0.0845 ms. Feedback
  joins cover at least 99.886% of rendered frames in the formal matrix.
- Missed-refresh tails remain: calibrated p99 absolute prediction errors are
  7.32–16.45 ms at 90 Hz and 8.23–8.34 ms at 120 Hz. An EMA needs subsequent
  frames to recover from a changed queue delay. Low median error does not imply
  every frame was predicted accurately.
- There is no consistent throughput win from calibration. At 120 Hz, CV alone
  reports 115.48–119.08 presents/s across loads, while calibrated CV reports
  113.11–116.37. The captures do not isolate whether this difference is policy,
  client prediction effects or background variation.
- At 90 Hz under GPU load, CV variants have p99 sample ages up to 47.35 ms versus
  38.28 ms in the fallback-only runs. At 120 Hz, CV variants instead have lower
  p99 sample ages (about 28 ms versus 29.07–32.37). Do not claim universal lower
  latency or better tail behavior.

Recommendation: retain the small WSI target with CV and calibration as explicit
options for the user's visible comparison. The result supports reusing our timing
work, but does not justify changing the integration default or choosing a final
upstream pacing policy yet. A future cleanup should extract the shared macOS
clock/display-link source rather than maintain duplicated native/WSI logic.

## Verification and evidence

- Full build passed without warnings in `macos-wsi-cv-build.log`.
- 36/36 CTest tests passed in `macos-wsi-cv-ctest-host.log`. The first sandboxed
  attempt could not create macOS shared-memory/socket/remote-layer resources;
  the complete host-access rerun passed before the performance matrix began.
- 14 analysis tests passed, including rejecting missing/wrong timing sources,
  nonadvancing/future CV timestamps, invalid calibration bounds/filter recurrence,
  insufficient warm-up samples and broken Vulkan deadline round trips.
- Actual clang-format 23.1.1 applied to the changed Objective-C file, followed by
  `--dry-run --Werror`. Existing changed CMake files also pass cmake-format
  0.6.13 with `.cmake-format.py`; no CMake edits were needed for this follow-up.
  `git diff --check` and new-file whitespace checks pass.
- Saved manifests verify exact source snapshots and binary hashes against the
  tested code. Both matrices restore the original display mode and service job.

Evidence directory in the main checkout: `build/review/macos-wsi-captures/`:
`generic-cv-pilot/` (three short captures, excluded from formal ranges),
`generic-cv-comparison/` (36), `generic-cv-validation/` (8), and
`generic-cv-evidence.json` (source/binary/restoration checks).

Reproduce the formal matrix from the main checkout with a fresh output directory:

```sh
python3 /Users/nickkennedy/.codex/worktrees/macos-wsi-experiment/monado-upstreaming/scripts/macos/wsi-experiment/run.py \
  --build build/review/macos-wsi-build \
  --display-helper build/review/wsi-tools/display-mode-generic \
  --output build/review/macos-wsi-captures/generic-cv-new \
  --hz 90 120 --conditions wsi-generic wsi-generic-cv wsi-generic-calibrated \
  --generic-fake-pacing --loads baseline gpu16 --repeats 3 --seconds 12
```

For the visible check, use the same conditions with `--loads baseline --repeats 1
--seconds 60`, followed by a deliberately arranged lid-closed comparison. This
experiment does not change the installed runtime or certify visible output.


## Interleaved native comparison

The [matched native comparison](macos-wsi-native-comparison.md) adds 36 interleaved
performance captures, four CPU-load validation captures and client-frame readiness
joins. The 120 Hz GPU-load presentation gain reproduces, while the 90 Hz case
shows fewer distinct client frames despite higher presentation cadence.

<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# WSI headset failures: fixes and validation — 15 October 2026

The refresh and passthrough failures from the
[initial checks](macos-wsi-headset-initial-checks.md) are corrected in the
Monado-2 `experiment/macos-wsi-current-native` worktree, base
`f076c87ed8715e00a66c58e6bec7ad43b5ad19b1`, plus the saved uncommitted patch.
Native remains the default. Visual acceptance, WSI calibration/rotation parity,
and automatic sleep/disconnect recovery remain open.

## Fixes

- Active native refresh switches now run at prediction boundaries, when the
  compositor cannot continually add presentation work during the drain.
  API requests still wait for a result; requests without an active session apply
  immediately. Native and WSI share the tested session-aware queue and retain
  frame IDs. This supersedes the initial synchronous API-thread implementation,
  whose real headset hang reproduced despite restoring the old pacer setter.
- Both targets wait for the requested active physical mode with matching pixel
  dimensions and refresh rate before rebuilding timing. Three matching polls
  reject transient CoreGraphics fallback values. WSI's rate getter also rejects
  inactive displays and wrong geometry, so 60 Hz from another display cannot
  be adopted as the headset rate. A failed settle returns an explicit failure.
- The non-CV GOOGLE pacer retains its current absolute compositor budget,
  presentation offset, margin and caller configuration. Period-relative limits
  and adaptation steps scale in their original ratios; the learned budget is
  only reduced if it exceeds the new maximum. Custom-config regression checks
  cover budget and offset preservation as well as frame-ID continuity.
- Unsupported WSI passthrough is rejected by the compositor creation hook, with
  propagation through the multi compositor and an OpenXR unsupported result.
  The OpenXR handle allocated before a failed creation is destroyed. A defensive presentation path
  warns once without returning a per-frame renderer error.
- Camera startup returns `VK_NOT_READY` before taking the Vulkan image into
  Metal ownership. Normal presentation continues until camera frames arrive.
  Actual encoding/device/verification failures retain explicit errors and the
  restored-layout/semaphore cleanup path.
- Pixel verification tracks camera-only and application-overlay frames
  independently. An overlay check requires at least one sampled opaque source
  pixel, so the early camera-only frame no longer completes that gate.
- The original native GPU-obligation error followed a 6.8 ms wait during
  shutdown, rather than the 500 ms live-client deadline. Stopped waiters retain
  cancellation/quarantine behavior without logging a GPU failure or publishing
  completion. Live-client missing-signal failures remain errors and cause loss.
- `macos-wsi-migration.md` now contains one current document; its obsolete
  appended migration copy is removed.

## Headset checks

All positive pilots use the isolated build, Metal API validation and the same
24-instance light application as the initial runs. No installed runtime is
replaced. Captures contain exact environments, binary hashes, source diffs and
new source files.

| Pilot | Result |
| --- | --- |
| Native refresh | 90 Hz request before begin, unsupported 73 Hz rejection, 90→120→90→120: pass, exit 0, no runtime errors |
| GOOGLE WSI refresh | Same: pass |
| CV WSI refresh | Same: pass |
| Calibrated CV WSI refresh | Same: pass |
| Service WSI overlay, 90 Hz | Camera readback plus actual overlay readback: 6,104 samples, 3 opaque application samples, zero mismatches; exit 0 |
| Service WSI camera-only, 120 Hz | 6,104 samples, zero alpha/grayscale errors, no startup error; exit 0 |
| Hosted WSI overlay, 90 Hz | Camera plus actual overlay readback: 6,104 samples, 4 opaque application samples, zero mismatches; visibility handoff and exit pass |
| Native 90 Hz shutdown, three repeats | All exited 0, positive presentation feedback, no GPU-fault errors |
| Missing WSI passthrough bridge | Application rejects unavailable capability before rendering; service has no per-frame errors |

All refresh checks verified each requested physical rate in the app. These short
runs establish functional switching, not comparative performance. The no-bridge
hardware negative test stops at the app's capability check; the real multi
compositor unit test separately verifies unsupported creation-result propagation.

Camera readbacks have `nonblack=5788`, `bad_alpha=0`, `bad_opaque=0`,
`bad_gray=0`, `completed=1`. Overlay coverage is small but nonzero in this light
scene; it is a pixel sanity check, not broad visual or calibration validation.

## Automated regressions and evidence

The final warning-free macOS build passes **79/79 CTests** (21.97 s). This
includes the new real-producer cancellation case (13 assertions), unsupported
passthrough propagation, custom GOOGLE budget preservation and existing GPU
missing-signal/future-value tests. Python analysis/capture tests pass **23/23**.
The Ubuntu 24.04/GCC 13.3 default-options build is warning-free and the full
Linux suite passes **51/51** (306.11 s, Docker x86_64 emulation). Three native
90 Hz shutdown pilots all exit 0 with positive presentation feedback and no
GPU-fault errors. The runtime code is unchanged between these passing checks.

`functional-summary.json` and `assertions.json` in the fixed-capture directory
check all ten positive hardware sessions, the four complete refresh cycles,
nonzero opaque overlay samples and the expected pre-session no-bridge rejection.
All capture manifests confirm display/job restoration. The independent final
check in `final-restoration.json` confirms 4000×2040 at 90 Hz, the original
installed service registered and idle, no experiment job, and no remaining
experiment processes. Nothing was committed, pushed or installed.

Local evidence in the main checkout:

- `build/review/wsi-headset-fixed-20261015/`: refresh, passthrough and native
  shutdown captures, plus the intentional unsupported-capability negative case.
- `build/review/wsi-fix-*.log`: builds, CTests, Python and runner output.
- `build/review/wsi-migration/review-followup.patch` and
  `review-followup-snapshot.json`: complete portable source and file hashes.

Full renderer reprojection diagnostics remain disabled in this build. No new
latency or motion-to-photon claim is made. The original failed captures remain
intact as historical evidence; they are not silently reclassified as passing.

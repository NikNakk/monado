<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# macOS / PS VR2 environment toggle audit

Audit of every `DEBUG_GET_ONCE_*` option and raw `getenv()` added on
`macos-wine-openvr-legacy-unity` since its merge-base with `combined`
(`9c1dbc39`, which is also the tip of `combined`). Snapshot: `eb607ca`,
2026-09-29. This document is analysis only. It does not change runtime defaults.

The inventory was generated from `git diff -U0 9c1dbc39 HEAD` over
`*.c *.h *.m *.mm *.cpp`. It keeps each added line that reads the environment
through `DEBUG_GET_ONCE_*`, `getenv()` or the `u_wait_macos_env_*` helpers. It
then groups the results by environment-variable name.

- 139 added lines contain `DEBUG_GET_ONCE_*` or `getenv(`: 81 option
  declarations and 58 raw `getenv` calls.
- These cover **130 (file, variable) pairs** and **99 distinct variable
  names**. The other lines are repeated reads of the same name within one
  file (for example two `PSVR2_TIMING_TRACE_DIR` reads in `comp_renderer.c`),
  the `u_wait.h` `getenv(name)` helper bodies, the `HOME` lookup in the OpenXR
  test and the four presence checks in `psvr2_eye.c`.
- 8 of the 99 names are **out of scope** (see [Out of scope](#out-of-scope)),
  leaving **91 classified names**.

"Intro" is the oldest commit on the branch whose diff adds the variable-name
string to that file (`git log -S`). "Last" is the newest commit touching a line
that names the variable or its `debug_get_*` accessor (`git log -G`).

> **Repository split note (2026-10-01):** Wine-specific build, test and
> compatibility tooling has moved to `NikNakk/macos-wine-xr`. The remaining Wine protocol has also moved into that
> project's transitional proxy (2026-10-02). Its transport rows are removed;
> the dated counts and findings remain historical audit evidence.

## Isolated Metal XPC endpoints (2026-10-02)

`XRT_MACOS_METAL_IPC_SERVICE_NAME` selects the Mach-service name used by
Metal clients, listeners and the optional broker. An unset or empty value keeps
`org.freedesktop.monado.metal-ipc`. Set it before starting the process and use
exactly the same name in the LaunchAgent's `MachServices` entry and every client.
Pair it with a separate `XDG_RUNTIME_DIR` for an independent service. It does not
change the Unix IPC protocol or select a compositor. Linux ignores the override.

This is for isolated testing alongside the registered hardware service. See
[Wine native-client endpoint validation](macos-wine-in-process-endpoint.md).
The inventory counts below remain the historical audit snapshot.

## PS Sense runtime 6DoF opt-in (2026-10-04)

The [integration procedure](macos-pssense-6dof-integration.md) is the current
build, replay and hardware-run reference. Set these in the **service**, before
constructing devices; they also work in an in-process runtime:

| Variable | Default | Purpose |
| --- | --- | --- |
| `PSVR2_SENSE_6DOF` | off | Start four-camera Sense tracking with the joint solver + EKF. |
| `PSVR2_SENSE_6DOF_CALIBRATION` | unset | Required absolute mode-4 calibration path; invalid/missing calibration leaves the existing fallback. |
| `PSVR2_LED_DETECTOR_RECORD` | unset | Path: open the headset's LED detector stream (USB interface 8) on its own, without the other auxiliary streams or a camera-mode change, and record it compactly (header plus populated records). Read with `scripts/psvr2_led_detector_dump.py`. The session script sets it with `PSVR2_SENSE_RECORD_LED_DETECTOR=1`. |
| `PSVR2_LED_DETECTOR_BLOBS` | off | Track the Sense controllers from the headset's own LED detections (USB interface 8) instead of blob detection on the camera images. The solvers and LED scheduling are unchanged. Works with `PSVR2_CAMERA_STREAMS=0`: the driver then announces exposures from the detector stream. |
| `PSVR2_LED_DETECTOR_VTS_OFFSET_US` | 0 | Detector device time minus camera VTS for the same exposure, used until camera frames supply it (measured 0 on 5 Oct). |
| `PSVR2_LATENCY_DIAG` | off | Diagnostic: log `LATENCY_DIAG` every 5 s with percentiles of arrival minus exposure time for the two mode-4 camera transfers and the LED detector stream (which it opens). With `CONSTELLATION_TRACKER_LOG=info`, `JOINT_STATUS` gives `pose_age_ms_p50/p95`, exposure to the end of the joint solve. |
| `PSVR2_BLOB_PIXEL_THRESHOLD` | 80 (50 with `PSVR2_SENSE_6DOF`) | Pixel threshold. |
| `PSVR2_BLOB_REQUIRED_THRESHOLD` | 180 (120 with `PSVR2_SENSE_6DOF`) | Blob seed threshold. |
| `PSVR2_BLOB_MAX_WIDTH` | 50 | Largest detected blob width. |

When requested, the common helper sets defaults with `setenv(..., 0)` so
explicit overrides win: camera streams on/mode 4, robust camera clock on with
200 ppm limit, `CONSTELLATION_TRACKER_JOINT=1`, `PSSENSE_FILTER=1`, future LED
scheduling, online gyro bias, 250 µs clock snap, LED correction and LED-off on
exit. It enables LED bootstrap, first controller R, phase hint 16350 µs,
keep-lock/LED-shape/strict/tracking/coverage options, and wide pulse period ID
32 (1.6 ms). Since 5 Oct it also sets the Sony-like LED profile
(`PSSENSE_CLOCK_STEADY=1`, `PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID=32`,
`PSSENSE_LED_BROAD_S=10`, `PSSENSE_LED_LATCH_INTERVAL_MS=1000`,
`PSSENSE_LED_NOMINAL_CYCLE=1`), blob thresholds 50/120 and
`CONSTELLATION_TRACKER_ORIENTED_BOOTSTRAP=1`, all validated in the 5 Oct
CLI series and two OpenBrush runs without lockouts. See `sense_tracking_defaults` in
`targets/common/target_psvr2_sense_tracking.c` for exact names. Future LED
scheduling, experimental clock/filter/bootstrap/model options remain off unless
requested explicitly or through this 6DoF switch. Linux's full-stream default
and macOS's conservative camera-off default are unchanged when it is absent.
The detailed tuning/evidence remain in [optical tracking](pssense-optical-tracking.md)
and [front-end evaluation](macos-pssense-mr2940-frontend-evaluation.md).

`PSSENSE_CLOCK_STEADY=1` (set by the helper) holds a controller's
host/device clock offset once its LED schedule has locked, advancing only at a
fitted drift rate (see `drivers/pssense/pssense_clock.h`). With `PSSENSE_TIMING_DIAG=1`, every controller also logs
`PSSENSE_CLOCK`: the lowest-latency (arrival, controller clock) pair of each
100 ms window, the max-tracked envelope, the offset in use, and the hold and
rate, for offline replay of clock mappings.

LED scheduling options. The helper sets `PSSENSE_LED_LATCH_INTERVAL_MS=1000`,
`PSSENSE_LED_BROAD_S=10` and `PSSENSE_LED_NOMINAL_CYCLE=1`; the defaults below
are the driver's own, which apply without the helper:

| Variable | Default | Purpose |
| --- | --- | --- |
| `PSSENSE_LED_LATCH_INTERVAL_MS` | 0 | Keep the latched PRESCAN anchor and re-latch only after this interval or on a content change (bootstrap/sync output, phase, period). 0 latches every exposure, as before. Sony's driver latches about every 1000 ms. |
| `PSSENSE_LED_BROAD_S` | 0 | Once the LED bootstrap holds its lock: three PRESCAN anchors 1 s apart, then BROAD (`cycle_position` 0) for this many seconds, repeated. Probes are only granted outside BROAD. Logs `LED_BROAD event=start/end/abort`. |
| `PSSENSE_LED_BOOTSTRAP_BLOB_FALLBACK` | off | Let a phase probe whose reference window was untracked steer the lock by LED-shaped blob counts (previously always on with LED-shaped counts). |
| `PSSENSE_LED_BOOTSTRAP_FULL_SCAN_FALLBACK` | off | With a phase hint, fall back to the full (wide-pulse) scan after the hinted retries. Off keeps retrying hinted scans with backoff; every always-lit fault on 5 Oct began on entering a full scan or in a burst of scans. |
| `PSSENSE_LED_BOOTSTRAP_LOST_LIT_PERCENT` | 10 | Rescan a locked controller when fewer than this percentage of its camera reports were lit over a 300-exposure window, even if stray lit frames keep the 300-dark-frame rule from firing; 0 disables. |
| `PSSENSE_LED_NOMINAL_CYCLE` | off | Send Sony's constant `cycle_length` (50,050,050 thirds of a ns, one nominal 59.94 Hz frame) instead of the measured average, which changes on almost every latch. |
| `PSSENSE_LED_BLINK_SWEEP` | unset | Diagnostic: comma-separated `led_blink` values (8 hex digits in report byte order, e.g. `0affffff`, or 2 for byte 0 with `ff` after) stepped through once the LED lock is held, one latch per step (inside BROAD without leaving it). Probes are suspended while sweeping; each step logs `LED_BLINK_SWEEP`. |
| `PSSENSE_LED_BLINK_SWEEP_S` | 4 | Seconds per sweep step. |
| `PSSENSE_LED_BROAD_PERIOD_ID` | lock period | BROAD pulse period ID. Sony uses 42 (2.1 ms), which is historically associated with the always-lit fault in wide scans, so test it separately. |

`CONSTELLATION_TRACKER_ORIENTED_BOOTSTRAP=1` (set by the helper; the tracker's
own default is off) re-acquires a lost
device from its IMU orientation (carried into the optical world by the
alignment from earlier solves) when stereo bootstrap fails, from one camera's
blobs, and lowers the coverage limit from 0.8 to 0.5 for any solve anchored to
the IMU orientation, tracking included (see
`tracking/constellation/oriented_bootstrap.hpp`). Counted as `oriented=` in
`JOINT_STATUS` and `JOINT_LOSS`.

Build optimisation is required for both solver and runtime. Use the
`macos-sense-relwithdebinfo` preset; do not use an empty build type or Debug for
performance/hardware comparisons. The following audit counts are historical.

## Summary

| Category | Count | Meaning |
| --- | ---: | --- |
| (a) shipping behaviour | 38 | Should become a real setting or be hard-coded |
| (b) diagnostics / tracing | 17 | Keep, but group under one prefix and one parser |
| (c) dead A/B arm | 8 | Experiment concluded; one side won |
| (d) unclear | 28 | Needs a decision from you (see [Decisions needed](#decisions-needed)) |
| Out of scope | 8 | Upstream options that were only moved, or pre-existing |

**Hardware result, 2026-09-29: legacy presentation is the best mode.** Driven
CAMetalDisplayLink mode suffered unpredictable timing delays on the display-link
thread and did not improve on legacy. The code still defaults to the losing
mode: `XRT_MACOS_CAMETALDISPLAYLINK_MODE` is `driven` on macOS 14 and later. In
driven mode `comp_window_macos.m` force-disables the present worker, drawable
slot and early drawable (`4f887c3`). It also suppresses the CVDisplayLink
callback and the learned present offset, and uses plain `presentDrawable:`.

The toggles marked **L** below (7 (d), 3 (a)/(c)) are therefore the ones that
matter in practice. Legacy mode with today's defaults (worker, stale substitution
and drawable slot all off) presents inline on the compositor thread, which blocks
in `nextDrawable`. The judder ledger found that this "deterministically force[s]
missed refreshes". So simply flipping the default to `legacy` would not
reproduce the good result: the legacy *configuration* must be pinned too (D1).

### Final classification after chat-history evidence (2026-09-29)

This section supersedes the counts below and in the summary table. The
evidence was recovered from the project's earlier ChatGPT chats, with quotes
and dates, and checked against this audit. A value that was merely present in a
good run is treated as configuration evidence, not proof that it caused the
result.

**Final counts: (a) 43, (b) 20, (c) 28, (d) 0** (91 names).

(c), removable now **without** changing default behaviour (8 already removed on
`cleanup/toggles`; 16 more):

| Toggle | Evidence |
| --- | --- |
| `PRESENT_WORKER` (standalone), `PRESENT_STALE_SUBSTITUTE` | Superseded by the redesigned drawable-slot newest-frame worker (0 slot drops, ~119.88 Hz, 19–21 Sep). Stale substitution is ignored in slot mode. |
| `EARLY_DRAWABLE`, `PRESENT_IMMEDIATE`, `UNIQUE_PRESENT_SLOTS` | Never actually enabled in any recovered run. |
| `CLIENT_FRAME_DIVISOR`, `U_PACING_APP_FORCED_FRAME_DIVISOR` | Fixed divisors tested and worse: irregular 1/2/3-refresh holds, only ~62–65% two-refresh holds, judder. Removing the pacer divisor also removes the `u_pacing_app.c` `#include` wrapper. |
| `DISPLAY_RATE_DIVISOR` | Only `1` used. The improvement came from phase sync, not N. |
| `COMPOSITOR_QOS` | Early positive result contradicted by a later A/B. Final baseline has it off. |
| `DISABLE_DISPLAY_SYNC`, `DISABLE_FRAMEBUFFER_ONLY` | Display sync off: 0.83% vs 0.90% misses, the repeating miss pattern remained, and near cubes shimmered. Framebuffer-only was a no-op (already `NO`). |
| `WAIT_HYBRID_US` | Failed its purpose: median lateness 29.6 ms vs ~0.006 ms for full spin. |
| `PROCESS_ACTIVITY` | Never validly tested (hooks compiled out), then abandoned for XPC importance. |
| `METAL_XPC_EXTERNAL_BROKER` | Implemented, never run. |
| `MAX_DRAWABLES` | 2 never reduced end-to-end latency and repeatedly collapsed to ~60 Hz. Hard-code 3. |
| `LATE_RENDER_DESIRED_OFFSET_US` | +2 ms sometimes improved pose freshness, but the newest controlled evidence showed the deliberate offset could break cadence, and removing it restored ~120 Hz. The no-offset default is supported. It was tested with `DRAWABLE_SLOT=1`, though never as a clean slot-only on/off A/B independent of the CAMetalDisplayLink change. |

(c), removable only **after** the default flip (D1b):
`CAMETALDISPLAYLINK_LATENCY` and `_THREAD_PRIORITY` (driven-only; latency=2 was
never validly run, and realtime priority gave "no convincing large
improvement"), plus `PRESENT_PRELATCH_US` and `PRESENT_MIN_LEAD_US` (inert once
the minimum duration is the default; prelatch was already deprioritised). The
driven/hybrid backend and the `DRIVE` alias go at the same time.

Corrections to the resolution table below:

- **`CLIENT_FRAME_MIN_HOLD` is (a), not (c).** Elastic hold=2 gave 98.33% exact
  two-refresh holds and was "much smoother". A 60 Hz app on the 120 Hz headset
  is a wanted feature. Keep it as a real opt-in setting (default 0, as in the
  baseline), and consider an app-facing name.
- **`XRT_MACOS_XPC_IMPORTANCE` is (b), not a default.** (Later removed
  outright; see the fourth batch below.) An excellent 18 Sep
  run stayed RT97 throughout, but on 21 Sep the lease was acquired and the
  thread was still demoted to priority 4. Keep it as an opt-in client-side
  diagnostic until a controlled A/B shows a benefit.
- **`PRESENT_MIN_DURATION_US=8000` and time constraint 35/70 are known-good
  baseline values, not demonstrated optima.** No alternative-value sweep and no
  90 Hz test exist. Make them the defaults, and keep one A/B each on the list.
- **D6 resolved:** keep acceleration-only as a selectable predictor mode.
  Replay showed it the most pointwise accurate (median/p95 2.57/11.97 mm vs
  continuity 2.79/12.90). Continuity trades accuracy for smoothness.

Reclassified to (b): `WAIT_SPIN` (by far the best wait accuracy, <1 µs
lateness, but costs a core; keep as an opt-in diagnostic) and
`XRT_COMPOSITOR_DEPTH_REPROJECTION` (useful rotation-only kill-switch).
Separately, depth reprojection **on by default** showed silhouettes, trails and
holes, and none of the disocclusion-fill approaches on
`macos-depth-aware-reprojection` fixed that yet. The default is therefore now
**off** (rotation-only), and the toggle also gates the layer-squash path in
`comp_render_cs.c`, not just the fast path. This became necessary once the
Metal depth-format mapping from that branch was ported: depth swapchains
(including `Depth32Float_Stencil8`, Unreal's first choice) can now be created,
so apps that submit depth layers would otherwise switch depth reprojection on.
Set `XRT_COMPOSITOR_DEPTH_REPROJECTION=1` to experiment. The
`XR_KHR_composition_layer_depth` extension itself now also defaults off on
Apple (`XRT_FEATURE_OPENXR_LAYER_DEPTH`), so apps don't render and submit depth
the compositor would ignore; depth experiments need both switches.

`APP_RELEASE_SHARED_EVENT_WAIT_THREAD` is resolved as (a): **turn it on by default
in service builds**, as the code comment always intended.

- **Finding 3 is confirmed on hardware.** With the variable unset (service PIDs
  65220 and 65383), the service's `client_gpu.csv` has no semaphore events, so
  the app took the blocking release path.
- **Setting it in the app's environment works** (service 65753, UE 5.8 native
  Metal, reduced quality). All 1,708 frames arrived with `semaphore_pushed`,
  `semaphore_wait_start` and `semaphore_ready` events, and the service logged
  `Metal IPC Stage 4 semaphore active`.

| Service run | Release path | Presents >1.5× period | App frames held 1 refresh | App `draw_actual` median / p95 | Service wait for app GPU |
| --- | --- | ---: | ---: | --- | --- |
| 65220 | blocking | 2.15% | 97.9% | 7.25 / 7.99 ms | n/a |
| 65383 | blocking | 0.59% | 98.6% | 7.25 / 8.00 ms | n/a |
| 65753 | shared event | 0.98% | 98.6% | **2.07 / 2.57 ms** | 4.59 / 5.74 ms |

The app-side CPU cost per frame drops by ~5 ms. The GPU completion wait moves
into Monado's wait thread (median 4.6 ms), and cadence and latency are unchanged
within run-to-run noise: all runs ~120 Hz presentation, ~118 fps app, and
presented−desired 16.68 ms. The benefit should appear when the app is CPU-bound;
this reduced-quality scene was not. The fix is to include `xrt/xrt_config_build.h`
in `comp_metal_release_wait_thread.m`. That is a default change and belongs in
its own commit.

`XRT_MACOS_XPC_IMPORTANCE` was in fact reaching the app in all four sessions
(`XR_XPC_IMPORTANCE acquired` for app PIDs 65103, 65263, 65464 and 65637). The
compositor thread stayed at priority 97 with no policy transitions in these runs.
With Game Mode enabled for UE, however, it is still demoted 97→4, so the lease
does not achieve its purpose. It stays an opt-in (b) diagnostic while the
RunningBoard investigation continues.

### Resolution from the best legacy configuration (2026-09-29)

You supplied the environment of the best-performing run. The PSVR2 prediction
values in it equal the current code defaults, and so do the seven settings
removed on `cleanup/toggles`. Its presentation and scheduling settings resolve
16 of the 28 (d) entries:

| Toggle | Best run | Current default | New category |
| --- | --- | --- | --- |
| `XRT_MACOS_CAMETALDISPLAYLINK_MODE` | `legacy` | `driven` | (a), **default must change** |
| `XRT_MACOS_DRAWABLE_SLOT` | `1` (newest-frame slot worker) | off | (a), **default must change** |
| `XRT_MACOS_PRESENT_MIN_DURATION_US` | `8000` | 0 | (a), **default must change** |
| `XRT_MACOS_COMPOSITOR_TIME_CONSTRAINT` | `1` | off | (a), **default must change** |
| `XRT_MACOS_COMPOSITOR_COMPUTATION_PCT` / `_CONSTRAINT_PCT` | 35 / 70 | 36 / 72 | (a), hard-code 35/70 |
| `XRT_MACOS_XPC_IMPORTANCE` | `1` | off | (a), **default must change** (read in the *client* process) |
| `XRT_MACOS_MAX_DRAWABLES` | 3 | 3 | (a), hard-code |
| `XRT_MACOS_PRESENT_WORKER` (standalone worker) | 0 | 0 | (c) |
| `XRT_MACOS_EARLY_DRAWABLE` | 0 | 0 | (c) |
| `XRT_MACOS_PRESENT_STALE_SUBSTITUTE` | 0 (ignored in slot mode anyway) | 0 | (c) |
| `XRT_MACOS_PRESENT_IMMEDIATE` | 0 (stale path only) | 0 | (c) |
| `XRT_MACOS_UNIQUE_PRESENT_SLOTS` | 0 | 0 | (c) |
| `XRT_MACOS_LATE_RENDER_DESIRED_OFFSET_US` | unset | unset | (c) |
| `XRT_MACOS_CLIENT_FRAME_DIVISOR` / `_MIN_HOLD` | 0 / 0 | 0 / 0 | (c) |
| `XRT_MACOS_COMPOSITOR_QOS` | 0 | 0 | (c) |

Revised counts: **(a) 45, (b) 17, (c) 17, (d) 12**. The (c) figure includes the
eight already removed.

Consequences:

- **Stale substitution becomes dead code.** It is ignored whenever the drawable
  slot is on (`comp_window_macos_create` in `_latest.m`), so its ~400-line copy
  of the presenter can simply be deleted. Finding 6 (the passthrough omission)
  disappears with it, and part-2 step 2 (the substitution policy) is no longer
  needed. `_latest.m` would then hold only refresh-rate switching and the
  CAMetalLayer diagnostics.
- **`PRESENT_MIN_DURATION_US` makes `PRESENT_PRELATCH_US` and
  `PRESENT_MIN_LEAD_US` inert.** With a non-zero minimum duration, the
  `presentDrawable:atTime:` shim in `comp_window_macos_trace_buffer.h` calls
  `presentDrawable:afterMinimumDuration:` and discards the requested time. The
  two lead/prelatch values then only shape the `target_output_ns` and
  `metal_request_ns` trace columns. Once 8000 µs becomes the default, both are
  (c).
- **The 8000 µs minimum duration is tuned for 120 Hz** (8.33 ms period). At
  90 Hz, reachable via refresh switching, the period is 11.1 ms, so 8 ms still
  permits one present per refresh but no longer tracks the period. Consider
  deriving it from `display_period_ns` when hard-coding it; that would need a
  90 Hz headset check.
- **`XRT_MACOS_XPC_IMPORTANCE` is read in the OpenXR application process**
  (`ipc_client_compositor.c`), not by the service. It only took effect if the
  app was started from a shell with that environment. If the service was
  launched separately (LaunchAgent), check the service log for
  `XR_XPC_IMPORTANCE acquired` to confirm the lease was actually used in the
  best run.
- The environment still mentions eight variables that `cleanup/toggles`
  removes: seven set, plus `LATE_RENDER_LEAD_US`, which it unsets. They are
  ignored harmlessly, but can be deleted from the file.

Still (d), with no setting in the best run: `DISPLAY_RATE_DIVISOR`,
`U_PACING_APP_FORCED_FRAME_DIVISOR`, `DISABLE_DISPLAY_SYNC`,
`DISABLE_FRAMEBUFFER_ONLY`, `WAIT_SPIN`, `WAIT_HYBRID_US`, `PROCESS_ACTIVITY`,
`METAL_XPC_EXTERNAL_BROKER`, `APP_RELEASE_SHARED_EVENT_WAIT_THREAD`,
`DEPTH_REPROJECTION`, `CAMETALDISPLAYLINK_LATENCY` and `_THREAD_PRIORITY`. The
last two go if driven mode is dropped (D1b).

### Status on `cleanup/toggles`

The original eight (c) toggles are removed there, one commit per toggle or group. Each
commit keeps the default behaviour and keeps the affected CSV columns, filled
with the constant value:

| Commit | Removed | Now always |
| --- | --- | --- |
| psvr2: always predict translation over the full horizon | `PSVR2_FULL_LINEAR_HORIZON` | full horizon |
| psvr2: drop EMA-filtered velocity as a live predictor | `PSVR2_FILTERED_LINEAR_PREDICTION` | raw velocity; EMA stays trace-only |
| comp/macos: remove predicted-relative late-render lead | `XRT_MACOS_LATE_RENDER_LEAD_US` | no predicted-relative wait |
| comp/macos: always feed CVDisplayLink vblanks to the pacer | `XRT_MACOS_CVDISPLAYLINK_PACING` | feedback on |
| comp/macos: always use the Metal shared-event handoff when available | `XRT_MACOS_METAL_SHARED_EVENT_WAIT` | shared event, with auto-fallback |
| comp/macos: always defer compositor GPU timestamp readback | `XRT_MACOS_DEFER_GPU_TIMESTAMPS`, `XRT_MACOS_SKIP_BLOCKING_GPU_TIMESTAMPS` | deferred |
| comp/macos: always present asynchronously | `XRT_MACOS_ASYNC_PRESENT` | async |

Second batch (approved 2026-09-29), again one commit per toggle or group and
keeping default behaviour:

| Removed | Now always |
| --- | --- |
| `PRESENT_STALE_SUBSTITUTE`, `PRESENT_IMMEDIATE` (and the ~400-line stale presenter copy) | drawable-slot newest-frame worker is the only substitution |
| `DISABLE_DISPLAY_SYNC`, `DISABLE_FRAMEBUFFER_ONLY` | layer flags as initialised |
| `EARLY_DRAWABLE` | no compositor-thread prefetch |
| `PRESENT_WORKER` (standalone) | worker used only by drawable-slot mode |
| `MAX_DRAWABLES` | 3 |
| `DISPLAY_RATE_DIVISOR` | compositor runs every refresh |
| `UNIQUE_PRESENT_SLOTS` | no slot reservation |
| `LATE_RENDER_DESIRED_OFFSET_US` | no late-render wait (`late_render.csv` kept for pose timing) |
| `CLIENT_FRAME_DIVISOR` | elastic `CLIENT_FRAME_MIN_HOLD` kept instead |
| `COMPOSITOR_QOS` | time-constraint policy only |
| `U_PACING_APP_FORCED_FRAME_DIVISOR` | upstream single-file `u_pacing_app.c` restored |
| `WAIT_HYBRID_US` | spin or Mach wait only |
| `PROCESS_ACTIVITY` | files removed |

Then two separately revertable **default changes**:

1. `client/metal: enable app-release wait thread by default in service builds`
   fixes finding 3 by including `xrt/xrt_config_build.h`.
2. `comp/macos: default to the best measured legacy presentation config` sets
   mode `legacy`, drawable slot on, minimum present duration 8000 µs and the
   time constraint on at 35/70. It also adds a `tests_macos_displaylink_default`
   test.

Third batch:

- **Driven and hybrid backends removed.** This takes out
  `CAMETALDISPLAYLINK_MODE`, the `DRIVE` alias, `_LATENCY`, `_THREAD_PRIORITY`,
  `_DRIVE_TRACE_PATH`, the child-layer probe (`_PROBE`, `_TRACE_PATH`), four
  force-included selector-rewriting headers, the multi-compositor callback
  bridge and its test (~2,050 lines). The force-included multi header is renamed
  `comp_multi_system_macos_compositor_rt.h` and keeps `compositor_rt.csv` and
  the time-constraint wrapper.
- **The synchronous Metal present path is pruned.**
- **`comp_window_macos_latest.m` is folded into `comp_window_macos.m`**
  (part-2 step 1). Only `comp_window_macos_trace_buffer.h` is still
  force-included.
- **Correction:** the `METAL_XPC_EXTERNAL_BROKER` removal was reverted. It is
  (b), not (c): `scripts/macos/run-wine-openvr-native-trace.zsh` runs
  `monado-service` directly in the legacy Wine bridge bootstrap namespace, where it
  cannot host the launchd Mach service, and needs the broker for Metal handle
  transport.

Fourth batch (2026-09-30):

- **The XPC importance lease is removed.** `XRT_MACOS_XPC_IMPORTANCE`, the
  service's lease methods and XPC transactions, the captured XPC context
  (`os_macos_xpc_context_*`) and its wrapper around the multi-compositor frame
  all go. `XRT_MACOS_LAUNCHD_PROCESS_TYPE` goes too, and the LaunchAgent is
  `ProcessType=Interactive` again, as it was before the lease. The lease never
  prevented the Game Mode demotion: Game Mode backgrounds the service from
  outside (`ext_darwinbg=1`), which no importance boost overrides, and the one
  good 18 Sep run coincided with Game Mode being off. The in-process client
  compositor is the fix instead; see
  [macos-client-compositor-design.md](macos-client-compositor-design.md).

Fifth batch (2026-10-01), no default changes:

- **One parser for the timing traces.** `util/u_timing_trace.h` reads
  `PSVR2_TIMING_TRACE`, `PSVR2_TIMING_TRACE_FULLY_BUFFERED` and
  `PSVR2_TIMING_TRACE_DIR` once through `DEBUG_GET_ONCE_*`, and
  `u_timing_trace_open()` replaces about a dozen copies of the file-opening
  code. The names are unchanged; the rename proposed below has not been done.
  As with other Monado boolean options, `true`, `on` and `yes` now also enable
  them. Fully buffered captures request at least 16 MiB per trace; IMU now requests
  64 MiB. See the 2026-10-03 buffering correction in the timing diagnostics.
- **No more redefined functions.** The force-included headers and `-D`
  renames are gone (including the ones that redefined `fflush`, `setvbuf` and
  `presentDrawable`); callers call the macOS hooks by name.
- **Other raw `getenv` reads now use `DEBUG_GET_ONCE_*`**, including
  `MONADO_WINE_TCP_PORT` (finding 5 below) and the macOS wait diagnostics.

Added 2026-10-01: `XRT_MACOS_DISPLAY_LINK` (`ca` default since 2026-10-03,
`cv` legacy fallback), a comparison switch for migration from deprecated
CVDisplayLink. Corrected CA closely matches CV in native service and hosted
headset captures; the heavy UE/Game Mode pair favours CA in completion logs.
The low UE frame rate is an intentional stress condition. Keep the switch as
category (d) for comparison/fallback; fully buffered hosted repeats now physically favour CA under heavy load
(105.18 vs 100.08 Hz); 90 Hz still needs coverage. The selector belongs in the presenting process: the service
launchd environment for IPC compositing, or the app for client compositing.
Older macOS or CA creation failure falls back to CV automatically. See
[the default decision](macos-psvr2-timing-diagnostics.md#cadisplaylink-promoted-to-default--2026-10-03).

Still not done: `PRESENT_PRELATCH_US` and `PRESENT_MIN_LEAD_US` (inert under the
default minimum present duration, but kept because the Wine trace script still
sets `PRELATCH_US`).

The async commit is deliberately minimal. The synchronous branches inside
`macos_execute_present_job()` are now unreachable but still present, and
should be pruned in a follow-up that is compiled on macOS. The Objective-C
changes have **not** been compiled. Only Linux builds and syntax-only checks
of the C files' macOS paths were possible (`comp_renderer.c`, `psvr2.c`,
`comp_multi_system.c` with its force-included macOS headers,
`comp_multi_macos_displaylink.c` and `u_wait.h`, using Mach stub headers).

## Cross-cutting findings

1. **`PSVR2_TIMING_TRACE` is parsed 13 times, using four different truthiness
   rules.**
   - `DEBUG_GET_ONCE_BOOL_OPTION` treats anything except
     `false/off/no/n/f/0` (either case) as true, including an empty string. It
     is used in `psvr2.c`, `comp_renderer.c`, `comp_compositor.c`,
     `comp_window_macos.m`, `comp_metal_client.m` and
     `comp_multi_system_macos_trace.h`.
   - `!= "" && != "0"` is used in `u_pacing_app_base.c`,
     `comp_multi_compositor.c` and `ipc_server_handler.c`.
   - `comp_multi_system.c` additionally rejects `off` and `false`.
   - `== "1"` exactly is used in `comp_multi_system_macos_displaylink_drive.h`,
     `comp_window_macos_cametal_drive.h` and `comp_window_macos_cametal_probe.h`.

   So `PSVR2_TIMING_TRACE=true` enables most CSVs but silently skips the
   display-link and `compositor_rt` traces. `PSVR2_TIMING_TRACE_DIR` (11 copies)
   and `PSVR2_TIMING_TRACE_FULLY_BUFFERED` (7 copies) have the same duplication.
2. **Compile gating is inconsistent.** `XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS`
   (a CMake option, default OFF) compiles out the traces in `psvr2.c`,
   `comp_renderer.c`, the multi-system latch trace and the CAMetalDisplayLink
   drive trace. Release builds still compile in, and gate only at runtime, the
   traces in:
   - `comp_window_macos.m`
   - `u_pacing_app_base.c`
   - `comp_multi_compositor.c`
   - `ipc_server_handler.c`
   - `comp_metal_client.m`
   - `comp_compositor.c`
   - the probe
3. **`XRT_MACOS_APP_RELEASE_SHARED_EVENT_WAIT_THREAD` probably never defaults
   on.** `comp_metal_release_wait_thread.m` selects a `true` default under
   `#if defined(XRT_FEATURE_SERVICE)`. That macro comes only from
   `xrt/xrt_config_build.h`, and no include chain from this file reaches that
   header. `comp_metal_glue.c`, next to it, includes it explicitly. The
   effective default is therefore `false` in service builds as well, contrary
   to the comment. This was checked statically, not on macOS. Fixing it would
   change runtime behaviour, so it is left for you (D8).
4. **`u_wait.h` calls `getenv()` three times on every `u_wait_until()`.** On
   macOS it reads `XRT_MACOS_WAIT_TIMING`, `XRT_MACOS_WAIT_SPIN` and
   `XRT_MACOS_WAIT_HYBRID_US` without caching, inside the compositor's pacing
   wait.
5. **The Wine TCP port has two names.** The server reads `IPC_WINE_TCP_PORT`
   (`DEBUG_GET_ONCE_NUM`, `0` = off). The Windows client reads
   `MONADO_WINE_TCP_PORT` (raw `getenv`, unset = named pipe).
6. **The stale-substitution copy has drifted from the main presenter.**
   `comp_window_macos_present_stale()` builds `struct macos_present_job` without
   `.passthrough_active` or `.passthrough_has_application_layers`.
   `macos_execute_present_job_stale()` never calls `macos_passthrough_encode()`.
   With `XRT_MACOS_PRESENT_STALE_SUBSTITUTE=1` on the legacy path, XR
   passthrough is therefore silently not drawn. This is not reachable with
   default settings. See [Presenter structure](#presenter-structure-comp_window_macos_latestm).
7. **The doc and code agree on defaults.** The defaults table at the top of
   `macos-psvr2-timing-diagnostics.md` matches the code at `eb607ca`. The
   release-default flip in `59150ab` was partially reverted by the
   CAMetalDisplayLink merge `e152634`: `DRAWABLE_SLOT`, `PRESENT_STALE_SUBSTITUTE`,
   `COMPOSITOR_QOS`, `PRESENT_MIN_DURATION_US` and `LATE_RENDER_DESIRED_OFFSET_US`
   went back to off. The older per-experiment docs, such as
   `psvr2-position-prediction.md` and `psvr2-continuity-prediction.md`, still
   show the pre-`59150ab` defaults (0 / 5 mm / 80 ms).

### Proposed prefix for (b)

Use `XRT_MACOS_TRACE` (bool) plus `XRT_MACOS_TRACE_DIR`, `XRT_MACOS_TRACE_BUFFERED`
and `XRT_MACOS_TRACE_<STREAM>` for per-stream overrides (`DRIVER`, `REPROJECTION`,
`IPC`, `WAIT`, `DISPLAYLINK`, `PROBE`, `SWAPCHAIN_REUSE`). Parse them in one
place, for example `u_macos_trace.h`, which returns a cached struct. Keep the
old `PSVR2_TIMING_TRACE*` names as aliases for one release, because existing
capture scripts and docs use them. Driver-local diagnostics, such as
`PSSENSE_INPUT_DIAGNOSTICS`, can keep their driver prefix and move to `*_LOG`
levels instead.

## Inventory

Paths are relative to `src/xrt/`. Ranges are the clamps applied in code.

### PS VR2 driver: `drivers/psvr2/psvr2.c`, `psvr2_eye.c`

| Variable | Default | What it changes | Intro / last | Cat |
| --- | --- | --- | --- | :-: |
| `PSVR2_TIMING_LOG` | off | 240-query pose-timing summary at WARN | `ab0d861` / `ab0d861` | b |
| `PSVR2_DRIVER_TIMING_TRACE` | on | Sub-switch that lets driver CSVs (imu/slam/pose/prediction/horizon) be suppressed while other traces stay on; macOS + diag build only | `f5ae9ae` / `f5ae9ae` | b |
| `PSVR2_FILTERED_LINEAR_PREDICTION` | off | Live EMA-filtered linear velocity instead of raw (only when acceleration is off) | `eebbbd1` / `b0034e4` | **c** |
| `PSVR2_LINEAR_VELOCITY_ALPHA` | 0.25 | EMA coefficient; also feeds the EMA candidate scored in `prediction.csv`/`horizon.csv` | `eebbbd1` / `9bde48d` | b |
| `PSVR2_FULL_LINEAR_HORIZON` | on | Predict translation over the full SLAM-to-target interval instead of the post-gyro residual | `b0034e4` / `59150ab` | **c** |
| `PSVR2_ACCELERATION_PREDICTION` | on (macOS; off elsewhere) | Bounded-acceleration position model; implied by continuity | `b0034e4` / `59150ab` | a |
| `PSVR2_ACCELERATION_ALPHA` | 0.25 [0,1] | Acceleration EMA coefficient | `b0034e4` / `b0034e4` | a |
| `PSVR2_ACCELERATION_GAIN` | 0.5 [0,1] | Fraction of acceleration correction applied | `b0034e4` / `b0034e4` | a |
| `PSVR2_ACCELERATION_LIMIT` | 2.0 m/s² [0,20] | Pre-filter acceleration clip | `b0034e4` / `b0034e4` | a |
| `PSVR2_ACCELERATION_MIN_SPEED` | 0.01 m/s [0,1] | Below this, fall back to raw full-horizon | `b0034e4` / `b0034e4` | a |
| `PSVR2_ACCELERATION_HORIZON_MS` | 90 [0,120] | Cap on the acceleration-correction horizon | `b0034e4` / `59150ab` | a |
| `PSVR2_CONTINUITY_PREDICTION` | on (macOS; off elsewhere) | Host-time decaying transition between SLAM updates; implies acceleration and full horizon | `a23d038` / `59150ab` | a |
| `PSVR2_CONTINUITY_TAU_MS` | 4 [0.5,20] | Continuity decay constant | `a23d038` / `a23d038` | a |
| `PSVR2_CONTINUITY_LIMIT_MM` | 7.5 [0,20] | Continuity residual cap | `a23d038` / `59150ab` | a |
| `PSVR2_RECENTER_ON_FIRST_POSE` | off | Re-origin tracking at the first valid pose | `d46c551` / `eeb0915` | a |
| `PSVR2_RECENTER_EYE_HEIGHT_M` | 1.6 | Floor height assumed by first-pose recenter | `eeb0915` / `eeb0915` | a |
| `PSVR2_AUXILIARY_STREAMS` | off on macOS, on elsewhere | Enable all non-core USB streams | `8f591e0` / `8f591e0` | a |
| `PSVR2_CAMERA_STREAMS` | off | Enable camera (passthrough) USB streams | `cdedde3` / `ad1d7bf` | a |
| `PSVR2_GAZE_STREAMS` | on | Provision the gaze USB interface (the eye tracker is still enabled lazily) | `cdedde3` / `de85300` | a |
| `PSVR2_STAGE_SPACE` | off | Advertise a STAGE reference space | `cdedde3` / `eeb0915` | a |
| `PSVR2_HEADSET_HAPTICS` | off | Advertise headset rumble (firmware-gated on stock units) | `fe7d2ac` / `fe7d2ac` | a |
| `PSVR2_GAZE_YAW_OFFSET_DEG` | 0 [-20,20] | Overrides the persisted user gaze calibration | `53f356c` / `39b19b7` | a |
| `PSVR2_GAZE_PITCH_OFFSET_DEG` | 0 [-20,20] | Same, for pitch | `53f356c` / `39b19b7` | a |
| `PSVR2_GAZE_YAW_GAIN` | 1.0 [0.5,1.5] | Same, for yaw gain | `53f356c` / `39b19b7` | a |
| `PSVR2_GAZE_PITCH_GAIN` | 1.0 [0.5,1.5] | Same, for pitch gain | `53f356c` / `39b19b7` | a |

Notes on classification:

- **`FULL_LINEAR_HORIZON` is (c).** The judder ledger says the shortened
  horizon is a "real tracking defect ... experimentally fixed". Continuity (on
  by default) already implies full horizon, so `=0` changes behaviour only when
  continuity and acceleration are also both off. It then re-enables the known
  defect.
- **`FILTERED_LINEAR_PREDICTION` is (c).** The ledger marks EMA smoothing
  "Disfavoured", and it is already ignored whenever acceleration or continuity
  is on (the default). Only the live selection is removed. `LINEAR_VELOCITY_ALPHA`
  stays as (b) because the EMA candidate is still scored in the traces.
- **The predictor mode should be one setting, not two booleans.**
  `ACCELERATION_PREDICTION` and `CONTINUITY_PREDICTION` should become a single
  `PSVR2_POSITION_PREDICTOR=raw|acceleration|continuity`, default
  `continuity`. The seven numeric tunables should become compile-time
  constants; the code already duplicates their defaults as fallbacks. Whether
  `acceleration`-only is worth keeping as a user-visible mode is D6.
- **These driver options also apply on Linux**, because none of them is
  OS-gated. The `59150ab` defaults therefore changed Linux behaviour too.

### PlayStation Sense controllers: `drivers/pssense/pssense_driver.c`

| Variable | Default | What it changes | Intro / last | Cat |
| --- | --- | --- | --- | :-: |
| `PSSENSE_SYNTHETIC_POSITION` | off | Fixed HMD-relative controller positions for untracked controllers | `7ccfc37` / `f91755f` | a |
| `PSSENSE_SYNTHETIC_ARM_MODEL` | off | Arm-model positions instead of fixed offsets | `f91755f` / `f91755f` | a |
| `PSSENSE_INDEX_PROFILE` | off | Advertise the Valve Index profile for app compatibility | `737fcd4` / `737fcd4` | a |
| `PSSENSE_INPUT_DIAGNOSTICS` | off | Log raw input changes | `f91755f` / `f91755f` | b |

### Compositor renderer and shared compositor code

| Variable | File | Default | What it changes | Intro / last | Cat |
| --- | --- | --- | --- | --- | :-: |
| `XRT_COMPOSITOR_FORCE_ATW_OFF_ON_APPLE` | `main/comp_renderer.c` | off | Disable ATW on macOS (WiVRn-style clients that do their own ATW) | `ab0d861` / `ab0d861` | a |
| `XRT_MACOS_LATE_RENDER_LEAD_US` | `main/comp_renderer.c` | 0 (off) | Legacy predicted-display-relative late-render wait | `cfedf46` / `cfedf46` | **c** |
| `XRT_MACOS_LATE_RENDER_DESIRED_OFFSET_US` | `main/comp_renderer.c` | unset (off) | Wait until `desired_present + offset` before the final dispatch | `9755374` / `59150ab` | d |
| `XRT_MACOS_SKIP_BLOCKING_GPU_TIMESTAMPS` | `main/comp_renderer.c` | off | Skip compositor GPU timestamp readback entirely | `5a118a8` / `84bfa46` | **c** |
| `XRT_MACOS_DEFER_GPU_TIMESTAMPS` | `main/comp_renderer.c` | on | Read the previous frame's timestamps after its fence, not the current frame's (blocking) | `84bfa46` / `59150ab` | **c** |
| `XRT_MACOS_REPROJECTION_TRACE` | `main/comp_renderer.c`, `multi/comp_multi_system.c` | off (on with `PSVR2_TIMING_TRACE`) | `reprojection.csv` / `reprojection_source.csv` | `dffa6d2` / `dffa6d2` | b |
| `XRT_COMPOSITOR_DEPTH_REPROJECTION` | `render/render_compute.c` | off (was on) | Opt-in depth-aware timewarp, fast and layer-squash paths (shared code) | `615a1da` / `615a1da` | b |
| `XRT_COMPOSITOR_WAIT_IMAGE_QUEUE_IDLE` | `util/comp_swapchain.c` | off | `vkQueueWaitIdle` before swapchain image reuse (sync-bug bisection; shared code) | `1d54237` / `1d54237` | b |
| `XRT_COMPOSITOR_LOG_SWAPCHAIN_GPU_REUSE` | `util/comp_swapchain_gpu_reuse.c` | off | Log service Metal swapchain GPU reuse | `c6b6d5c` / `c6b6d5c` | b |

- **`LATE_RENDER_LEAD_US` is (c).** `macos-psvr2-timing-diagnostics.md` calls it
  "retained only as the legacy predicted-display-relative diagnostic",
  superseded by the desired-relative mode because of positive feedback with the
  learned present offset.
- **`DEFER_GPU_TIMESTAMPS` and `SKIP_BLOCKING_GPU_TIMESTAMPS` are (c) as a
  group.** The ledger shows that blocking readback costs about 3.7 ms per frame
  ("Major experimental confound ... keep deferred"). Deferral won, and skipping
  was the earlier, cruder arm.

### macOS presenter: `main/comp_window_macos.m`, `comp_window_macos_latest.m`, force-included headers

In the notes column, **L** means active only in legacy (or hybrid) mode, not in
the current default driven mode. Legacy is the mode that tested best, so these
are the ones that matter in practice.

| Variable | File | Default | What it changes | Intro / last | Cat |
| --- | --- | --- | --- | --- | :-: |
| `XRT_MACOS_CAMETALDISPLAYLINK_MODE` | `multi/comp_multi_macos_displaylink.c` | `driven` (macOS ≥ 14), else `legacy` | Presenter cadence source: `legacy`, `driven` or `hybrid` | `3c82b46` / `3c82b46` | a |
| `XRT_MACOS_CAMETALDISPLAYLINK_DRIVE` | same | unset (= driven) | Older alias: `0` forces legacy. Documented as the opt-out | `c949862` / `3c82b46` | a |
| `XRT_MACOS_CAMETALDISPLAYLINK_LATENCY` | `main/comp_window_macos_cametal_drive.h`, `_probe.h` | 1 | `preferredFrameLatency` (1 or 2); parsed twice | `e55dddd` / `7bbac93` | d |
| `XRT_MACOS_CAMETALDISPLAYLINK_THREAD_PRIORITY` | same | `interactive` | Display-link thread: `normal`, `interactive` or `realtime`; parsed twice | `e55dddd` / `7bbac93` | d |
| `XRT_MACOS_CAMETALDISPLAYLINK_PROBE` | `main/comp_window_macos_cametal_probe.h` | off | Independent child-layer timing probe (hybrid mode also uses it) | `3a074ef` / `7bbac93` | b |
| `XRT_MACOS_CAMETALDISPLAYLINK_TRACE_PATH` | same | unset | Probe CSV path | `3a074ef` / `7bbac93` | b |
| `XRT_MACOS_CAMETALDISPLAYLINK_DRIVE_TRACE_PATH` | `main/comp_window_macos_cametal_drive.h` | unset | Driven-mode CSV path (also enables it) | `c949862` / `c949862` | b |
| `XRT_MACOS_REFRESH_RATE_HZ` | `main/comp_window_macos_latest.m` | 0 (keep) | Switch the PS VR2 mode to 90/120 Hz at startup | `588b4d0` / `588b4d0` | a |
| `XRT_MACOS_PASSTHROUGH_FOV_DEG` | `main/comp_window_macos.m` | 150 | Passthrough reprojection FOV | `2b84217` / `8973ec2` | a |
| `XRT_MACOS_PASSTHROUGH_CONVERGENCE_MILLI` | same | 100 (0.1) | Passthrough convergence | `2b84217` / `8e14ef4` | a |
| `XRT_MACOS_PASSTHROUGH_BRIGHTNESS_PERCENT` | same | 160 | Passthrough brightness gain | `2b84217` / `2b84217` | a |
| `XRT_MACOS_DISPLAY_RATE_DIVISOR` | same | 1 | Compositor `frame_interval = period × N` | `ab0d861` / `ab0d861` | d |
| `XRT_MACOS_MAX_DRAWABLES` | same | 3 | `CAMetalLayer.maximumDrawableCount` (2 or 3) | `54436b1` / `54436b1` | d |
| `XRT_MACOS_ASYNC_PRESENT` | same | on | Drop the synchronous `waitUntilCompleted`; protect in-flight IOSurfaces | `cf390bf` / `59150ab` | **c** |
| `XRT_MACOS_METAL_SHARED_EVENT_WAIT` | same | on | Export the render-complete timeline as `MTLSharedEvent` and wait on the GPU (auto-fallback to CPU wait) | `cf390bf` / `401a349` | **c** |
| `XRT_MACOS_CVDISPLAYLINK_PACING` | same | on | Feed CVDisplayLink vblanks into the fake pacer. **L** | `017d9a9` / `017d9a9` | **c** |
| `XRT_MACOS_PRESENT_MIN_LEAD_US` | same (+ `_latest.m`) | 2000 | Minimum lead when picking the target output slot. **L** | `1f5cd7c` / `1f5cd7c` | a |
| `XRT_MACOS_PRESENT_PRELATCH_US` | same (+ `_latest.m`) | 2000 | `presentDrawable:atTime:` request = target − prelatch. **L** | `ac3b696` / `ac3b696` | a |
| `XRT_MACOS_PRESENT_WORKER` | same | off | Off-thread `nextDrawable`/present worker. **L** | `fe5e436` / `4f887c3` | d |
| `XRT_MACOS_DRAWABLE_SLOT` | same | off | Async one-drawable slot + newest-frame worker. **L** | `627e98b` / `4f887c3` | d |
| `XRT_MACOS_EARLY_DRAWABLE` | same | off | Prefetch the drawable before the late pose latch. **L** | `5feee10` / `4f887c3` | d |
| `XRT_MACOS_PRESENT_STALE_SUBSTITUTE` | `main/comp_window_macos_latest.m` | off | After a ≥ 1.25-refresh `nextDrawable` stall, present the newer pending frame. **L** | `af5bfb8` / `6a94b8e` | d |
| `XRT_MACOS_PRESENT_IMMEDIATE` | same | off | Plain `presentDrawable:` on the stale path. **L** | `f985d4e` / `588b4d0` | d |
| `XRT_MACOS_DISABLE_DISPLAY_SYNC` | same | off | `CAMetalLayer.displaySyncEnabled = NO` after init | `97c849f` / `30078b5` | d |
| `XRT_MACOS_DISABLE_FRAMEBUFFER_ONLY` | same | off | `CAMetalLayer.framebufferOnly = NO` after init | `57a46b2` / `57a46b2` | d |
| `XRT_MACOS_UNIQUE_PRESENT_SLOTS` | `main/comp_window_macos_trace_buffer.h` | off | Force successive timed presents ≥ 1 learned period apart. **L** | `838f651` / `d11d38f` | d |
| `XRT_MACOS_PRESENT_MIN_DURATION_US` | same | 0 (off) | Replace `atTime:` with `afterMinimumDuration:`. **L** | `d11d38f` / `d11d38f` | d |
| `PSVR2_TIMING_TRACE` | 13 files (see finding 1) | off | Master switch for all timing CSVs | `ca55a39` / `38624ef` | b |
| `PSVR2_TIMING_TRACE_DIR` | 11 files | `/tmp` | CSV directory | `ca55a39` / `38624ef` | b |
| `PSVR2_TIMING_TRACE_FULLY_BUFFERED` | 7 files, incl. `comp_window_macos_trace_buffer.h`, `psvr2_trace_buffer.h` | off | at least 16 MiB (IMU 64 MiB); periodic flushes suppressed after 2026-10-03 correction | `ed2e5a3` / `eb607ca` | b |

- **`ASYNC_PRESENT` is (c).** The ledger says "Synchronous Metal
  `waitUntilCompleted` ... **Not required**". The default has been `on` since
  `59150ab`, and the timing doc says async "remain[s] enabled". Driven mode
  also submits asynchronously on the compositor thread.
- **`METAL_SHARED_EVENT_WAIT` is (c).** The ledger says queue-wide idle was
  "replaced by exact timeline/shared-event handoff". The CPU-wait fallback when
  `VK_EXT_metal_objects` cannot export an event stays; only the manual
  `=0` override goes.
- **`CVDISPLAYLINK_PACING` is (c).** It was added in `017d9a9` as an A/B
  alongside the clock-domain fix. The fix is recorded as "Real defect fixed",
  and there is no remaining reason to disable vblank feedback in legacy/hybrid
  mode.
- **`PRESENT_MIN_LEAD_US` and `PRESENT_PRELATCH_US` should be hard-coded (a).**
  They are tunables, not A/B arms. The ledger says prelatch "Not a root-cause
  fix" and shows no measurable difference between 0 and 2000.

### Multi-client compositor, pacing and scheduler: `multi/`, `auxiliary/util/`

| Variable | File | Default | What it changes | Intro / last | Cat |
| --- | --- | --- | --- | --- | :-: |
| `XRT_MACOS_CLIENT_FRAME_DIVISOR` | `multi/comp_multi_system_macos_trace.h` | 0 (off) | Deliver client frames only every N system frames | `032a2b8` / `2bced9d` | d |
| `XRT_MACOS_CLIENT_FRAME_MIN_HOLD` | same | 0 (off) | Elastic minimum hold per client frame | `2bced9d` / `2bced9d` | d |
| `U_PACING_APP_FORCED_FRAME_DIVISOR` | `auxiliary/util/u_pacing_app.c` | 0 (off) | Phase-locked app pacing at display/N | `5d567bc` / `6570eff` | d |
| `XRT_MACOS_COMPOSITOR_QOS` | `multi/comp_multi_system_macos_trace.h` | off | `USER_INTERACTIVE` QoS on the compositor thread | `a9d01d0` / `59150ab` | d |
| `XRT_MACOS_COMPOSITOR_TIME_CONSTRAINT` | same | off | Mach time-constraint policy anchored to the display period | `233b75e` / `233b75e` | d |
| `XRT_MACOS_COMPOSITOR_COMPUTATION_PCT` | same | 36 | Time-constraint computation % of the period | `233b75e` / `233b75e` | d |
| `XRT_MACOS_COMPOSITOR_CONSTRAINT_PCT` | same | 72 | Time-constraint constraint % of the period | `233b75e` / `233b75e` | d |
| `XRT_MACOS_WAIT_TIMING` | `auxiliary/util/u_wait.h` | off | Per-wait stderr line (not cached; see finding 4) | `200b0bc` / `dbc4e6d` | b |
| `XRT_MACOS_WAIT_SPIN` | same | off | Busy-wait the whole `u_wait_until` | `74a938c` / `dbc4e6d` | d |
| `XRT_MACOS_WAIT_HYBRID_US` | same | 0 (off) | Sleep, then spin the final N µs | `dbc4e6d` / `dbc4e6d` | d |
| `XRT_IPC_FRAME_TIMING` | `multi/comp_multi_compositor.c` | off | Layer-submission stall timing log | `3fc29bf` / `3fc29bf` | b |

### Service, IPC and Metal client: `ipc/`, `targets/service/`, `compositor/client/`

| Variable | File | Default | What it changes | Intro / last | Cat |
| --- | --- | --- | --- | --- | :-: |
| `XRT_MACOS_EXIT_ON_DISPLAY_LOSS` | `ipc/server/ipc_server_mainloop_apple_xpc.m` | off (LaunchAgent sets `1`) | Stop the service when the PS VR2 display disappears | `4135890` / `4135890` | a |
| `XRT_MACOS_DISPLAY_LOSS_DELAY_MS` | same | 3000 | Grace period before stopping | `4135890` / `4135890` | a |
| `XRT_MACOS_DISPLAY_LOSS_SHUTDOWN_WATCHDOG_MS` | same | 5000 | Hard-exit watchdog after display-loss shutdown | `eaa4d7f` / `eaa4d7f` | a |
| ~~`XRT_MACOS_LAUNCHD_PROCESS_TYPE`~~ | removed 2026-09-30 | `Interactive` | LaunchAgent `ProcessType` | `b38ccd0` / removed | c |
| `XRT_MACOS_PROCESS_ACTIVITY` | `ipc/server/ipc_server_macos_activity.m` | unset (off) | Process-lifetime `NSProcessInfo` activity (`user-interactive` or `latency-critical`) | `69ca4d0` / `69ca4d0` | d |
| ~~`XRT_MACOS_XPC_IMPORTANCE`~~ | removed 2026-09-30 | — | Client held an XPC importance lease for the session | `472c930` / removed | c |
| `XRT_MACOS_METAL_XPC_EXTERNAL_BROKER` | `ipc/shared/ipc_metal_xpc_service.m` | off | Retired override; service startup rejects it | `1cec3f6` / `1cec3f6` | d |
| `XRT_MACOS_APP_RELEASE_SHARED_EVENT_WAIT_THREAD` | `compositor/client/comp_metal_release_wait_thread.m` | on in all builds (2026-09-30; finding 3 fixed earlier) | App-side swapchain release via the compositor wait thread | `b714613` / `b714613` | d |

### Tests and tools

| Variable | File | Default | What it changes | Intro / last | Cat |
| --- | --- | --- | --- | --- | :-: |
| `PSVR2_OPENXR_LOADER` | `targets/psvr2_openxr_test/psvr2_openxr_test.mm` | search `$HOME` | OpenXR loader path for the test app | `85832da` / `afd08e5` | b |

### Out of scope

| Variable | Why |
| --- | --- |
| `U_PACING_APP_LOG`, `_MIN_TIME_MS`, `_MIN_MARGIN_MS`, `_USE_MIN_FRAME_PERIOD`, `_IMMEDIATE_WAIT_FRAME_RETURN`, `_IMMEDIATE_WAIT_FRAME_RETURN_BELOW_REFRESH`, `_ALIGN_PREDICTED_DISPLAY_TIME_TO_APP_PERIOD` | Upstream Monado options (present on `main`). `6570eff` moved them to `u_pacing_app_base.c` when `u_pacing_app.c` became an `#include` wrapper, the same pattern as the presenter. `3c60714` touched `USE_MIN_FRAME_PERIOD`'s call site only. |
| `MACOS_RUNTIME_PROBE_SUBMIT_FRAME` | Already existed in `tests/tests_macos_runtime_probe.c` at the merge-base; only touched by `20338a1`. |

## Decisions needed

Each item names the (d) toggles it resolves. The recommendation is mine; the
decision needs hardware validation that I cannot do.

- **D1. Which exact legacy configuration was best?** You have said that legacy
  beats driven. The remaining question is which legacy variant. The last
  recorded legacy baseline (`macos-psvr2-stale-substitution.md`, 2026-09-11) is
  `ASYNC_PRESENT=1`, `METAL_SHARED_EVENT_WAIT=1`, `PRESENT_WORKER=1`,
  `PRESENT_STALE_SUBSTITUTE=1`, `DRAWABLE_SLOT=0`, `EARLY_DRAWABLE=0`,
  `MAX_DRAWABLES=3`, `LATE_RENDER_DESIRED_OFFSET_US=2000`, `PRESENT_MIN_LEAD_US=2000`,
  `PRESENT_PRELATCH_US=2000` and deferred GPU timestamps. After that, `59150ab`
  made the drawable-slot newest-frame worker (`07f16e2`), `PRESENT_MIN_DURATION_US=8000`
  and `COMPOSITOR_QOS=1` release defaults, before driven mode replaced them.
  Please give the environment of the run you consider best. The answer settles
  seven **L** (d) toggles, D2, and possibly `COMPOSITOR_QOS`. The winners become
  hard-coded legacy defaults, and `DRAWABLE_SLOT`/`STALE_SUBSTITUTE` (whichever
  lost), `EARLY_DRAWABLE`, `PRESENT_IMMEDIATE`, `UNIQUE_PRESENT_SLOTS` and
  possibly `PRESENT_MIN_DURATION_US` become (c).
- **D1b. Flip the default mode to legacy, and drop driven/hybrid?** This changes
  runtime defaults, so it needs your explicit go-ahead, and it must land together
  with the D1 configuration (see above). Hybrid also depends on a
  CAMetalDisplayLink callback thread, via a child layer used as the cadence
  source. Was it tested, or does it share driven's problem? If both go:
  - `CAMETALDISPLAYLINK_MODE` and the `DRIVE` alias,
    `CAMETALDISPLAYLINK_LATENCY`/`_THREAD_PRIORITY`, and
    `CAMETALDISPLAYLINK_DRIVE_TRACE_PATH` all go.
  - `CAMETALDISPLAYLINK_PROBE`/`_TRACE_PATH` could remain as a standalone
    diagnostic, or go too.
  - About 2,200 lines of CAMetalDisplayLink-only code go with them:
    `comp_window_macos_cametal_{drive,drive_cv,idle_black,probe}.h`,
    `multi/comp_multi_macos_displaylink.[ch]`,
    `comp_multi_system_macos_displaylink_drive.h` and
    `tests_macos_displaylink.cpp`, plus their call sites.
- **D2. `LATE_RENDER_DESIRED_OFFSET_US`.** It was the winning setting (2000 µs)
  on the legacy path and was briefly a release default. It was turned off when
  driven mode landed. If your best legacy run used it, it should become the
  legacy default (a). Otherwise it is an open experiment.
- **D3. Any fixed-divisor mode?** There are three overlapping mechanisms:
  `XRT_MACOS_DISPLAY_RATE_DIVISOR` (compositor), `XRT_MACOS_CLIENT_FRAME_DIVISOR`
  / `CLIENT_FRAME_MIN_HOLD` (multi-system latch) and
  `U_PACING_APP_FORCED_FRAME_DIVISOR` (app pacer). No outcome is recorded for
  any of them. I recommend keeping at most one, as a real "app runs at display/N"
  setting, if 60 Hz-on-120 Hz is a product need.
- **D4. Compositor-thread scheduling.** This covers `COMPOSITOR_QOS`,
  `COMPOSITOR_TIME_CONSTRAINT` (+ 2 percentages), `WAIT_SPIN`, `WAIT_HYBRID_US`,
  `PROCESS_ACTIVITY`, `XPC_IMPORTANCE` and `METAL_XPC_EXTERNAL_BROKER`. This was
  the RunningBoard realtime-to-timeshare investigation (2026-09-17/18), and the
  docs stop before a result. Which, if any, helped under Game Mode / Unreal? The
  winners become (a) and the rest (c). `EXTERNAL_BROKER` in particular looks
  like a one-off A/B.
- **D5. `DISABLE_DISPLAY_SYNC` and `DISABLE_FRAMEBUFFER_ONLY`.** These are live
  in every mode, and no result is recorded. Delete them unless you
  remember a finding.
- **D6. Is `acceleration` without `continuity` worth keeping as a user
  mode?** This decides whether `PSVR2_ACCELERATION_PREDICTION` survives inside a
  `PSVR2_POSITION_PREDICTOR` enum.
- **D7. `MAX_DRAWABLES`.** The "3 drawables" conclusion (2 is lower latency
  but more juddery) was reached on the legacy worker path, which is the mode you
  are keeping. Hard-code 3 unless your best run used 2. (`CAMETALDISPLAYLINK_LATENCY`
  and `_THREAD_PRIORITY` are decided by D1b.)
- **D8. `APP_RELEASE_SHARED_EVENT_WAIT_THREAD`.** Was the wait thread meant to
  be on in service builds (finding 3)? If so, the one-line fix is to include
  `xrt/xrt_config_build.h`, but that turns it on for everyone and needs headset
  validation. Otherwise flip the default to plain `false` and fix the comment.
- **D9. `XRT_COMPOSITOR_DEPTH_REPROJECTION`.** Resolved: kept as an opt-in
  diagnostic (b), default **off** until a disocclusion fill works on hardware.

## Presenter structure: `comp_window_macos_latest.m`

### What exists today

The macOS target is one translation unit that CMake builds as
`comp_window_macos_latest.m`, assembled in three layers:

1. **`-include` headers** (set in `compositor/main/CMakeLists.txt` via
   `COMPILE_OPTIONS`) are injected, in this order, before any source line:
   - `comp_window_macos_trace_buffer.h`: `#define fflush`, `#define setvbuf`,
     `#define presentDrawable monadoPresentDrawable` (min-duration and
     unique-slot logic in an `NSObject` category).
   - `comp_window_macos_cametal_probe.h`: `#define setDrawableSize`.
   - `comp_window_macos_cametal_idle_black.h`, which `#include`s
     `comp_window_macos_cametal_drive.h`:
     `#define setDrawableSize/nextDrawable/presentDrawable` again, chained.
     It also sets a temporary `#define present` for the idle-black path.
   - `comp_window_macos_cametal_drive_cv.h`: `#define CVDisplayLinkStart(...)`
     and a stricter `#define nextDrawable`.
2. **`#include "comp_window_macos.m"`** (2,409 lines), with
   `comp_window_macos_create` and `comp_target_factory_macos` renamed to
   `*_legacy`. The renamed `comp_target_factory_macos_legacy` is an unreferenced
   exported global.
3. **The rest of `comp_window_macos_latest.m`** (~900 lines). It wraps
   `create` and overrides six vtable slots:
   - `init_pre_vulkan`: CAMetalLayer diagnostics and `REFRESH_RATE_HZ`.
   - `get_refresh_rates`, `get_current_refresh_rate` and
     `request_refresh_rate`: 90/120 Hz physical mode switching. This is
     **shipping behaviour**.
   - `present` and `destroy`: stale substitution.

So every `[layer nextDrawable]`, `[cmd presentDrawable:…]` and
`CVDisplayLinkStart()` in `comp_window_macos.m` actually calls up to three
chained replacements, and nothing at the call site says so.
`macos_execute_present_job_stale()` (~290 lines) and
`comp_window_macos_present_stale()` (~55 lines) are copies of the worker
branches of `macos_execute_present_job()` and `comp_window_macos_present()`,
with 30 lines of substitution inserted. They have already diverged: the missing
passthrough fields are finding 6. The same `#include`-and-rename pattern is used
by `u_pacing_app.c`, and a force-include with `#define os_thread_helper_name`
and `multi_compositor_deliver_any_frames` is used by `comp_multi_system.c`.

### Proposal

Legacy is the mode being kept, so the legacy presenter, worker and substitution
code is the product path, and step 2 is worth doing. It also fixes finding 6
(stale path drops passthrough), which becomes user-visible if the D1 answer is
worker + stale substitution. Step 3 depends on D1b.

**Step 1: fold the wrapper into the main file.** This is mechanical and
behaviour-preserving, about +10/−25 lines net, with ~900 lines moved.

- Append the non-`#include` body of `_latest.m` to `comp_window_macos.m`.
- Rename the base `comp_window_macos_create` to a `static
  comp_window_macos_create_base`.
- Delete the rename macros and the dead `comp_target_factory_macos_legacy`.
- Point CMake at `comp_window_macos.m`, keeping the same `-include` list.

The preprocessed token stream is the same apart from identifier names, so the
behaviour cannot change. Refresh-rate switching then lives next to the code it
overrides. I did **not** apply this step, because I can't compile Objective-C in
this environment (no macOS host). It is the safest first change to make on a
Mac: the build either compiles or it doesn't, and there is nothing to validate
on the headset.

**Step 2: one presenter with a pluggable substitution policy.** About −330/+80
lines; it needs a headset run.

```c
/* Called on the worker after nextDrawable returns. May swap *job for a newer
 * pending job; the caller retires the old one. */
struct macos_present_policy {
	const char *name;
	bool (*after_drawable)(struct comp_window_macos *cwm,
	                       struct macos_present_job *job,
	                       uint64_t drawable_wait_ns);
};
static const struct macos_present_policy macos_policy_none;          /* returns false */
static const struct macos_present_policy macos_policy_stale_125;     /* current stale logic */
static const struct macos_present_policy macos_policy_newest_slot;   /* current drawable-slot selection */
```

`macos_execute_present_job()` calls `cwm->policy->after_drawable()` at the one
point where the stale copy differs, after `nextDrawable`. Both `*_stale`
functions are then deleted, and passthrough and every later fix apply to all
policies. `XRT_MACOS_PRESENT_STALE_SUBSTITUTE` and `XRT_MACOS_DRAWABLE_SLOT`
collapse into one `XRT_MACOS_PRESENT_POLICY=none|stale|newest` toggle. The
behavioural risk is that the base worker branch has picked up small differences
from the copy (trace columns, the `image_reuse_wait` source, the `wait_mode`
string), and these must be reconciled one by one.

**Step 3: remove the selector macros.** If D1b drops driven and hybrid, most
of the macro layer simply goes away. Three of the four `-include` headers
(`cametal_probe.h`, `cametal_idle_black.h` → `cametal_drive.h`, `cametal_drive_cv.h`)
exist only to redirect `nextDrawable`, `presentDrawable`, `setDrawableSize`,
`CVDisplayLinkStart` and `present` to the display-link drawable. Deleting them,
together with the rest of the ~2,200 CAMetalDisplayLink-only lines, leaves only
`comp_window_macos_trace_buffer.h`, whose `presentDrawable` rename carries the
`UNIQUE_PRESENT_SLOTS`/`PRESENT_MIN_DURATION_US` experiments. That rename should
become a plain function called at the one `atTime:` site, or go entirely if D1
rejects both experiments. This is roughly −2,300/+30 lines, and the headset
check is simply "legacy still behaves as before".

If driven or hybrid must stay, replace the macros with explicit hooks instead
(about −250/+150 lines, touching every present call site, needs a headset run):

```c
struct macos_presenter_backend {
	id<CAMetalDrawable> (*acquire_drawable)(struct comp_window_macos *cwm);          /* layer vs display-link */
	void (*present)(struct comp_window_macos *cwm, id<MTLCommandBuffer> cmd,
	                id<CAMetalDrawable> d, uint64_t target_ns);                     /* atTime / plain / driven-complete */
	void (*idle_present)(struct comp_window_macos *cwm, id<CAMetalDrawable> d);    /* idle-black */
};
```

The driven and legacy backends are chosen once in `create`. This removes the
`NSObject` categories (process-wide method injection), the `#define
presentDrawable`/`nextDrawable`/`setDrawableSize`/`CVDisplayLinkStart`/`present`
rewrites, and the `-include` list. The trace-buffer `fflush`/`setvbuf` macros
go away with the single trace helper from finding 1.

| Scope | Driven/hybrid dropped (D1b yes) | Driven/hybrid kept |
| --- | --- | --- |
| Step 1: fold wrapper | ~35 changed lines, mechanical | same |
| Step 2: substitution policy | ~−330/+80, headset A/B; losing policies from D1 deleted | same |
| Step 3: selector macros | ~−2,300/+30, delete driven/hybrid backend | ~−250/+150, explicit backend hooks |

Recommended order: step 1, then D1b (default flip to the D1 configuration, as its
own commit so it can be reverted on its own), then step 3, then step 2.


### Retired CA compositor-thread experiment (2026-10-03)

`XRT_MACOS_CA_COMPOSITOR` has been removed. Both execution inside the CA callback
and a deferred render on its run loop failed to demonstrate a benefit over
ordinary CA pacing. The latter kept callbacks short, but its five-pair comparison
had slightly worse physical cadence and a longer typical latency tail. This does
not retire CADisplayLink pacing or client-hosted compositing; both are retained.
See the [follow-up decision](macos-psvr2-timing-diagnostics.md#deferred-run-loop-results-and-retirement).

`PSVR2_TIMING_TRACE=1` retains the useful passive `ca_callback.csv` and
`renderer_stage.csv` diagnostics. CA callbacks only record clock timing; their
historical frame columns remain zero. Renderer stages separate the previous GPU
fence wait, feedback, submission, draw dispatch, acquisition and present enqueue.

## Shared tracking experiment (2026-10-03)

`XRT_MACOS_SHARED_TRACKING=1` is **off by default** and read in the client.
With `XRT_MACOS_CLIENT_COMPOSITOR=1`, it requests the PS VR2 driver's read-only
tracking snapshot and predicts future head/view poses locally. The service
publishes automatically on request; no service environment override is needed.
Use matching newly built service and client binaries. Startup/recenter and
historical poses keep the service path, as do unsupported devices/inputs and
general IPC space-overseer operations. Snapshot contention retains the last
coherent state, with the existing 500 ms SLAM tracking-loss rule. It cannot
remove USB/source-update stalls.

`PSVR2_TIMING_TRACE=1` records `shared_tracking.csv` when this path is active;
`PSVR2_TIMING_TRACE_FULLY_BUFFERED=1` applies to it too. Inspect publication,
SLAM and IMU age, validity and fallback fields. See the
[transport design](macos-client-compositor-design.md#shared-ps-vr2-tracking-experiment--2026-10-03)
and the hardware evidence in the timing diagnostics.

The prepared moving-head runner checks zero-byte required traces throughout its
window and verifies nonempty files after normal shutdown. Shared tracking version
2 records actual USB callback/SLAM receipt and raw device timestamps alongside
returned poses. See [the capture protocol](macos-psvr2-timing-diagnostics.md#prepared-moving-head-freshness-capture--2026-10-03).

With timing tracing and full buffering enabled on macOS, the compositor worker
and service main loop can acknowledge owned `.flush-request` markers in the
trace directory. The capture runner uses these only before/after measurement,
checks source health after warmup, and requires fixed file sizes inside the
window. New `present_scheduled` and `appkit_pump` traces are passive; presentation
and Game Mode defaults are unchanged. See the
[completed-frame investigation](macos-psvr2-timing-diagnostics.md#completed-frame-presentation-investigation--2026-10-03).

## Pixel-sample diagnostics cleanup — 2026-10-03

`XRT_COMPOSITOR_LOG_APPLE_SAMPLES=1` now controls debug buffer allocation and
GPU pixel readbacks, as well as their output. With the default `0`, these
copies and their barriers are absent. Samples require four-byte RGBA/BGRA
color images with transfer-source usage; multisampled sources are skipped.
Unsupported images are skipped without failing normal rendering. Target
readbacks honor the requested final layout. The previous log-only gating
left the GPU work active every frame. See the
[inherited compositor audit](macos-inherited-compositor-audit.md).

## Sense joint solver isolation (2026-10-04)

`CONSTELLATION_TRACKER_JOINT=1` now also requires the caller's
`T_CONSTELLATION_TRACKER_FLAGS_ALLOW_JOINT` flag. Only the Sense runtime and
PS VR2 diagnostic/replay callers set it. Rift retains its original per-camera
solver even when the process inherits this environment option.

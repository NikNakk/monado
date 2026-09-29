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
  names**. The remaining lines are `getenv(name)` helper bodies, the `HOME`
  lookup in the OpenXR test and the four presence checks in `psvr2_eye.c`.
- 8 of the 99 names are **out of scope** (see [Out of scope](#out-of-scope)),
  leaving **91 classified names**.

"Intro" is the oldest commit on the branch whose diff adds the variable-name
string to that file (`git log -S`). "Last" is the newest commit touching a line
that names the variable or its `debug_get_*` accessor (`git log -G`).

## Summary

| Category | Count | Meaning |
| --- | ---: | --- |
| (a) shipping behaviour | 38 | Should become a real setting or be hard-coded |
| (b) diagnostics / tracing | 17 | Keep, but group under one prefix and one parser |
| (c) dead A/B arm | 8 | Experiment concluded; one side won |
| (d) unclear | 28 | Needs a decision from you (see [Decisions needed](#decisions-needed)) |
| Out of scope | 8 | Upstream options that were only moved, or pre-existing |

The single biggest lever is **D1: whether the legacy (non-CAMetalDisplayLink)
presentation path still has to be supported**. `XRT_MACOS_CAMETALDISPLAYLINK_MODE`
defaults to `driven` on macOS 14 and later. In that mode `comp_window_macos.m`
force-disables the present worker, drawable slot and early drawable (`4f887c3`).
It also suppresses the CVDisplayLink callback and the learned present offset, and
uses plain `presentDrawable:`. That makes 7 of the 28 (d) entries, and 3 (a)/(c)
entries, inert by default (marked **L** below). If you drop legacy/hybrid mode,
all 10 become (c), along with most of `comp_window_macos_latest.m`.

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
| `PSVR2_ACCELERATION_PREDICTION` | on | Bounded-acceleration position model; implied by continuity | `b0034e4` / `59150ab` | a |
| `PSVR2_ACCELERATION_ALPHA` | 0.25 [0,1] | Acceleration EMA coefficient | `b0034e4` / `b0034e4` | a |
| `PSVR2_ACCELERATION_GAIN` | 0.5 [0,1] | Fraction of acceleration correction applied | `b0034e4` / `b0034e4` | a |
| `PSVR2_ACCELERATION_LIMIT` | 2.0 m/s² [0,20] | Pre-filter acceleration clip | `b0034e4` / `b0034e4` | a |
| `PSVR2_ACCELERATION_MIN_SPEED` | 0.01 m/s [0,1] | Below this, fall back to raw full-horizon | `b0034e4` / `b0034e4` | a |
| `PSVR2_ACCELERATION_HORIZON_MS` | 90 [0,120] | Cap on the acceleration-correction horizon | `b0034e4` / `59150ab` | a |
| `PSVR2_CONTINUITY_PREDICTION` | on | Host-time decaying transition between SLAM updates; implies acceleration and full horizon | `a23d038` / `59150ab` | a |
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
| `XRT_COMPOSITOR_DEPTH_REPROJECTION` | `render/render_compute.c` | on | Kill-switch for depth-aware compute timewarp (shared code) | `615a1da` / `615a1da` | d |
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

In the notes column, **L** means inert in the default driven CAMetalDisplayLink
mode: it only matters on macOS < 14 or with `XRT_MACOS_CAMETALDISPLAYLINK_MODE=legacy|hybrid`.

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
| `PSVR2_TIMING_TRACE_FULLY_BUFFERED` | 7 files, incl. `comp_window_macos_trace_buffer.h`, `psvr2_trace_buffer.h` | off | 16 MiB stdio buffers, flush only at close | `ed2e5a3` / `eb607ca` | b |

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
| `XRT_MACOS_LAUNCHD_PROCESS_TYPE` | `targets/service/macos_xpc_control.m` | `Adaptive` | LaunchAgent `ProcessType` (`Adaptive` or `Interactive`) | `b38ccd0` / `b38ccd0` | a |
| `IPC_WINE_TCP_PORT` | `ipc/server/ipc_server_mainloop_apple.c` | 0 (off) | Loopback TCP listener for Wine clients | `68b8620` / `68b8620` | a |
| `MONADO_WINE_TCP_PORT` | `ipc/client/ipc_client_connection.c` | unset (named pipe) | Windows client connects over TCP instead | `17f9d14` / `17f9d14` | a |
| `XRT_MACOS_PROCESS_ACTIVITY` | `ipc/server/ipc_server_macos_activity.m` | unset (off) | Process-lifetime `NSProcessInfo` activity (`user-interactive` or `latency-critical`) | `69ca4d0` / `69ca4d0` | d |
| `XRT_MACOS_XPC_IMPORTANCE` | `ipc/shared/ipc_metal_xpc.m` | off | Client holds an XPC importance lease for the session | `472c930` / `472c930` | d |
| `XRT_MACOS_METAL_XPC_EXTERNAL_BROKER` | `ipc/shared/ipc_metal_xpc_service.m` | off | Route Metal handles through the standalone broker (launchd vs manual A/B) | `1cec3f6` / `1cec3f6` | d |
| `XRT_MACOS_APP_RELEASE_SHARED_EVENT_WAIT_THREAD` | `compositor/client/comp_metal_release_wait_thread.m` | intended on for service builds; **effectively off** (finding 3) | App-side swapchain release via the compositor wait thread | `b714613` / `b714613` | d |

### Tests and tools

| Variable | File | Default | What it changes | Intro / last | Cat |
| --- | --- | --- | --- | --- | :-: |
| `PSVR2_OPENXR_LOADER` | `targets/psvr2_openxr_test/psvr2_openxr_test.mm` | search `$HOME` | OpenXR loader path for the test app | `85832da` / `afd08e5` | b |
| `MONADO_IOSURFACE_DONE_FILE` | `tests/windows/macos_wine_d3d11_iosurface_producer.cpp` | unset | Handshake file for the Wine D3D11 IOSurface test | `45de2f5` / `45de2f5` | b |

### Out of scope

| Variable | Why |
| --- | --- |
| `U_PACING_APP_LOG`, `_MIN_TIME_MS`, `_MIN_MARGIN_MS`, `_USE_MIN_FRAME_PERIOD`, `_IMMEDIATE_WAIT_FRAME_RETURN`, `_IMMEDIATE_WAIT_FRAME_RETURN_BELOW_REFRESH`, `_ALIGN_PREDICTED_DISPLAY_TIME_TO_APP_PERIOD` | Upstream Monado options (present on `main`). `6570eff` moved them to `u_pacing_app_base.c` when `u_pacing_app.c` became an `#include` wrapper, the same pattern as the presenter. `3c60714` touched `USE_MIN_FRAME_PERIOD`'s call site only. |
| `MACOS_RUNTIME_PROBE_SUBMIT_FRAME` | Already existed in `tests/tests_macos_runtime_probe.c` at the merge-base; only touched by `20338a1`. |

## Decisions needed

Each item names the (d) toggles it resolves. The recommendation is mine; the
decision needs hardware validation that I cannot do.

- **D1. Keep the legacy / hybrid presentation path?** This covers the 10 **L**
  toggles: the (d) items `PRESENT_WORKER`, `DRAWABLE_SLOT`, `EARLY_DRAWABLE`,
  `PRESENT_STALE_SUBSTITUTE`, `PRESENT_IMMEDIATE`, `UNIQUE_PRESENT_SLOTS` and
  `PRESENT_MIN_DURATION_US`, plus the (a)/(c) items `CVDISPLAYLINK_PACING`,
  `PRESENT_MIN_LEAD_US` and `PRESENT_PRELATCH_US`. It also covers
  `CAMETALDISPLAYLINK_MODE=hybrid`, the `DRIVE` alias and most of
  `comp_window_macos_latest.m`.
  - *If you keep it* (macOS 13 support, or hybrid as a fallback), the
    evidence ledger names worker + stale substitution as the best legacy
    baseline. Make that combination the legacy default, and treat `DRAWABLE_SLOT`
    (converted stalls into ~2% drops), `EARLY_DRAWABLE`, `PRESENT_IMMEDIATE`,
    `UNIQUE_PRESENT_SLOTS` and `PRESENT_MIN_DURATION_US` as (c). None of the
    last three has a recorded result.
  - *If you drop it*, delete the worker, slot, prefetch, stale and timed-present
    machinery and the four selector-rewriting headers. Most of the part-2
    restructure then becomes unnecessary.
- **D2. `LATE_RENDER_DESIRED_OFFSET_US`.** It was the winning setting (2000 µs)
  on the legacy path and was briefly a release default. It was turned off when
  driven mode landed. Has it been measured in driven mode? If not, it is an open
  experiment rather than dead code.
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
  in driven mode too, and no result is recorded. Delete them unless you
  remember a finding.
- **D6. Is `acceleration` without `continuity` worth keeping as a user
  mode?** This decides whether `PSVR2_ACCELERATION_PREDICTION` survives inside a
  `PSVR2_POSITION_PREDICTOR` enum.
- **D7. `MAX_DRAWABLES`, `CAMETALDISPLAYLINK_LATENCY` and `_THREAD_PRIORITY`.**
  The "3 drawables" conclusion was reached on the legacy worker path. In driven
  mode these interact with `preferredFrameLatency`. Should they be re-measured
  before hard-coding 3 / 1 / interactive?
- **D8. `APP_RELEASE_SHARED_EVENT_WAIT_THREAD`.** Was the wait thread meant to
  be on in service builds (finding 3)? If so, the one-line fix is to include
  `xrt/xrt_config_build.h`, but that turns it on for everyone and needs headset
  validation. Otherwise flip the default to plain `false` and fix the comment.
- **D9. `XRT_COMPOSITOR_DEPTH_REPROJECTION`.** This is an A/B kill-switch in
  shared compositor code, with no recorded result. Keep it as a diagnostic (b),
  or remove it?

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

I recommend deciding D1 first, because it changes which of these is worth doing.

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

**Step 2: one presenter with a pluggable substitution policy.** This only makes
sense if D1 keeps the legacy path. About −330/+80 lines; it needs a headset run.

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

**Step 3: replace the selector macros with explicit hooks.** This is needed with
or without D1. About −250/+150 lines, touching every present call site; it needs
a headset run.

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

| Scope | If D1 keeps legacy | If D1 drops legacy |
| --- | --- | --- |
| Step 1 | ~35 changed lines, mechanical | same |
| Step 2 | ~−330/+80, headset A/B | not needed: delete ~600 lines of worker, slot and stale code instead |
| Step 3 | ~−250/+150, headset A/B | ~−450/+60: only the driven backend remains |

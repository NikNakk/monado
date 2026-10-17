<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Current WSI experiment and review follow-up

The implementation is committed on the WSI topic branch; the
[fork checkpoint](macos-wsi-vulkan-passthrough.md#fork-checkpoint-17-october-2026)
records review units and pending hardware validation. Raw captures/build products
remain local and ignored.

## Unified presenter selector (17 October 2026)

Use `XRT_MACOS_PRESENTER=wsi|wsi-adaptive|native` in both service and hosted
client launch environments. `wsi` means calibrated CV WSI; `wsi-adaptive`
selects the adaptive display-timing pacer. Unset remains `native` until WSI's
outstanding moving-head checks pass. Empty/invalid values warn once and fall
back to native. A common process-wide selector feeds detection, hosted factory
selection, compositor settings and WSI pacing. The four old WSI selection
booleans are removed; they do not override the new selector.

The runner accepts `--conditions native native-timed wsi wsi-adaptive` and emits
only the new selector (`native-timed` remains a native timing diagnostic).
Its analyzers still support archived manifests with old labels/flags. Historical
commands below using the old runtime flags or runner conditions are records,
not current launch instructions. Compute/backend and passthrough feature
controls remain independent. No runtime installation or default promotion.

Software validation: warning-free macOS build; 85/85 CTests pass, including
fresh-process checks of native, WSI, adaptive and invalid selectors with
conflicting retired flags present. The runtime choice is cached once per
process. All 26 runner/analysis/comparison Python tests pass, including new
manifests without legacy flags and archived captures. Linux's default build
and portable parser test pass; unrelated Linux suites are not repeated for
macOS-only selector wiring. Logs are `build/review/wsi-presenter-selector-*`.
No new headset run is started; the private-output visual gate remains pending.


## Private compute output candidate

The user isolates `XRT_COMPOSITOR_COMPUTE=1` as the trigger in WSI and reports
native working. The [private-output candidate](macos-wsi-vulkan-passthrough.md#private-compute-output-candidate)
now renders WSI compute output into private images before a GPU-ordered Vulkan
copy to presentation images, retaining calibration and rotation. Native stays
default. Earlier performance captures below predate this candidate; its real
display behavior and moving-head quality await headset access.

## Vulkan camera replacement (16 October 2026)

The blocking Metal camera bridge is replaced by a
[portable Vulkan camera pass](macos-wsi-vulkan-passthrough.md), with PS VR2
calibration/source policy kept in a separate adapter. The first short physical
captures reach about 118–119 presents/s at 120 Hz; the final single-batch
implementation takes about 0.38 ms median camera CPU submission time. Offscreen pixel and pending-resource tests pass. Moving-head
service/hosted acceptance remains pending; native is still default.
The old Metal startup verifier and `--verify-passthrough` runner option are
removed. The rest of this ledger preserves the path to that change.

## Calibrated WSI passthrough prepared (16 October 2026)

The user accepts WSI presentation as good enough for the eventual default.
Before switching, WSI now carries the existing native passthrough calibration
and rotation correction. This working-tree change is based on `ec4c02591`.
Native remains the runtime default: camera-only and gentle blended alignment
pass, but rapid-motion shimmer is worse than native. Cadence/bridge follow-up
below blocks promotion.
After that check, the requested policy is calibrated CV WSI by default, native
behind an optional selector and adaptive GOOGLE pacing behind a separate toggle.
Here “calibrated CV WSI” means presentation-offset calibration; camera lens
calibration is independent and uses the saved per-headset file.

The WSI camera pass reuses the native calibration parser/projection helpers and
production Metal shader. It automatically loads the serial-bound
`psvr2/passthrough.json` in Monado's user config, with the same explicit
`XRT_MACOS_PASSTHROUGH_CALIBRATION` override and empty-override behavior.
It stores optical rays alongside static UVs, associates each uploaded camera
texture with its header-time tracked orientation, and takes the display head
orientation from the exact target image used by the compute renderer. The
rotation formula, stereo freshness/pose gates and static fallback are the native
ones. `XRT_MACOS_PASSTHROUGH_ROTATION=0` provides the same static-map comparison.
Disabling scene timewarp also disables camera correction. No USB, timestamp,
IPC, Linux or native-presenter behavior changes are included. WSI's existing
blocking Vulkan/Metal correctness bridge remains; this is not a bridge
performance improvement or translation/depth reprojection.

Validation: full macOS RelWithDebInfo build passes without compiler warnings;
**80/80 CTests pass**, including the CPU calibration/rotation cases and the
production Metal shader reprojection test. Python calibration-transfer tests
pass **2/2**, and WSI capture/analysis tests pass **23/23**. The initial sandbox
run could not access Metal, IPC sockets and remote layers; repeating with host
access passes the full suite, including the GPU test. Logs are in the main
checkout's `build/review/wsi-passthrough-build.log` and
`build/review/wsi-passthrough-ctest-host.log`. Linux was not rerun for these
macOS-only source changes. This validates software regressions, not WSI camera
alignment on the headset.

### First calibrated camera-only run (16 October 2026)

At the user's readiness confirmation, the camera-only service-composited check
ran for 60 seconds at 120 Hz on `ec4c02591` plus the working-tree parity change.
The manifest preserves the exact source diff (SHA-256
`ea71ca478ec265a5a918cb969444670328e0939583fb5a5178431603f8322d97`)
and binary hashes in `build/review/wsi-calibrated-camera-only-20261016-1/`.
Logs confirm the saved calibration loaded, rotational correction became active,
and the readback checked 6,104 samples (5,730 nonblack) with no alpha or grey
errors. The application exited successfully, and display/service-registration
restoration both returned zero. User visual feedback is pending; this is not
yet a camera alignment acceptance or a default-switch gate pass. Blended and
hosted checks remain next.

### Camera-only visual acceptance (16 October 2026)

The user missed the first viewing window, so the same service-composited
120 Hz camera-only check was repeated for two minutes. During that repeat the
user reported “All looks fine.” This confirms subjective camera geometry,
stereo and rotational stability for this configuration. Logs again confirm the
saved calibration loaded and rotation was active; the readback sampled 6,104
pixels (5,753 nonblack) without alpha or grey errors. Evidence and source/binary
hashes are in `build/review/wsi-calibrated-camera-only-20261016-2/`.
Blended scene alignment and hosted coverage remain separate checks.

### User-operated visual check

Use the rebuilt `build/review/wsi-integration-build` and existing display helper.
Close XR applications first. From the main Monado-2 checkout, run:

```sh
python3 build/review/wsi-integration/scripts/macos/wsi-experiment/run.py \
  --build build/review/wsi-integration-build \
  --display-helper build/review/wsi-display-mode \
  --output build/review/wsi-calibrated-camera-only \
  --conditions wsi-generic-calibrated --hz 90 120 --seconds 30 --repeats 1 \
  --loads light --passthrough only

python3 build/review/wsi-integration/scripts/macos/wsi-experiment/run.py \
  --build build/review/wsi-integration-build \
  --display-helper build/review/wsi-display-mode \
  --output build/review/wsi-calibrated-overlay \
  --conditions wsi-generic-calibrated --hz 90 120 --seconds 45 --repeats 1 \
  --loads baseline --passthrough overlay

python3 build/review/wsi-integration/scripts/macos/wsi-experiment/run.py \
  --build build/review/wsi-integration-build \
  --display-helper build/review/wsi-display-mode \
  --output build/review/wsi-calibrated-hosted-overlay \
  --conditions wsi-generic-calibrated --hz 90 120 --seconds 45 --repeats 1 \
  --loads baseline --passthrough overlay --hosted
```

The runner enables cameras/bridge, supplies the same settings to service and
client, and restores the display mode and previous service registration.
The saved local calibration should require no explicit path. Before judging
motion, confirm `Experimental calibrated passthrough` and `Passthrough rotation
active` in the composing process's log (`service.stderr.log`, or
`application.log` for hosted). Keep stationary geometry/stereo comfortable, then
check virtual details remain aligned with the room during gentle yaw, pitch
and roll. Check teardown/relaunch as well. Pixel verification alone cannot
establish calibrated alignment. Record the tested commit plus working-tree diff
(the runner saves both), rate, compositor mode and visual result here.
The installed runtime has not been replaced and no headset run is claimed.

## Integrated branch (15 October 2026)

The experiment below and Claude's generic refresh-request work are now
committed together on `claude/macos-wsi-integration`, based on
`macos-upstream-clean` at `f076c87ed`, in the Monado-2 worktree
`build/review/wsi-integration`. Native presentation remains the default.

Differences from the tested `experiment/macos-wsi-current-native` sources:

- **Refresh requests.** The main compositor now owns the hand-off
  (`comp_refresh_request.h`, used by `comp_compositor.c`): while a session runs
  the compositor thread applies the request in `predict_frame`, otherwise it
  applies on the caller's thread under the same lock, and ending a session
  applies any outstanding request. This is the behaviour the experiment's
  `comp_macos_refresh.h` queue and `comp_target::set_session_active` hook gave
  the two macOS targets, so both are removed. It also guards deferred target
  creation and destruction. The WSI target's request applies directly, and it
  still follows mode changes made outside Monado.
- **Pacer period.** `u_pacing_compositor` gains an `update_frame_period` entry.
  It replaces `u_pc_update_period`, which identified pacers by comparing
  function pointers, and the fork-only `u_pc_fake_set_frame_period`. The
  experiment's budget, offset and phase checks are kept in `tests_pacing`.
- **No clock bridge.** `os_monotonic_get_ns()` is `CLOCK_UPTIME_RAW` on this
  base, the same Mach domain as MoltenVK's GOOGLE timestamps; the headset runs
  logged offsets of -21 to +41 ns. The swapchain offset field, the WSI
  sleep-jump check and their tests are removed; `on_present_feedback` stays.
- **Mode settle.** Native and WSI share `comp_macos_display`, so the native
  target's duplicate rate and mode helpers are removed.

The hardware evidence in this document and in
[the headset fixes](macos-wsi-headset-fixes.md) was gathered on the experiment
sources, not on this branch. Before relying on it, repeat at least the refresh
cycle for native, GOOGLE WSI and CV WSI, a hosted WSI session and the passthrough
overlay check. From the Monado-2 checkout:

```sh
cmake -S build/review/wsi-integration -B build/review/wsi-integration-build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/review/wsi-integration-build --parallel 6
ctest --test-dir build/review/wsi-integration-build --output-on-failure
clang -fobjc-arc -framework AppKit -framework CoreGraphics build/review/wsi-integration/scripts/macos/wsi-experiment/display_mode.m -o build/review/wsi-display-mode
python3 build/review/wsi-integration/scripts/macos/wsi-experiment/run.py --build build/review/wsi-integration-build --display-helper build/review/wsi-display-mode --output build/review/wsi-integration-refresh --conditions native wsi-generic wsi-generic-cv --hz 90 --seconds 15 --repeats 1 --refresh-cycle
```

### Review fixes after `f46892ad4` (16 October 2026)

- WSI with `XRT_MACOS_WSI_PASSTHROUGH=1` and an explicit
  `XRT_COMPOSITOR_COMPUTE=0` keeps the graphics target running, but does not
  create the passthrough bridge. It warns at image creation and passthrough
  creation returns unsupported. This avoids advertising an overlay whose
  opaque application image hides the camera. The default compute passthrough
  path is unchanged.
- Refresh targets own the compositor period as well as their pacer period.
  The main compositor no longer replaces an unchanged period with `1/hz`:
  a successful same-rate request now preserves the measured 119.88 Hz period,
  and a mode change keeps the target's new measurement. The only target refresh
  callbacks on this branch are native macOS and WSI; both already update the
  period when a change takes effect.

Regression probes compile the production refresh dispatcher and bridge-selection
block with mocked hardware calls. All eight cases pass, and the unchanged
`f46892ad4` code reproduces both failures. The probe and output are saved in
`build/review/wsi-review-fixes-check-guards.py` and
`build/review/wsi-review-fixes-guards.log` in the main checkout. These are CPU
checks, not new headset or visual validation.

The full macOS RelWithDebInfo build passes without compiler warnings, all
**80/80 CTests** pass, and the capture/analysis Python checks pass **23/23**.
Build and test logs are in `build/review/wsi-review-fixes-macos-*.log`.
The Ubuntu 24.04 Linux CI dependency configuration also builds without compiler
warnings and passes **52/52 CTests** (311.53 seconds under x86-64 emulation).
Its configure, build and test logs are in `build/review/wsi-review-fixes-linux-*.log`.

### Headset validation of the branch (16 October 2026)

Claude ran the checks above on the headset at the user's request, using
`build/review/wsi-integration-build`, the light 24-instance application and Metal
API validation, with the lid open. The runner restored 4000x2040 at 90 Hz and the
installed service registration after every group.

| Check | Commit | Result |
| --- | --- | --- |
| Refresh cycle: native, GOOGLE WSI, CV WSI, calibrated CV WSI | `ffeda97f0` | All pass: 90 Hz before `xrBeginSession` in under 1 ms, 73 Hz rejected without a mode change, 90->120->90->120 verified |
| Hosted session: GOOGLE WSI and calibrated CV WSI, 90 Hz | `ffeda97f0` | Pass; visibility hidden->pending->visible->hidden |
| Camera-only passthrough, calibrated CV WSI, 120 Hz | `ffeda97f0` | Pass; 6,104 samples, no alpha or grey errors |
| Overlay passthrough, service, 90 Hz | `ffeda97f0` | **Fail**, see below |
| Overlay passthrough, service, 90 Hz, three repeats | `da79ed3d4` | All pass; 1-3 opaque application samples each, no mismatches |
| Overlay passthrough, hosted, 90 Hz | both | Pass |
| Native 90 Hz shutdown, three repeats | `da79ed3d4` | Pass; no GPU-fault lines |
| Passthrough requested without the WSI bridge | `da79ed3d4` | Pass; the application finds no capability and exits before a session, no service errors |

The overlay failure was in the verifier, not the composition. Its first overlay
frame had application layers but sampled no opaque application pixel, and the
check treated that as a failure; that readback and every other had no
mismatches, and the next frame verified. Codex's earlier pass sampled opaque
pixels on its first overlay frame. The verifier now retries such frames and only
fails after 120 frames without an opaque sample. Overlay coverage remains a few
samples per frame in this scene, a sanity check rather than visual validation.

Captures, `functional-summary.txt` and the `check_hw.py` checker that produced it
are in `build/review/wsi-integration-hw-20261016/`. These are functional checks,
not a performance comparison. Visual acceptance in the headset, lid-closed runs,
Game Mode, sleep and reconnect remain open. WSI camera calibration and rotation
are now implemented; their visual check is pending as described above.

The rest of this document is the experiment's record and is kept as written.

The active Monado-2 worktree is `build/review/wsi-source`, branch
`experiment/macos-wsi-current-native`, based on
`f076c87ed8715e00a66c58e6bec7ad43b5ad19b1`. The old worktree in the
`monado-upstreaming` repository is preserved. The main checkout's four dirty
documentation files are untouched. Source transfer used the self-contained
patch with SHA-256 `5ac511c3b4f84416557b66f7f54cab0a448130f53234f3532c4b31a024ee173f`.

## Claude review dispositions

- Idle native refresh requests apply immediately. Active native requests now
  apply at prediction boundaries: hardware testing showed the direct API-thread
  drain could wait while the compositor kept feeding the presenter. The existing
  `u_pc_fake_set_frame_period` preserves frame IDs, phase, presentation offset
  and compositor budget. The older base actually recreated the fake pacer in
  this request path; using the existing setter also removes that reset.
- Native and generic WSI targets queue active-session requests at prediction
  boundaries. An optional target session callback serializes session transitions
  with idle API requests. Before begin and after end, requests apply immediately
  under the queue lock. Stopping a session processes any queued request before
  its rendering loop sleeps. A completed request retains success even when
  its timed wait expired while the apply function held the mutex. Outstanding
  waiters prevent a second caller overwriting the first result.
- The duplicate fake-period update now delegates to the existing setter rather
  than resetting its budget or phase. The GOOGLE pacer still updates its own
  period without restarting frame IDs, retaining its learned budget, offset
  and caller configuration (with the budget clamped to the new maximum).
- The native-wrapper WSI target, `XRT_MACOS_WSI`,
  `XRT_MACOS_WSI_MAX_PENDING`, and their credit loop are removed. Native GPU
  attribution changes are removed too. Historical analysis still understands
  those old capture formats, but the runner rejects the removed conditions.
- Passthrough export/Metal/pixel-check failures after transition all attempt to
  restore PRESENT_SRC and re-signal render completion. Recoverable failures
  present the acquired image before returning the composition error. Missing
  passthrough support is rejected at creation with an explicit unsupported
  result. A defensive presentation fallback warns once without renderer spam. If Vulkan synchronization cannot be restored, the target stops acquiring
  and releases the outstanding image through swapchain destruction at teardown.
- Physical mode changes now wait for an active display with matching geometry
  and the requested rate before adopting timing; inactive-display fallback
  values such as 60 Hz cannot become the WSI pacer period.
- Camera startup is a not-ready state and preserves normal image presentation.
  Camera-only and actual overlay pixel verification run separately.
- Stopping a GPU waiter cancels its pending obligation without publishing GPU
  completion or reporting a false device fault; real live-client timeouts keep
  their loss and quarantine behavior.
- Refresh request errors propagate on macOS only; Linux and Android keep
  their prior request behavior. Shared clock conversion defaults to zero offset
  on other targets.
- Test CMake changes are just the two test registrations; formatting churn and
  stray pacing-header blank lines are removed.

## Current controls and limits

Native remains the default with its CADisplayLink pacing, calibrated/rotational
passthrough and normal shared-event handoff. `XRT_MACOS_WSI_GENERIC=1` selects
WSI. `_GENERIC_CV=1` adds CV phase and `_GENERIC_CALIBRATE=1` calibrates its
presentation offset. The opt-in `_PASSTHROUGH=1` / `_PASSTHROUGH_VERIFY=1`
bridge remains a blocking correctness experiment, now with native calibration
and rotational correction ported for the visual check above.

Sleep clock discontinuities and display loss require target/service restart.
Automatic recovery is not implemented. The current-base hardware gates include
refresh requests before begin, 90→120→90→120, invalid-rate rejection, repeated
sessions, hosted visibility, verified overlay/only passthrough, and sleep/reconnect.
The [hardware fixes and reruns](macos-wsi-headset-fixes.md) now pass native and
all WSI refresh cycles, camera-only pixels and actual overlay pixels, including
hosted WSI. The original failed captures remain in the initial-check ledger.
Visual acceptance and the integration gaps above remain pending.

## Earlier review validation

Final fix validation is recorded in [the hardware-fix report](macos-wsi-headset-fixes.md).

- Fresh macOS RelWithDebInfo configure and full build passed without compiler
  warnings. Final host CTest run passed **79/79** (21.66 s).
- Python capture/analysis tests passed **23/23**. The display-mode helper builds,
  and runner help lists only the remaining conditions.
- Ubuntu 24.04, GCC 13.3, default CMake options and the Linux CI dependency recipe:
  full build passed without compiler warnings, and **51/51 CTests passed**
  (531.12 s). This used Docker x86_64 emulation on Apple Silicon; times are
  validation costs. The full suite was run before the last non-Apple request
  behavior restoration and strengthened timing assertions. A final incremental
  build and the two timing test executables validate those changes; unrelated
  solver/fusion cases are not repeated.
- The timeout race negative control passes with corrected code and fails when
  its old unconditional timeout handling is restored in a temporary header.
- clang-format 23.1.3 formatted new source and changed lines using the repository
  configuration (which already includes an Objective-C section). Test CMake has
  exactly two added lines. The complete portable patch passes strict whitespace
  and clean-index application checks at the exact base.
- Initial sandboxed macOS CTest failures were host-access restrictions affecting
  GPU, sockets and remote layers. Host-permission runs pass all tests. The final
  build uses a newly configured directory after an overlapping initial build
  damaged Ninja's log; the final clean build and subsequent incremental update
  logs contain no compiler warnings.

Evidence is in the main checkout's `build/review/wsi-review-*` logs. The reviewed
portable source patch and per-file hashes are
`build/review/wsi-migration/review-followup.patch` and
`review-followup-snapshot.json`. Nothing is committed, pushed or installed.

## Build and user-operated pilot

From the main Monado-2 directory:

```sh
cmake -S build/review/wsi-source -B build/review/wsi-current-native-fresh -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/review/wsi-current-native-fresh --parallel 6
ctest --test-dir build/review/wsi-current-native-fresh --output-on-failure
python3 -B -m unittest discover -s build/review/wsi-source/scripts/macos/wsi-experiment -p 'test_*.py'
clang -fobjc-arc -framework AppKit -framework CoreGraphics build/review/wsi-source/scripts/macos/wsi-experiment/display_mode.m -o build/review/wsi-display-mode
python3 build/review/wsi-source/scripts/macos/wsi-experiment/run.py --build build/review/wsi-current-native-fresh --display-helper build/review/wsi-display-mode --output build/review/wsi-refresh-current-base --conditions native wsi-generic wsi-generic-cv --hz 90 --seconds 15 --repeats 1 --refresh-cycle
```

The refresh pilot now explicitly requests 90 Hz before `xrBeginSession` and
checks its duration and physical mode. The runner uses an isolated service and
restores prior registration/display mode. Headset runs require user authorization;
the user explicitly authorized the initial automated checks recorded above. The installed runtime has not been replaced.
All old-base timing claims remain historical. New performance comparisons must
follow correctness and include distinct client frames as well as cadence/tails.


### Blended scene correction (16 October 2026)

The initial service blended check used `--loads light`, which caps drawing at
24 instances. The diagnostic adds its floor markers first, so the user correctly
reported seeing only floor boxes. Calibration and rotation were active, and
pixel checks passed (four sampled opaque application pixels, no mismatches),
but the full alignment scene was absent. The run was intentionally interrupted
with the runner's cleanup path, and restarted with `--loads baseline` to draw
the complete scene. Reduced-scene capture:
`build/review/wsi-calibrated-overlay-20261016-1/`; full-scene replacement:
`build/review/wsi-calibrated-overlay-full-20261016-1/`. Visual acceptance of the
blended scene remains pending.


### Full-scene blended motion report (16 October 2026)

With the complete scene restored at 120 Hz, the user reports: “Gentle head
movements are fine. More rapid ones and the objects slightly shimmer.” This
supports gentle-motion camera/virtual alignment, with a residual rapid-motion
artifact. It does not establish whether the shimmer is virtual-scene temporal
aliasing, camera timing/reprojection error, cadence, or a WSI-specific defect.
The service log shows rotation active without subsequent static fallback and
pixel verification passes (744 sampled opaque pixels, no mismatches).
A matched native full-scene passthrough comparison is the next discriminator;
hosted coverage and the default switch remain pending.

The full-scene WSI run was intentionally interrupted after the user's motion
report to move to the native control. Runner cleanup restored display and
service registration; its incomplete run record is not a completed performance
sample. The next user-viewed control is `--conditions native`, 120 Hz, baseline
full scene and blended passthrough, captured in
`build/review/wsi-native-passthrough-control-20261016-1/`.


### Native control is visually better (16 October 2026)

The user reports no comparable rapid-motion shimmer with the native presenter:
“No, that looks better with native.” Both runs use the same build, 120 Hz,
full scene, service composition, saved camera calibration and active rotational
correction. WSI additionally used its one-time pixel verifier. This is positive
native control evidence and holds the WSI default promotion pending a fix;
it does not identify the cause by itself.

The flushed WSI trace shows display intervals of 8.342 ms at median but
16.683 ms at p95/p99, and render GPU time median/p99 3.778/4.929 ms. Its
calibrated presentation offset has median 16.700 ms; display-target error is
about -0.011 ms at median and +8.342 ms at p99 after the first 240 feedback
entries. Native traces are still being collected for comparison. These are
sequential subjective runs, not a controlled latency benchmark.


### Camera queue priority parity prepared (16 October 2026)

Inspection found that WSI applies `XRT_MACOS_GPU_QUEUE_PRIORITY` to its
MoltenVK compositor queue but omitted the additional Metal camera bridge queue.
Native applies the setting to both. The bridge now uses the existing priority
helper for its camera queue as well, preserving the unset/default behavior and
existing validation of explicit values. This removes a configuration mismatch;
it is not yet a proven shimmer fix. Build and all three focused calibration,
Metal rotation and GPU-policy tests pass. The initial native visual feedback
preceded this small incremental build; do not treat its whole capture as a
quiet matched performance benchmark because the later build overlapped it.
The native run exited successfully and its runner restored registration/mode.
The next WSI check disables pixel readbacks, which previously ran only at startup,
and otherwise keeps full scene, 120 Hz and calibrated CV pacing unchanged.


### Priority parity does not improve shimmer (16 October 2026)

The user's repeat report is “No better.” The log confirms both compositor and
camera presenter queues changed foreground priority from 1 to requested 0,
with rotation active. Keep this as priority-setting parity, not a shimmer fix.
The initial full-scene WSI capture presents about 87.91 Hz with 35.38% of
intervals above 12.5 ms after 240 feedback entries, versus about 118.63 Hz and
0.81% for the native control. The native capture's later build overlap limits
strict performance claims, but this substantial difference motivates inspecting
the blocking bridge and compositor wake budget before changing camera geometry.
The fake pacer currently budgets 3 ms, while distortion alone takes about
3.8 ms median, before WSI's synchronous Vulkan/Metal camera bridge.

The next diagnostic changes only the existing
`U_PACING_COMP_MIN_TIME_MS` from 3 to 7 in the priority-corrected build,
keeping calibrated CV WSI, full scene, 120 Hz and blended passthrough. It is a
wake-budget trial, not a new default or calibration tweak. Capture:
`build/review/wsi-camera-budget7-overlay-20261016-1/`.


### Earlier wake gives inconsistent smoothness (16 October 2026)

For the 7 ms minimum-budget trial the user reports: “Weird. Sometimes it was
smooth, other times not, even with consistent head movement speed.” This is not
a successful fix and does not justify promoting the budget to a default.
The variation over time does not establish a direct head-speed threshold or a
camera calibration defect. No further subjective run is started until this
capture has been examined. Keep native as default; retain the unchanged saved
camera calibration.

The completed 7 ms trial measures 101.84 presents/s after 240 feedback entries,
with 17.50% of intervals longer than 12.5 ms. Ten-second windows range from
98.17 to 105.67 presents/s, versus 88.93 overall for the priority-only 3 ms
control. The filtered present offset spans 8.342–22.115 ms, median 12.963 ms;
pose/display target error p95/p99 is +6.701/+9.094 ms. This quantifies persistent
cadence/phase variation; it does not assign the user's smooth/rough intervals
to specific trace times. Both restoration statuses are zero.

Opt-in existing timing tracing now also records `wsi_passthrough` stage times:
camera ready/upload, Vulkan render-complete/layout wait, export/encode, Metal
completion, optional pixel verification and layout restoration, plus Metal GPU
duration. The extra trace is disabled when timing tracing is disabled and
writes through the existing buffered trace helper. A 20-second same-budget
functional capture is prepared to distinguish these costs, without requesting
another subjective headset judgment.


### Blocking bridge measured (16 October 2026)

The 20-second diagnostic capture at
`build/review/wsi-bridge-timing-budget7-20261016-1/` succeeds with no composition
errors, application exit zero and display/registration restoration zero.
After discarding 240 bridge samples, 1,751 samples give:

| Stage | Median (ms) | p99 (ms) |
| --- | ---: | ---: |
| Camera ready/upload and capture pose | 0.329 | 0.894 |
| Vulkan render-complete/layout CPU wait | 3.914 | 5.004 |
| Metal texture export and encode | 0.138 | 0.298 |
| Metal completion CPU wait | 1.376 | 2.299 |
| Restore Vulkan present layout CPU wait | 0.129 | 0.259 |
| Complete blocking bridge | 5.964 | 7.208 |
| Camera Metal GPU duration (within completion wait) | 1.088 | 1.717 |

Stage percentiles do not sum to the total percentile. Upload includes capture
pose lookup, not just byte transfer. These measurements identify a concrete
compositor-thread cost: WSI serially waits for distortion and the camera pass,
whereas native hands completed work to its asynchronous presentation worker.
Only about 0.13 ms median is in the final restoration wait, so simply deferring
that one wait will not remove the main bottleneck. The next implementation
needs GPU-ordered handoff and safe per-image camera/app-copy lifetime, rather
than more fixed timing offsets. Preserve all Vulkan/Metal ordering and image
reuse protections. This is a performance lead, not proof that every subjective
shimmer event is caused by a particular wait.

The 7 ms budget remains an experiment supplied only to the test runner; neither
runtime defaults nor installed runtime were changed. The current calibration
port is retained with its positive camera-only/gentle-motion results. WSI
promotion and hosted visual acceptance remain blocked on this motion issue.

The diagnostic build has no compiler warnings; all 80 macOS CTests and all
23 WSI capture/analysis tests pass after instrumentation. Test log:
`build/review/wsi-bridge-timing-ctest.log`. No headset test remains running.


### Vulkan throughput gain does not fix shimmer (16 October 2026)

The full-scene moving-head check of the Vulkan camera pass receives:
“Higher frame rate, but just higher frame rate shimmer!” It measures 118.18
presents/s with no runtime/validation errors and successful restoration, but
fails visual acceptance. Native stays default. The implementation, exact source
snapshot and timing limits are recorded in
[the Vulkan passthrough note](macos-wsi-vulkan-passthrough.md#moving-head-visual-check-fails-16-october-2026).

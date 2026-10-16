<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Current WSI experiment and review follow-up

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
Game Mode, sleep and reconnect, and WSI passthrough calibration and rotation
remain open.

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
bridge is still a blocking correctness experiment with static mapping; parity
with native calibration and rotational correction remains open.

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

<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# External FD sync guard and KosmicKrisp Alyx candidate — 21 October 2026

The six external FD capability flags now require the corresponding enabled
device extension as well as physical-device import/export support. This fixes a
generic Monado capability-selection bug exposed by KosmicKrisp; no driver-specific
exception is added. Both device creation and adoption populate the extension
flags before this check. Enabled/supported FD paths retain their behavior.

Claude identified/reproduced the crash and supplied commit c964fce70. The patch
is integrated with its attribution and DCO sign-off as 285916e on the isolated
branch `codex/vulkan-fd-sync-guard`, based on 90a6cdb64. The primary checkout stays
on its existing branch. The isolated client runtime is built at
`~/Code/alyx-native/build/monado-kk-fd-guard`; no installed runtime/service or
LaunchAgent is rebuilt, replaced or reconfigured.

## Validation

[Recorded identities, public driver censuses and numeric evidence](alyx-valve-lua-results/kk-fd-sync-guard/):

- Full RelWithDebInfo build passes. CTest passes 88/88.
- A new no-GPU test calls actual `vk_init_from_given` using fake Vulkan dispatch.
  It covers 128 combinations of enabled FD extensions, per-handle capabilities
  and timeline feature state: 1,408 assertions pass. FD export entry points stay
  NULL in the fixture, modelling the original failure condition. Disabled
  extensions never perform corresponding capability queries. Removing the six
  guards causes 288 assertion failures; restoring/rebuilding passes again.
- Real Klepton strict device creation with KosmicKrisp and both timeline and
  sample-location profiles passes. Unsupported core-feature creation returns -8,
  with the caller request intact. No game is launched for this check.
- With Metal validation enabled, the xrizer submission probe completes 180 frames,
  zero errors on each of KosmicKrisp and MoltenVK. Both use the guarded runtime,
  existing xrizer GPU-timing library and actual Monado Metal shared-event handoff
  (no queue-idle handoff fallback). The isolated service uses MoltenVK; only the
  client ICD changes. Each temporary service is removed after the run.
- An additional KosmicKrisp capture records 178 application GPU-clock intervals:
  timestampPeriod 41.666668 ns, 64 valid bits. This checks timestamp operation,
  not the game's timing feedback consumption or a matched performance comparison.
- Launcher dry-runs select the intended ICD/runtime and retain MoltenVK as the
  original default. The installed runtime SHA256 still matches the successful
  gameplay build. Actual Alyx and visual stereo correctness remain user tests.

The first extended timeline probe crashed during teardown, separately from the
FD issue: the old probe enabled Vulkan 1.2 core timeline features without the
KHR extension; Monado currently loads KHR-named timeline functions. The recorded
[probe-only patch](../experiments/alyx-vulkan/fd-sync-guard/probe-timeline-extension.patch)
explicitly enables the supported KHR extension, matching the existing Alyx host
and temporary-device profile. Subsequent KK and MoltenVK probes complete cleanly.
No broad Monado function-loader change is included; support for core-only callers
remains a separate follow-up. The failure and private stack remain at
`~/Code/alyx-native/runs/kk-guard-timeline-client` (PC NULL from
`client_vk_compositor_destroy`, semaphore counter query).

KosmicKrisp worktree is c8e17c9 on `claude/kk-metal-objects`: prototype
VK_EXT_metal_objects plus imported-texture usage correction. The embedded driver
version string still reports git-0c9a1f072e; the tested dylib hash is recorded to
avoid confusing that cached version with the checked-out revision. ShaderWrite
usage warnings appear for imported non-storage textures; these are the prototype's
known diagnostic warnings, not proof of missing texture import. Full extension
conformance, actual game shaders, depth behavior and sustained performance remain
unverified. No FDM extension is advertised by this work.

## User test command

With native Steam running/signed in and the usual simulated-HMD setup:

```sh
~/Code/alyx-native/run-alyx-kosmickrisp \
  --output ~/Code/alyx-native/runs/kk-alyx-2 --timeout 300 \
  -- +vr_fidelity_level_auto 0 +vr_fidelity_level 0
```

Load the same saved scene, wait about 30 seconds, move/turn briefly and quit with
Ctrl+C. The output path must be new. The first run may compile shaders; do not
compare it as a warmed performance benchmark. No eye-tracked FDM is enabled.

The helper wraps the successful GPU-timing launcher with explicit selections:

```sh
~/Code/alyx-native/run-alyx-gpu-timing \
  --driver kosmickrisp \
  --icd ~/Code/alyx-native/build/kk-metal-objects/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json \
  --runtime-json ~/Code/alyx-native/build/monado-kk-fd-guard/openxr_monado-dev.json \
  --extended-dynamic-state3 disabled --sample-locations-compat enabled \
  --api-trace --output ~/Code/alyx-native/runs/kk-alyx-2 --timeout 300 \
  -- +vr_fidelity_level_auto 0 +vr_fidelity_level 0
```

KosmicKrisp, like MoltenVK, does not expose EDS3 color-write-mask support, so the
existing engine fallback is explicit. Only genuinely supported sample-location,
EDS1 and timeline features are enabled. CPU ABI compatibility is unchanged.
The helper's source is kept under `experiments/alyx-vulkan/fd-sync-guard`.
Driver, loader and client-runtime hashes and the full extension census are saved
by the launchers. The installed service stays on its working driver. The original
`run-alyx` and `run-alyx-gpu-timing` still default to MoltenVK/the installed runtime.

## IPC version correction — 21 October

The first user Alyx attempt failed before creating an OpenXR instance: the
isolated client identified as v25.1.0-2380-g1823c339d while the installed service
identified as v25.1.0-2379-g90a6cdb64. Monado checks exact Git version strings,
not just wire-protocol layout. The earlier instruction selecting only the new
client runtime was incomplete. No game compatibility result can be inferred.

Both isolated service and runtime were rebuilt together at 3f2a9f5c4. The opt-in
launcher now invokes `~/Code/alyx-native/with-matching-monado.py`, which starts
that build's service as a uniquely named temporary launchd job, selects a private
IPC directory/XPC endpoint for both processes, and keeps the service on MoltenVK.
Desktop simulated-HMD mode is explicit in both the client and service. No global
launchd environment, installed job, installed runtime or game file is modified.
`IPC_IGNORE_VERSION` is removed from the test environment: the check stays on.

Service output is saved beside the game run in `kk-alyx-2-monado/`; temporary
plists and socket directories are removed on exit. Reusing either output
directory is rejected. A real paired offscreen probe completes 180 frames, zero
errors with the strict version check. Separate failing-client (exit 7) and
Ctrl+C (exit 130) checks verify that the temporary job is removed. A desktop
probe is also recorded in the private permanent workspace. Game rendering is
still a user checkpoint.

The direct `run-alyx-gpu-timing --runtime-json ...` expansion above describes
the client command only. Use `run-alyx-kosmickrisp` to start the matching service
as well. Rebuild both together with:

```sh
cmake -S ~/Code/monado-2/build/review/kk-fd-guard \
  -B ~/Code/alyx-native/build/monado-kk-fd-guard -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build ~/Code/alyx-native/build/monado-kk-fd-guard --parallel 6
```

## First paired Alyx result: main scene missing — 21 October

The user reports the intro did not play and menu/hands appeared, but the main
rendered environment was absent: grey 3D background with Alyx logo. This is
explicitly not the xrizer loading-stage override. The run ends with Ctrl+C
(exit 130) after 49.13 seconds. Full scene compatibility is not validated.

The unsupported override warning also occurs with MoltenVK, but does not explain
the missing application environment. The compared runs all have 50 error-texture
and 100 command-buffer-purge warnings. No graphics-pipeline failure is logged;
this does not establish correct pipelines/shaders. Intro playback has no decisive
error in this capture and remains unexplained.

The concrete enabled-extension difference in the engine log is
VK_EXT_custom_resolve. Only KK logs programmable resolves. The actual ARM64
renderer contains the built-in option `-vulkan_disable_ext_custom_resolve`.
Test that fallback without modifying the game or driver, using the same profile:

```sh
~/Code/alyx-native/run-alyx-kosmickrisp \
  --output ~/Code/alyx-native/runs/kk-alyx-3 --timeout 300 \
  -- -vulkan_disable_ext_custom_resolve +vr_fidelity_level_auto 0 +vr_fidelity_level 0
```

A dry-run verifies that option is forwarded and KK/timeline/GPU feedback stay
selected (explicit 24576 MiB was used for the sandboxed dry-run only). This is a
hypothesis, not a confirmed cause. Check the intro/menu environment first, then
load the usual save if they return. The next log must confirm programmable
resolves are no longer selected before attributing a visual difference to the
option. If the environment remains missing, instrument the actual scene render
passes and pipelines rather than implement the unrelated stage-override renderer.

[Sanitized run identity and comparisons](alyx-valve-lua-results/kk-fd-sync-guard/kk-alyx-2-menu.json)
retain the evidence without private game logs. The installed runtime and default
experimental profile are unchanged.

## Custom-resolve fallback succeeds; hitching remains — 21 October

User: "That worked. Subjectively performance worse than MVK." The next run
forwards the game disable option, no longer logs the programmable-resolve path,
creates its device successfully and ends with Ctrl+C (130) after 147.78 seconds.
No Klepton fault, xrizer panic, device loss or OpenXR instance-loss marker occurs.
Logs show restoration of the same a1_intro_world_2 save and loading completion.
The option is now the default only in the opt-in KK helper: this is a confirmed
working game fallback, not a fix to KK's VK_EXT_custom_resolve implementation.
The driver continues to report its capabilities unchanged.

The API 90–115-second window is after loading in both the KK and prior
fixed-fidelity-request MoltenVK captures. Frame IDs join the GPU samples to that
API window; absolute CSV timestamps are not assumed to share an epoch. A logged
completed GPU frame after the final loading-end maps to API second 57.05 for KK
and 41.28 for the older MVK run. No conversion of native logger wall times into
launcher Unix times is needed.

| Capture measure (ms) | KK median | KK p95 | KK p99 | Earlier MVK median | Earlier MVK p95 | Earlier MVK p99 |
|---|---:|---:|---:|---:|---:|---:|
| Application GPU-clock interval | 13.495 | 21.101 | 107.654 | 14.031 | 14.977 | 15.461 |
| Pose-call start interval | 17.093 | 53.360 | 214.322 | 16.819 | 20.124 | 21.808 |
| xrEndFrame wall duration | 0.110 | 0.266 | 2.284 | 0.148 | 0.257 | 0.485 |

KK has 929 GPU records in the selected window versus 1495 in the historical
MoltenVK capture. 53 logged actual game timing returns match the KK CSV to
0.001ms rounding, with no mismatches. Feedback is being consumed, not just
collected. The larger tails support the reported hitching. Similar median GPU
intervals do not establish equal GPU throughput: these intervals include waits/
idle gaps and the whole application queue boundary, not GPU busy utilization.
Scene motion, shader compilation/caches, actual settled fidelity, thermal state
and service configuration are not controlled between these historical runs.
No matched performance ratio or cause of the stalls follows from this table.

A dedicated `~/Code/alyx-native/run-alyx-moltenvk-control` now uses the same
paired service/runtime, timing-enabled xrizer, host and fallback options, changing
only the client ICD. Default KK/MVK dry-runs pass, including the KK helper's
engine-argument separator handling. The original successful MoltenVK launcher
is unchanged. For a current control, repeat the same save, wait and movement:

```sh
~/Code/alyx-native/run-alyx-moltenvk-control \
  --output ~/Code/alyx-native/runs/mvk-paired-control-1 --timeout 300 \
  -- +vr_fidelity_level_auto 0 +vr_fidelity_level 0
```

[Numeric evidence and limitations](alyx-valve-lua-results/kk-fd-sync-guard/kk-alyx-3-compatibility-and-timing.json)
are committed; private game logs remain in the permanent workspace. If the
paired MVK capture is smooth, instrument KK pipeline compilation and queue
submission around the outlier frames. Do not optimize or benchmark FDM before
establishing this non-FDM baseline.

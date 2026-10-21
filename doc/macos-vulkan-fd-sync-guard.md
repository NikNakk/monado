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

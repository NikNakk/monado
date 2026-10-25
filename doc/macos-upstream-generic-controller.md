<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Generic controller comparison and upstream merge — 2026-10-21

Review branch: `codex/upstream-generic-controller`, based on
`claude/wsi-fragment-output` at `90a6cdb64`. This is a review-branch merge;
`macos-upstream-clean` has not moved. The pre-existing untracked
`tools/macos-present-compare/` is outside this change.

GitLab `main` was fetched at `9fd3968ba`. The initial merge `7b1c728e7`
brought in 62 commits through `ffe0a758c`. The follow-up merges the remaining
eight I/O-cache commits with the compatibility adaptation below, bringing the
review branch up to that upstream tip. The original branch and its ten local
commits are preserved. Nothing is pushed or installed.

## Recommendation and implementation

Use upstream's generic controller implementation. The interaction-profile
JSON and generic input/output enums already match upstream: the material
comparison is the binding resolver and device mappings, rather than different
standard paths. Upstream's action-system refactor (`a39b9983a`, MR 2873),
generic rebindings (`dbec8b09f`, MR 2979) and subsequent driver-table conversion
(`2f0bbfc71`, MR 3049) replace our common hard-coded Touch/Index arrays and
Sense-specific generic arrays with generated, inspectable path rebindings.
The old interaction-profile-array implementation is removed.

| Area | Fork before merge | Upstream at merge boundary | Decision |
| --- | --- | --- | --- |
| Device coverage | Sense, Index and Touch-family generic mappings | Also ContactGlove/Magnetra, FlipVR, Hydra, Oppo, Pico, Vive Cosmos/Focus 3 and HP mixed-reality mappings | Adopt upstream. Hydra explicitly leaves unsupported squeeze/haptics unbound. |
| Representation | C arrays in OpenXR and the Sense driver | Shared JSON helpers, generated rebindings and a runtime registry | Adopt upstream; remove duplicate Sense generic tables. |
| Sense controls | Square/Cross primary, Triangle/Circle secondary, squeeze click converted to float, trigger, stick, poses and haptics | Equivalent generated Sense mapping | Adopt upstream; no evidence that ours interprets these controls better. |
| Specific compatibility selection | Suggested Touch/Index precedes generic, even with simple first in driver order | Native profile wins, then generated rebinding order; generic group 20 precedes Touch group 50 | Retain our narrower preference for explicitly advertised Touch/Index compatibility profiles in the new resolver. |
| Generic grip surface | Palm pose when available, otherwise approximate grip pose | Identity mapping to target grip-surface path, usually backed by generic palm; absent input stays unbound | Retain our fallback in the new input resolver, only for generic grip-surface suggestions. It is a continuity benefit, not a more accurate palm calibration. |
| Tests | Dedicated generic fallback-selection tests | Registry tests plus binding inspector and broader action-transform tests | Port our regressions to the new resolver, extend to actual Sense input/output resolution. |
| Extension exposure | Build-time opt-in | Default enabled in `eab5ff028` | Preserve this fork's opt-in default pending hardware validation. |

The new `binding_inspector` target makes generated profiles and
rebindings reviewable. Upstream also snapshots suggested profiles at action-set
attachment, separates profile and binding resolution, and applies action-set
priority to bound-source enumeration (`768f8d6af`). These are reasons to adopt
the common implementation rather than maintain our older action system.

Our two small resolver additions are independently regression-tested. Native
profiles still take precedence. The compatibility preference only considers
Touch/Index entries actually advertised by the driver, so Sense-to-Index stays
opt-in. Generic still precedes simple. Grip-surface resolution prefers the
actual target input; fallback does not add an EXT palm pose to the driver or
change non-generic profiles.

## Other merged work

The merge includes OpenXR headers 1.1.65; swapchain conversion/cast helper
refactoring and `XR_EXT_enumerate_swapchain_formats2`; stationary reference-space
enums/switches; configurable shader-specialization prefixes; frame-server
robustness and C++ wrappers; clock-extender/clock-reset corrections; generic
SLAM hand-mask arrays; Linux HID/serial EINTR handling; WMR/HDK descriptor fixes;
Rift clock cleanup; Vive Pro 2 status/input work; and OpenVR historical-interface
stubs, array-index submission and defensive checks.
The new format-enumeration extension stays off by default on macOS until
validated, preserving upstream's enabled default on other platforms.

Upstream IPC shutdown (`10c57897e`) is a newer version of this fork's submitted
fix. Adopt its locked running-state reads, fixed join-slot snapshot, internal
Windows cancellation and expanded tests while preserving macOS teardown hooks,
peer checks and connection failure handling.

For Sense, adopt corrected calibration-block layout, factory gyro speed reference
and fallback handling (`5f75f36bb`). Give each controller its own LED array by
copying into the existing embedded `corrected_leds` storage, preserving measured
corrections and avoiding mutation of shared static model memory (the purpose
of `5bca1effb`). No extra allocation/free is necessary here.

### Deliberate Sense coordinate adaptation

Do not mechanically apply `3ce42b71e` (IMU-angle sign) and `8152e68bf`
(model/IMU pose conversion) to the fork's calibrated LED-space tracker. Preserve
the existing angle, LED-space model and pose correction for this merge.
Upstream now transforms its LED model into IMU space and converts at output;
our optical histories, measured LED corrections, joint solver and EKF already
share LED-model coordinates and use `pssense_get_corrected_imu_pose`.
Mixing the two conventions would change more than the final grip offset.
These commits are in merge ancestry, but their coordinate changes are
intentionally adapted out, not claimed as adopted or hardware-validated.
A complete coordinate migration needs a separate replay/calibration comparison.
See [the existing integration decisions](macos-pssense-6dof-integration.md#compatibility-decisions).

### I/O-cache follow-up

`2bb37b006` through `9fd3968ba` is now integrated. OpenXR actions, pose-space
creation and haptics use upstream's per-session `xrt_io_cache`, including its
IPC client/server transport. Generated rebindings remain the primary path.
The earlier reason for deferral was that upstream's final commit removed the
legacy driver-table fallback while several mappings still had no generated
replacement.

The adaptation snapshots the remaining compatibility mappings in `get_info`:
ordered profile names and bounded, pointer-free input/output pairs. The base
cache copies the service-selected tables into that snapshot. The client resolves
aliases against native cached inputs and outputs; it never reads a client-side
environment variable to decide which service mappings are enabled.
Physical input names, value types, timestamps and activity flags stay intact.
Resolved poses/haptics use native device IDs and native names through upstream's
cache methods. The live `read` arrays and their sizes are unchanged: compatibility
metadata is transferred at layout query/recreation, not on every action sync.

This intentionally keeps driver tables as the source of truth for mappings
without a generated replacement; it does not create invented public interaction
profiles or duplicate these tables in JSON. A future declarative conversion can
retire each remaining table independently. OpenXR no longer dereferences driver
binding tables or devices to resolve these fallback bindings.

| Audited mapping | Why generated rebindings alone are insufficient | Preservation |
| --- | --- | --- |
| Sense to Index | Optional fork mapping is absent from upstream JSON | Only profiles present in the service's controller table enter the snapshot; `PSSENSE_INDEX_PROFILE` remains off by default. |
| Hand-controller emulation to simple | No native target interaction profile | Snapshot aliases preserve pinch input types and native grip/aim names. |
| Vive Pro 2 to Vive Pro | Upstream conversion omitted this device name | Snapshot preserves the existing identity button mappings. |
| PS VR2 eye gaze | No corresponding generated rebinding | Snapshot preserves the gaze profile only when the driver supplies it. Existing lazy eye-feature activation remains in action-space handling. |
| Other driver fallbacks | Some already have generated replacements; others are device-specific | Generated mappings win as before; the snapshot carries the complete ordered fallback set so remaining profiles are not silently lost. |

There are explicit limits of 128 compatibility profiles, 1024 input pairs and
128 output pairs across a cache. Oversized driver tables fail the layout query
with `XRT_ERROR_INVALID_ARGUMENT`; they are never silently truncated. Duplicate
input-pair fallbacks still try the first native input actually present. The
snapshot contains no pointers, and arm64/x86-64 use the same protocol fields.
Service and client must be rebuilt together for the changed IPC layout.

Our generic grip-surface fallback and specific Touch/Index preference now use
cache metadata. Our optical solver, EKF, coordinate convention and USB defaults
are unchanged by these eight commits and their compatibility adaptation.

## Initial merge validation

Both configured development runtimes build: the full arm64 tree and the
x86-64 `openxr_monado` target. Generic resolver regressions pass **622 assertions
in 3 test cases**; the generated binding inspector successfully dumps the generic
profile and rebindings. The first sandboxed CTest run was blocked in 20
shared-memory/socket/Metal/remote-layer suites; all 20 pass with host access.
The final full-suite host run passes **90/90 tests**, with no skips, in 43.83 s.

The native build emits existing pedantic warnings from the external Perfetto
SDK's variadic macros. No compiler warnings were observed outside that SDK.
`git diff --check` passes. Linux validation could not run because the local
Docker daemon is stopped. Linux CI and user-operated headset regression remain
required before integration/publication. In particular, exercise generic controls, grip/aim,
squeeze and haptics, optical 6DoF, opt-in Index compatibility, and normal
OpenXR action binding in the existing applications. No hardware run was made
by the agent.

## I/O-cache follow-up validation

The tested source merge is `302e9beaa`; both runtime builds report
`v25.1.0-2451-g302e9beaa`. The full native arm64 build and x86-64 OpenXR runtime
build succeed. A compiled C layout probe produces identical sizes and offsets
on arm64 and x86-64 (under Rosetta), including the 21,304-byte cache-info
structure and its profile/input/output metadata arrays.
All **91 native CTest suites pass**, with no skips, in 48.33 s. Generic and
compatibility resolver regressions pass **849 assertions in 5 test cases**.
The new IPC fixture, including actual OpenXR action synchronization and priority
overrides, passes in the full native run. The Ubuntu 24.04 default CI
configuration builds with **no compiler warnings** and passes **57/57 CTest
suites**, with no skips, in 524.90 s under Docker x86-64 emulation on Apple
Silicon. This includes the solver, fusion and IPC-backed OpenXR regressions.
The Linux build uses no feature overrides and an empty CMake build type,
matching the CI recipe. The native and Linux logs are saved locally at
`/tmp/monado-io-cache-native-ctest.log` and
`/tmp/monado-io-cache-linux-ci.log`.
The new `tests_io_cache_ipc` fixture uses the real generated IPC dispatcher,
handlers and client proxy with synthetic devices, without starting the service
or claiming hardware. It covers metadata, native samples, stale generations,
invalid-count recovery, pose spaces and haptics. Its OpenXR fixture resolves
service-provided compatibility bindings against that IPC-backed cache, then
runs action synchronization and bound-source enumeration with competing action
sets and temporary priority overrides. Resolver tests also cover hand-controller
emulation, Vive Pro 2, gaze, and both hands' optional Index mappings.

## User-operated headset gate

Do not move `macos-upstream-clean` or publish this branch until the user has
checked the matching service/native client and Wine client. Hardware runs are
user-operated under `AGENTS.md`. The automated fixtures do not establish optical
tracking quality, controller feel, gaze accuracy or Game Mode behavior.

Prepare the existing isolated Sense launch procedure with the reviewed build:

```sh
python3 scripts/macos/prepare-pssense-6dof-runtime.py \
  "$HOME/Code/psvr2-datasets/calibration/20260926-charuco-mode4-combined-head.json" \
  --build build/arm64
```

The isolated LaunchAgent has been prepared at
`build/arm64/pssense-6dof.plist`, with the native presenter and Index compatibility
off. Reproduction commands are saved in `/tmp/monado-io-cache-headset-instructions.txt`.
Preparation writes the LaunchAgent and prints launch/unload instructions;
it does not start hardware.
Use the printed commands after stopping any existing service that owns the USB
interfaces. Keep the current native presenter, optical calibration and solver
settings. Test in this order and record the exact commit/settings/results:

1. With Index compatibility off, run the printed `--generic-controller` trial.
   Check each hand's buttons, stick, trigger, squeeze, grip/aim rays and haptics.
   Also check grip-surface behavior in a client that uses it.
2. Run Open Brush or the previously validated application with optical 6DoF.
   Check normal movement, occlusion/reacquisition and reconnect without changing
   the solver or calibration. Repeat the normal client-hosted/Game Mode route.
3. Set `PSSENSE_INDEX_PROFILE=1` in the **service** environment before preparing
   the isolated launch, restart that service and test an Index-bound client.
   Confirm grip/aim, analog squeeze and haptics; remove the opt-in afterwards.
4. Check gaze with `psvr2-openxr-test --gaze`, then close/relaunch a normal
   application. Verify the native and Wine clients can each create a session,
   bind controls and stop cleanly against the matching service.

After user confirmation, check the current local and remote integration tips,
then integrate this branch without overwriting intervening work. Publication and
installed-runtime replacement remain separate from this local review merge.

<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Upstream merge review and integration — 2026-11-02

The subsequent [formatting and source-license cleanup](macos-upstream-contribution.md#formatting-and-metadata-cleanup-2026-11-02)
resolves the source contribution-check failures described below. It preserves
the archived evidence and external patches under an explicitly documented
source-license scope. The raw whole-repository REUSE archival audit remains
separate; passing source CI does not claim that every archive is licensed.

## Integration implementation

The user authorised the merge with preservation of the proven Sense tracker.
The integration merges upstream `db1903bd4` into `macos-upstream-clean`, from
integration parent `cf1c9e99b` (the review record on top of `ae2b6d059`).
The exploration below is retained as the initial review, not the final status.

### Tracking and compatibility decisions

The production constellation sources, public tracking API, Sense and PS VR2
drivers, Rift and Rift-sensor drivers, and PS VR2 Sense builder are unchanged
from the integration parent. Joint solving, EKF, LED-space transforms, camera
and illumination policy, reacquisition and selected settings are preserved.
There is no second copy of the tracker and no new backend-selection toggle.

The coupled upstream tracker migration is explicitly deferred for this sync,
including its Rift changes. This preserves existing Linux/Rift behaviour;
it does **not** claim to integrate the new upstream Rift fusion backend.
These upstream commits enter ancestry but their changes are deferred:

| Commits | Deferred change |
| --- | --- |
| `d823f1b9f` | Pose-metrics return-value/API changes, coupled to our separate association implementation. |
| `2458cc046`, `a90761e99`, `85e7d233e` | Tracker prior ordering, device search order and pose-rejection changes. |
| `54eebd9a4`, `6e8efeba5`, `c7f2c1c90`, `59be345db` | Rig calibration, online fusion, IMU changes, shared Ceres context and tracker-owned pose queries. |
| `53ac594b1`, `d9178085a`, `5b1118e95`, `7a97f712e`, `9b73f2546`, `f9dd56627` | Related sensor-pose, sample/sequence, blob-coordinate, Rift calibration/capability and comment changes. |
| `9884e32a8`, `d5893d16d`, `b1174ee76` | Follow-ups to the deferred fusion implementation. |

Future upstream syncs must consult this list: Git ancestry alone will treat
these commits as merged, although subsequent patches that depend on their
new APIs still require adaptation or explicit deferral here.

Legacy driver compatibility tables and their IPC metadata remain available.
Upstream `129ed9cf7` (removal of these tables) and its QWERTY follow-up
`647eb1b5c` are deliberately deferred, preserving optional Sense-to-Index,
hand-controller emulation and the other recorded mappings. Upstream's generated
rebinding improvements are integrated, including its PS VR2 gaze target and
subaction-path filtering. Cache-based compatibility selection now also takes
the subaction path, so head and gaze profiles cannot claim each other's slot.

### Other merge resolutions

- Adopt upstream shutdown cancellation, handler lock changes, haptic bounds,
  I/O-block handling and IPC fixes. Keep macOS input-lock/share initialisation,
  native ownership, hosting and GPU-lifetime protections.
  Adapt the new cache PCM exchange to hold this fork's wire-send mutex through
  the allocation handshake and payload, releasing it on every error and before
  waiting for the final reply. Cache transport tests cover capped PCM contents,
  blocked-output consumption and the next command's framing.
- Adopt upstream's target-type-aware manifest filenames. A prefix install to
  a temporary directory generates runtime and libmonado paths ending in
  `.dylib`, both resolving to installed files.
- Adopt unique ordinary POSIX shared-memory names and retain the private and
  read-only APIs. Failed mappings still return a null output.
- Adopt upstream's Unix byte transport and interrupted-operation handling.
  Retain macOS graphics-handle transport and completion of fragmented fd
  payloads, carrying the tail without sending SCM_RIGHTS again. Linux keeps
  upstream's exact-size fd-message policy.
- Combine image-tiling control with the platform-independent image-create
  chain and Metal allocation path. Keep the private three-image request at
  bit 3; assign tiling preference bit 4 to avoid changing the existing external
  compatibility value. Upstream uses bit 3 in its own ABI; builds of this fork
  consistently use the fork's enum. The preference is capability-gated and
  makes no demonstrated macOS performance claim.
- Keep the new view-configuration-change extension off by default on macOS,
  with upstream's enabled default elsewhere. Existing macOS feature choices
  and tracking environment preferences are unchanged.

The added regression coverage checks shared-memory isolation, head/gaze
subaction selection, macOS fragmented descriptor payloads, and real
space-location/PCM handlers blocked after acknowledging allocation. The latter
exercise the shutdown helper against the actual handlers, including repeated
cleanup. They do not claim coverage of every full-service deadlock.

### Linux test-only repairs

The initial Linux suite exposed two inherited fixture problems. Neither repair
changes production tracking or weakens the FD extension-negotiation assertions:

- The Vulkan FD-sync mock omitted `vkGetPhysicalDeviceFormatProperties`, which
  is used during external-image probing on Linux. Add that mock entry point;
  its intentionally null FD export entry points remain null.
- The sparse prior-reacquisition test asserted <40 mm error on every accepted
  noisy single-camera observation with three or four real LEDs and clutter.
  Linux seed 53/trial 40 returns 43.696 mm, with four matches and 0.848 px RMS.
  A diagnostic refinement started at truth also fails, reaching 60.687 mm
  error with four matches and 0.361 px RMS: this is not an independently
  established unique-pose accuracy guarantee. The 80 trials accept 78 poses,
  with 7.588 mm aggregate position RMS and two >20 mm errors.
  Replace the per-observation 40 mm claim with **<10 mm aggregate RMS**,
  retaining at least 60% acceptance, at most 10% >20 mm errors, the failure of
  ordinary oriented bootstrap on these sparse inputs, and the separate search
  radius/wrong-hand tests. This explicitly changes the statistical acceptance
  criterion; it does not establish a hard bound on individual sparse poses.
- Use explicit identity-matrix element assignments in the graphics foveation
  fixture to remove its existing macOS missing-braces warning.

### Validation and deployment

Validation builds use `/private/tmp/monado-merge-arm64` and
`/private/tmp/monado-merge-x64-client`, preserving the runnable development
builds and installed runtime. The x86-64 client uses the existing universal
MoltenVK dependency and its existing client-only dependency/feature selection.
The first x86-64 validation setup accidentally selected arm64 Homebrew
dependencies; correcting the temporary settings produces a successful build.

The full macOS suite passes **94/94**, including three isolated launchd service
GPU fault/recovery tests, Metal/Vulkan pixel tests, tracking fixtures and the
new handler shutdown regressions. The new handler fixture initially omitted
its per-client device slot and timed out in setup; that fixture error is fixed.
The full default-options Ubuntu 24.04 x86-64 suite passes **58/58** in
317.76 s under Docker emulation, with no compiler warnings. After the final
PCM send-lock adaptation, all six affected Linux transport/binding/rendering
test executables pass again (0.71 s), with a warning-free incremental build.
The final macOS suite passes **94/94** again in 37.70 s, including all three
isolated service recovery suites. Both native and x86-64 client builds pass.
The final targeted render check also passes after the test initializer warning
fix. Windows/Android CI and physical-headset regression were not run locally.

Checks use the CI-pinned clang-format 23.1.1, cmakelang 0.6.13, codespell 2.4.1
and REUSE 6.2.0. The contribution-check base is updated to `db1903bd4`.
Formatting/spelling for merge-changed files passes. The full fork-wide check
still reports pre-existing formatting in unchanged experiment, compositor,
tracking and QWERTY files; retained tracking source is not reformatted during
this sync. REUSE reports 153 pre-existing files with incomplete metadata:
147 lack both copyright and license information, three patches lack copyright
information, and three Markdown files have unrecognised license headers.
One additional xrizer patch references GPL-3.0-or-later without the matching
license text in `LICENSES/`. All 154 flagged files are byte-identical to the
integration parent; none is introduced by this merge.
These existing contribution-check failures are not claimed as passing.

No runtime is installed to the user's prefix, no registered service is
restarted, no headset run is made, and nothing is pushed. Before deploying this
source, rebuild matching native service/client, x86-64 client and external
bridge/proxy against the changed IPC protocol. User-operated headset
regression remains the deployment/publication gate.

## Scope and recommendation

Fetched both remotes and compared integration `ae2b6d059` with GitLab
`main` at `db1903bd4` (1 November). `origin/macos-upstream-clean` and local
integration have the same hash. Their latest shared upstream ancestor is
`9fd3968ba`; upstream has **93 new commits**, changing 184 files relative to
that ancestor. The generic-controller/I/O-cache merges `7b1c728e7` and
`302e9beaa` are already integrated through `b3ad95b0b`.

**Recommend a staged upstream integration, preserving the validated Sense
backend.** The IPC shutdown improvements are suitable to adopt. A wholesale
resolution in favour of upstream would replace tracking and compatibility
behaviour that this fork deliberately retained. The initial exploration did
not merge, commit, push, install, restart the service, or run headset hardware.

`git merge-tree --write-tree HEAD upstream/main` produced trial tree
`93d4925277201f1f5145259a4acf4a0d4e3ee50a` with **19 conflicted files**.
This tree contains conflict markers and is not a buildable merge candidate.
No integration branch or index change was made during that initial exploration.

## Shutdown: an extension of our upstream contribution

Our [MR !3029](https://gitlab.freedesktop.org/monado/monado/-/merge_requests/3029)
is upstream commit `10c57897e`. Its commit message explicitly reconstructs the
fork implementation introduced in `2114ff9f5`, with the cancellation helper's
earlier upstream provenance credited to Jonathan Steffan. The previous
generic-controller merge adopted that submitted version.

[MR !3052](https://gitlab.freedesktop.org/monado/monado/-/merge_requests/3052)
builds on it:

| Commit | Effect | Relationship to our implementation |
| --- | --- | --- |
| `5830d1ea1` | Removes the global lock from the multi-message space-location exchange and holds it only around the PCM device call in the haptic exchange. | Fixes a remaining deadlock: shutdown cannot mark threads stopped or cancel their I/O while a stalled client owns that lock. |
| `f5da48938` | Calls `shutdown(fd, SHUT_RDWR)` on Unix client sockets before joining. Adds real blocked-receive and blocked-send tests. | Extends our cancellation helper to Unix. Idle polling was already covered; handlers blocked partway through socket I/O were not. |

The following remain intact: the three-second graceful-exit window,
STOPPING-before-join ordering, locked state reads and late-start protection,
the fixed join-candidate snapshot, READY after join, repeatable cleanup,
joining before resource teardown/main-loop restart, and Windows
`CancelIoEx` plus `DisconnectNamedPipe`. The client thread still owns and
closes its socket; `shutdown` wakes I/O without transferring fd ownership.
Our existing test cases remain, with new Unix cases appended.

The shutdown helper and tests merge without conflicts. The conflict in
`ipc_server_process.c` is at startup: retain our input-lock/share
initialisation and upstream's added process-creation TODO. It is not a
disagreement over shutdown ordering. The macOS display-loss watchdog,
hosted-layer teardown, bounded GPU/display-link drain and Metal/XPC cleanup
are separate mechanisms, not replacements introduced by !3052.

Other !3052 commits complete Unix byte payloads, retry interrupted fd sends,
handle blocked haptic exchanges and reject empty visibility masks. Merge
these together with their surrounding handler changes rather than treating
Unix cancellation as a complete fix on its own.

## Conflict inventory and required adaptations

| Area | Conflicted files | Resolution direction |
| --- | --- | --- |
| Runtime manifest | `cmake/GenerateOpenXRRuntimeManifest.cmake` | Adopt upstream's target-type-aware filename helper (`aa4b32333`). It covers our SHARED/dylib workaround while distinguishing MODULE libraries. Verify installed manifests. |
| Vulkan/Metal | `vk_helpers.c`, `comp_vk_client.c`, `xrt_compositor.h` | Combine upstream tiling flags/extension support with our Metal image-create chain, native texture allocation, sharing and lifetime guards. Audit failure-path cleanup of both Metal and native references. |
| Driver bindings and Sense poses | `pssense_driver.c`, `psvr2.c` | Upstream removes driver binding tables; preserve optional Sense-to-Index compatibility and lazy gaze activation until their generated replacements are proven. Preserve LED-space pose/filter behaviour. |
| Constellation | `t_constellation.h`, `pose_metrics.c`, `t_constellation_tracker.cpp`, `t_constellation_tracker.h`, `t_constellation_tracker_internal.hpp` | Explicitly adapt the API and blob/model changes around our joint solver, EKF, bounded worker, recordings and replay. Do not silently switch the Sense runtime to sliding-window fusion. |
| IPC client compatibility | `ipc_client_xdev.c`, `ipc_client_xdev.h` | Retain necessary service-selected compatibility metadata while adopting upstream removals where generated rebindings are equivalent. |
| IPC process/transport/storage | `ipc_server_process.c`, `ipc_message_channel_unix.c`, `ipc_shmem.c` | Preserve macOS startup hooks and graphics-handle transport. Combine unique shared-memory names and MAP_FAILED handling with our private/read-only creation and mapping helpers. Compare both fd-transfer implementations before choosing short-transfer behaviour. |
| OpenXR bindings/results | `oxr_input.c`, `oxr_resolver_for_profiles.c`, `oxr_xret.h` | Carry our cache-based generic grip fallback and specific-profile preference into upstream's subaction-path-aware resolver; combine result mappings. |

The tracking change is the largest semantic risk. Upstream `54eebd9a4`
introduces rig calibration and online sliding-window fusion; `59be345db`
makes the tracker own pose queries and removes the old driver tracking-source
callback. Blob undistortion, sequence IDs and observation/sample structures
also change. These touch our joint solver and recording contracts even where
Git auto-merges a file.

The [existing fusion evaluation](macos-pssense-upstream-fusion-evaluation.md)
and [integration decisions](macos-pssense-6dof-integration.md#compatibility-decisions)
remain the basis for retaining our live backend. The historical ~300x cost
comparison evaluated an earlier MR 3015 adaptation, not the newly fetched
tip. Upstream has since changed threading, reset-on-failure and thread counts;
this review makes no fresh performance or hardware claim about those changes.
Preserve upstream behaviour for Rift/Linux while isolating the Sense-specific
adaptation.

Upstream `db1903bd4` adds a generated PS VR2 gaze rebinding, so that particular
legacy table may now be redundant. Its presence alone does not prove parity
for optional Sense Index mappings, other compatibility profiles, or our
lazy activation policy. The upstream queue-priority refactor and OpenXR
instance/system/session locking largely auto-merge, but still need builds
and lifecycle tests. The new view-configuration-change extension needs the
fork's usual opt-in treatment until validated.

## Sense preservation policy

On 2 November the user confirmed that this fork's Sense tracking works very
well during extended play. Preserve that established runtime behaviour as a
requirement of the sync. Replacing the tracker is outside the merge scope.

- Keep joint multi-camera solving, the IMU/optical EKF, calibrated LED-space
  transforms, illumination/synchronisation, reacquisition and loss policies,
  and the currently selected settings.
- Integrate unrelated upstream fixes normally. Adapt shared interfaces around
  the existing Sense backend; retain the callbacks and data it needs behind
  an explicit backend boundary where upstream removes them. The exact boundary
  needs implementation review; no source isolation has been implemented here.
- Preserve upstream's new tracker for its other consumers. Do not select the
  fork's Sense backend globally or freeze the entire constellation directory
  with an indiscriminate merge resolution.
- If those interfaces cannot be reconciled confidently in the first sync,
  defer the coupled tracking changes explicitly. Record which upstream changes
  were adapted or deferred even when their commits enter merge ancestry.
- A future Sense backend replacement is a separate experiment, requiring
  replay evidence and user-confirmed hardware benefit before promotion. It is
  not a prerequisite for receiving IPC, build, binding or compositor fixes.

## Validation performed

- Rebuilt `tests_ipc_thread_shutdown` in the existing `build/arm64` tree;
  focused CTest passes. Its dependency rebuild emitted the previously
  documented external Perfetto variadic-macro warnings.
- Compiled upstream's exact shutdown helper and expanded tests against the
  current integration headers/support libraries, with upstream's Unix
  byte/fd transport implementation. Retained this fork's graphics-buffer/sync
  transport section and its `u_misc.h` include: upstream alone has a macOS
  graphics-buffer `#error` under this fork's configuration.
- The expanded suite passes **6 cases / 800 assertions**, repeated
  **10/10 times**, each with a ten-second process timeout. The expected
  broken-pipe/partial-payload errors are the tested failure paths.
- Negative control: substituting our current shutdown helper, keeping the
  new tests and adapted upstream transport, hangs until the ten-second
  timeout. This confirms the Unix cases expose a gap in the current helper.

The isolated harness used compile flags from
`build/arm64/compile_commands.json` and the existing target's Ninja link
command, substituting temporary source/object files under
`/private/tmp/monado-upstream-shutdown-review`. It did not overwrite runtime
sources or service binaries. This validates the helper/transport behaviour,
not a full merged service or the handler lock changes end to end.

Full merge compilation, Linux/Windows CI, x86-64 client compatibility,
service recovery and user-operated headset checks remain unperformed.

## Suggested implementation sequence

1. Prepare a review merge off `ae2b6d059`, preserving both histories. Resolve
   manifest, transport/shared-memory and IPC shutdown changes first; verify
   actual stalled-message service shutdown, not just the helper fixture.
2. Adapt generated bindings/cache metadata, retaining the documented optional
   profiles and fallbacks. Run resolver/cache tests, gaze activation and
   haptic tests against matching rebuilt service/client protocols.
3. Resolve constellation as a dedicated, reviewable adaptation: keep the
   validated Sense joint/EKF path while integrating upstream changes for its
   other consumers. Run existing synthetic/replay/calibration tests and verify
   recording compatibility before any hardware comparison.
4. Combine Vulkan extension/queue changes with Metal sharing and run both
   architecture builds, Linux CI, GPU/lifecycle tests and the user-operated
   native/hosted headset gate. Review new extension defaults and installed
   manifests before publishing or replacing the installed runtime.

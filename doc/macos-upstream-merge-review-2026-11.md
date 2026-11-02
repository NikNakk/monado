<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Upstream merge exploration — 2026-11-02

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
behaviour that this fork deliberately retained. This exploration does not
merge, commit, push, install, restart the service, or run headset hardware.

`git merge-tree --write-tree HEAD upstream/main` produced trial tree
`93d4925277201f1f5145259a4acf4a0d4e3ee50a` with **19 conflicted files**.
This tree contains conflict markers and is not a buildable merge candidate.
No integration branch or index change was made.

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

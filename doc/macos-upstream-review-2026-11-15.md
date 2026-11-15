<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Branch consolidation and upstream review — 2026-11-15

## Integration implementation

The user authorised consolidation and upstream integration after the initial
review below. Work is on `integration/macos-upstream-2026-11-15`, in the isolated
worktree `/private/tmp/monado-integration-20261115`, starting at `0b6304263`.
Validation leaves the original worktrees untouched. After validation, the
main `macos-upstream-clean` checkout is advanced by fast-forward, preserving
and restoring the Unity investigation through a named Git stash. The quad
worktree stays unchanged. Binary patches, untracked-file archives and starting
heads are backed up under
`/private/tmp/monado-integration-backup-20261115`.

### Fork consolidation

- `027ce7721` merges simulator `b74d5eae2`, including the identical Ceres/session
  tips `a185d42d5`. All 22 missing commits enter ancestry. Retain current SPDX
  metadata in the three document conflicts and explicit matrix assignments in
  the graphics fixture. Combine the established <10 mm aggregate sparse-pose
  RMS criterion with the newer 100 mm individual cap and <=5% >40 mm criterion;
  acceptance and <=10% >20 mm checks remain intact.
- `e0d433ab9` merges committed quad-view tip `172b1355c`, preserving both
  histories and deduplicating equivalent fixes through Git's content merge.
  The remaining conflicts are the same three document headers.
- The quad worktree's two uncommitted edits are first-use diagnostic logging,
  not a rendering fix. They are preserved in place and in the backup, and are
  deliberately excluded from production consolidation. The Unity rate-map
  investigation is restored as uncommitted work after the integration fast-forward.
- Gaze-following insets, quad-view fast composition, Catmull-Rom filtering and
  depth reprojection retain their opt-in controls. The newly merged lens-derived
  visibility mask is now **off by default on macOS** (`PSVR2_VISIBILITY_MASK=1`
  enables it); the branch's existing Linux mask default is retained. No new
  headset acceptance is claimed.

### Upstream resolutions and preservation

Upstream `9dc151cd2` is merged after consolidation. The final merge encounter
has 30 conflicted paths, matching the initial trial count.

- Move the native IOKit backend into `aux_hid`, rename its callers to `h_hid`,
  and retain the optional receipt-timestamp callback, timestamp fallback and
  Bluetooth reconnect entry point. Link the Sense driver explicitly to
  `aux_hid`; the normal service prober link alone is insufficient for its
  standalone tracking tests. Linux retains hidraw; upstream's hidapi backend
  remains available on other supported platforms.
- Adopt endpoint/service parameterisation, threading the socket filename through
  both macOS mainloop layers. Keep native XPC activation for the built-in
  endpoint only, so a failed derived-runtime connection cannot activate the
  unrelated default service. Update both direct IPC test probes to pass
  connection information and use the generated default-endpoint header.
- Adopt the reusable runtime CMake helper, retaining conditional Vulkan linkage,
  Apple dylib/export behavior, manifest paths and target defaults. Keep the
  private three-image flag at bit 3 and the performance-tiling flag at bit 4
  when moving swapchain enums into `xrt_defines.h`.
- Adopt Vulkan printers and clock-domain conversions, retaining macOS
  CLOCK_MONOTONIC_RAW calibration, sleep-to-uptime adjustment and zero-host-time
  rejection. Remove the duplicate debug-image option introduced by auto-merge.
- Combine upstream coverage-aware alpha blending with the fork's foveation,
  depth, quad bounds and common-layer shader paths. The extra upstream blend
  struct would make the layer UBO **65,584 bytes**, over the 64 KiB limit.
  Pack four four-bit factors into the existing image-info padding word instead,
  preserving 128 layers and the static size assertion. A real GPU regression
  compares all six supported factors in every component slot against CPU
  blending and verifies that uncovered ZERO/ZERO layers cannot erase content.
- Repair the consolidation's duplicated Vulkan format-properties mock, the
  controller-visual test's missing ImageIO/CoreGraphics links and new bitmap
  enum warnings. Clearing reused bootstrap view vectors avoids copying
  uninitialised Eigen storage without changing the assigned camera matrices.
- Update the style-check base to `9dc151cd2`. Preserve the GPL license text
  required by the archived xrizer patch in the repository, but omit that text
  from the temporary source-only license view **only when REUSE reports it
  unused there and the named excluded patch references it**. Source references
  retain the text; unrelated unused licenses still fail. Three new real-REUSE
  regressions cover these cases.

### Explicit tracking deferrals

The [November 2 tracking deferrals](macos-upstream-merge-review-2026-11.md#tracking-and-compatibility-decisions)
remain in force. Keep the consolidated constellation tree and tracking API;
retain the Sense protocol and LED model, and the Rift builder's rerun hooks.
The only Sense driver changes are the mechanical HID migration/formatting.
The existing optional-Ceres fallback and ambiguity rejection from consolidation
are retained. No global upstream sliding-window tracker replacement is made.

New upstream commits entering ancestry with coupled changes deferred:

| Commits | Deferred changes |
| --- | --- |
| `7aa2d557c`, `7336a9070`, `c274aa9d1`, `8913adb25` | Fusion refactoring/determinism/logging and optimizer covariance removal. |
| `da3d98f34`, `68ab02d97`, `82428b596` | Tracker interfaces, direct frames and brightness/sample contract changes. |
| `5202f736f`, `5d339858d`, `77fd01dba`, `9dc151cd2` | Upstream solve-bound/fusion-history changes and tracker C++/keyframe history migration. Shared math relation-history changes and their expanded tests are adopted separately. |
| `f97aa1af7`, `cb6a2ca9f` | Blob scanning/splitting changes; defer detector behavior changes with the tracking migration. |
| `7352f5e1d`, `b1bbd1312`, `672521e37` | LED-model emission offsets, default advanced LED synchronization and its refinement cleanup. |

The shared `c10bb41fb` time constants and `e440845cb` HID migration are adopted;
retained tracking source continues using its established compatible types/API.
These deferrals also preserve the existing Rift backend rather than claiming
integration of upstream's new Rift fusion path.

### Validation and deployment

- Native RelWithDebInfo service/client/full build succeeds. **98/98 CTests
  pass** in 74.61 s, including all three isolated launchd GPU service fault and
  recovery suites. The separate compute check executes 29 cases / 506,906
  assertions, including the new packed-blend/coverage GPU regression.
- The x86-64 client builds against the existing universal MoltenVK dependency.
- A temporary-prefix install under `/private/tmp/monado-integration-prefix`
  produces runtime and libmonado `.dylib` manifest paths resolving to installed
  files. The runtime exports only `xrNegotiateLoaderRuntimeInterface`.
- Ubuntu 24.04 x86-64 passes **62/62 tests** with CI default build settings
  in 580.27 s, with no compiler warnings. The separate optimized run also passes
  62/62; its inherited GCC/Eigen warnings are not treated as a clean CI gate.
- With Ceres disabled, the replay tool and five focused tracking test targets
  build successfully; **5/5 focused CTests pass** (3.90 s).
- Pinned formatting/spelling/whitespace checks pass. Source-scope REUSE passes,
  and all **9 license-scope regression tests pass**.

No existing installed runtime or registered service is replaced. These are
headset-free checks; user-operated native/hosted PS VR2 regression remains a
separate deployment gate. Rebuild matching service/native client, x86-64 client
and external bridge/proxy consumers before deploying the changed IPC protocol.
The tested upstream merge is `b3b5a6af3`, following the two consolidation
merges listed above. Validation logs, binary hashes and the worktree backups
are retained in `build/review/upstream-20261115` in the main checkout; see
`validation.json`. The native and x86-64 builds were refreshed after that merge
commit so the two clients and service share a revision tag. Physical headset,
Windows and Android validation were not run locally.

The main checkout was fast-forwarded to the validated result and its Unity work
restored. The restoration combines the new lens-image paths with the pending
frame-limit option and keeps both status notes while discarding only the
superseded initial-review summary. The restored test app compiles and links
against the integrated helper libraries. The gaze document and two untracked
Unity artifacts are verified byte-identical to their backups. The safety stash
`261ae903a9d8624e9e37f4d8ed86a98684869c46` is retained; restored investigation
changes remain uncommitted.

Nothing has been pushed. The initial investigation follows for provenance.

## Finding

`macos-upstream-clean` remains the designated integration target, but does not
contain all the newer fork development. Consolidate the wanted November fork
changes before resolving another upstream merge. Do not switch wholesale to a
newer-dated side branch: those branches fork before substantial integration work.

Both remotes were fetched and pruned for this review. Local integration is
`0b6304263`; `origin/macos-upstream-clean` is `42c99bb1d`. The only local-only
commit is the November 14 AppKit main-thread window-creation guard. The current
worktree also contains uncommitted Unity rate-map investigation changes in the
port/gaze documents, PS VR2 test app and comparison script. Preserve these
separately when preparing consolidation.

## Newer fork branches

| Branch / fetched tip | Work absent from integration | Treatment |
| --- | --- | --- |
| `build/ceres-optional-constellation`, `diagnostics/session-stage-recording-20261031` / `a185d42d5` | The same 21 commits: session/stage recording and replay, LOCAL-space recentering, app/WSI pacing, display wake, CI/license fixes, ambiguous-bootstrap rejection, optional Ceres and portable tracking tests. | Primary consolidation candidate; review behavior changes as well as build fixes. These are identical tips, not two independent sets of work. |
| `test/simulator-display-size` / `b74d5eae2` | The preceding 21 commits plus simulated/QWERTY display-size overrides. | Superset of the Ceres/session line; useful baseline for consolidation. |
| `claude/quad-gaze-inset` / `172b1355c` | 26 commits absent here: shared session/pacing work plus gaze-following quad insets, one-pass quad composition, lens-based hidden-area masks, Catmull-Rom filtering, graphics depth reprojection, lens logging/still-image testing and fixes. | Review and bring wanted features in separately, retaining opt-in/default policies and validation gates. |
| Local `claude/kk-fdm-stage1` / `a2945a309` and Alyx preserve branches | Separate KK/Alyx experiments and evidence, including FDM/pipeline/hang investigations. | Retain as experiment/evidence history; do not blanket-merge runtime changes as a prerequisite for upstream. |

The Ceres/session and quad branches share integration ancestor `a89988a0b`.
There are **99 commits on current integration absent from each branch**, so a
checkout/reset to either would discard later integrated work. Relative to the
Ceres tip, the quad branch has 14 commits outside ancestry; `git cherry` finds
five patch equivalents (Windows/timing fix, REUSE, both CI updates and bootstrap
rejection), leaving nine non-equivalent patches. Some of the latter are fixes
for the new features rather than standalone features.

The session-recording and Ceres worktrees are clean. The quad worktree has
uncommitted changes in `comp_compositor.c` and `comp_render_gfx.c` (29 insertions,
five deletions), which are **not included** in the fetched quad tip or trial
merges. Include their review before claiming consolidation is complete.

Older remote branches were checked for ancestry and patch equivalence. Their
unmerged commit counts do not establish missing functionality: many are the
historical/ported tracking and compositor experiments described in the branch
map. In particular `claude/gfx-foveation` and
`macos-psvr2-timing-diagnostics` have no remaining non-equivalent patches.
The old tracking/evaluation, display-distortion and preservation lines are not
recommended for blanket integration on the basis of this inventory.

## Upstream delta

Fetched GitLab `main` is `9dc151cd2`, after the previous integrated upstream
tip `db1903bd4`: **79 new commits**, **232 changed files**, 7,642 insertions and
3,529 deletions. Important groups:

- Fixes for pacing NULL/clamping errors, Vive buffer overflow, JSON/config
  handling, returned OpenXR pointers and Windows pipe/reconnection behavior.
- HID abstraction migration from `os_hid` to `h_hid`, plus hidapi probing on
  platforms without hidraw. Adapt the fork's macOS IOKit implementation and
  callers rather than dropping it during the rename/delete resolution.
- Configurable IPC endpoint/service names and a reusable OpenXR runtime CMake
  helper. Adapt launchd/XPC activation, socket selection, service entry points,
  runtime exports/manifests and installed paths together.
- Vulkan pretty-printer/enum changes, calibrated clock-domain conversions,
  pacer creation timing and alpha-blend/system-UI shader support. Preserve
  native Metal sharing, macOS timing, passthrough, foveation and feature gates.
- Relation-history C++ refactoring and expanded tests, constellation fusion
  changes, blob splitting and BROAD/BG/STABLE LED synchronization. Upstream also
  changes the Sense LED model (`7352f5e1d`) and enables advanced LED sync by
  default (`b1bbd1312`). These are changes to proven tracking behavior, not
  mechanical conflict resolutions.

Consult the [November 2 deferral ledger](macos-upstream-merge-review-2026-11.md#tracking-and-compatibility-decisions):
ancestry already includes tracking/API changes deliberately not adopted in the
tree. New upstream tracking patches can depend on those missing changes.
Preserve the established Sense joint/EKF backend, LED-space calibration,
illumination, synchronization, reacquisition and chosen settings. Evaluate
new tracking/model/sync behavior separately; explicitly record adaptations and
deferrals even if an eventual merge makes all commits ancestors. Review
relation-history consumers even where Git merges them without conflict.

## Trial merges and next steps

`git merge-tree --write-tree --name-only` wrote temporary objects only; it did
not change any branch, index or working file:

| Inputs | Result |
| --- | --- |
| Current integration + upstream `9dc151cd2` | 30 conflicted paths, including tracking, shader/rendering, Vulkan timing, HID, CMake and IPC. |
| Current integration + simulator/Ceres/session `b74d5eae2` | Five conflicted paths: three Alyx evidence documents, `tests_joint_pose_solver.cpp`, `tests_render_gfx_foveation.cpp`. |
| Simulator/Ceres/session `b74d5eae2` + quad `172b1355c` | Clean textual merge of committed tips. This is not behavioral validation or a complete integration candidate. |

Recommended sequence:

1. Preserve both dirty worktrees and the local AppKit guard. Prepare a review
   branch from current integration, merging the wanted session/Ceres/simulator
   work, resolving the five conflicts and retaining both histories.
2. Review the quad additions and dirty compositor edits, deduplicate equivalent
   fixes, and integrate the accepted scope. Do not revive the rejected WSI
   acquisition-phase experiment: its code was removed on the session line.
3. Repeat the upstream trial against that consolidated tree. Adopt unrelated
   upstream fixes; adapt HID, IPC/CMake and rendering interfaces explicitly.
   Keep the previously deferred tracking migration separate unless a reviewed
   adaptation can preserve the established backend.
4. Build native service/client and x86-64 client, run Linux and native tests,
   verify runtime manifests, IPC/launchd lifecycle, GPU sharing and tracking
   replay. Rebuild matching external protocol consumers before deployment.
   Hardware regression is user-operated; preserve feature validation gates.

This investigation did not implement or build an integration candidate, install
a runtime, restart services, run hardware, commit or push. Conflict counts are
textual diagnostics, not estimates of correctness or total integration effort.

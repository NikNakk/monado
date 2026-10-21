<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Generic controller comparison and upstream merge — 2026-10-21

Review branch: `codex/upstream-generic-controller`, based on
`claude/wsi-fragment-output` at `90a6cdb64`. This is a review-branch merge;
`macos-upstream-clean` has not moved. The pre-existing untracked
`tools/macos-present-compare/` is outside this change.

GitLab `main` was fetched at `9fd3968ba`. Merge boundary: `ffe0a758c`,
62 upstream commits after the fork's last merged upstream ancestor
`ec188bb13`. The final eight commits introducing and adopting `xrt_io_cache`
remain unmerged for the compatibility reasons below. The original branch and
its ten local commits are preserved. Nothing is pushed or installed.

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

### Deferred I/O cache series

`2bb37b006` through `9fd3968ba` adds `xrt_io_cache` and uses it for OpenXR input,
poses and haptics, including IPC transport. The last commit removes legacy
`xrt_binding_profile` resolution. Its cache device metadata does not carry those
mappings. Upstream's conversion commit itself records omissions for hand
controller emulation, Vive Pro 2 and PS VR2 eye gaze; this fork additionally
has optional Sense-to-Index bindings absent from upstream JSON.

Merge that series after converting remaining mappings and preserving their
runtime opt-ins across the service/client boundary. Do not mark all of latest
`main` merged while silently discarding these compatibility paths. This merge
uses the upstream generated resolver wherever it has mappings and retains its
legacy fallback path for the remaining cases.

## Validation

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

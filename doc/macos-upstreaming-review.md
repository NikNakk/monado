<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

**Review of macos-upstream-clean**

**Reviewed branch:** NikNakk/monado:macos-upstream-clean
**Reviewed tip:** 0f919ce71f7b71c997d7ef22abffbbaadb9cce5f
**Recorded upstream sync base:** 045931d12f1cc9afde942f7905db08e6f51b9d8e
**Review date:** 2 October 2026

| **Purpose: turn the mature integration branch into a small, reviewable, dependency-aware series of upstream Monado merge requests, while keeping experimental and compatibility work out of the upstream core.** |
|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|

# Executive summary

macos-upstream-clean is now a credible basis from which to prepare upstream Monado contributions, but it should not itself become a merge request. The architecture is substantially cleaner than the development history: Wine/DXMT/OpenVR compatibility, the Win64 D3D11 compositor client, TCP compatibility transport and most transitional proxy protocol have been removed from Monado and moved to macos-wine-xr.

| **Category**                                   | **Recommendation**                                                                                             |
|------------------------------------------------|----------------------------------------------------------------------------------------------------------------|
| Generic bug fixes and portability improvements | Upstream first as small independent MRs.                                                                       |
| Core macOS platform/runtime support            | Upstream as a deliberately designed macOS series; probably one moderately large enablement MR plus follow-ups. |
| Standards-based OpenXR and PS VR2 feature work | Separate MRs after the basic macOS port; some are already close to reviewable.                                 |
| Experimental, compatibility or research code   | Keep out of upstream, or substantially redesign first.                                                         |

| **Primary recommendation: aim for the first macOS milestone to be a complete, working in-process Monado/OpenXR runtime on Apple Silicon if a fresh hardware test confirms that this remains viable. Add the service/XPC path afterwards. If an in-process runtime cannot form a genuinely usable port, make the initial macOS enablement MR larger and include the minimum service/Metal sharing required.** |
|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|

The Game Mode client compositor should not be part of the first macOS MR. It depends on private CAContext/CALayerHost interfaces and addresses a secondary macOS scheduling problem rather than basic platform enablement.

# 1. State of the branch

The branch is much cleaner conceptually than its ancestry. The recorded upstream sync point is 045931d12. The current branch has a long integrated history because it contains upstream merges, experiments, cleanup and automated follow-up commits; commit count is therefore not a useful measure of upstreamable work.

- macos-upstream-sync-2026-10 -\> macos-upstream-clean: 180 commits.

- macos-game-mode-upstream-sync-2026-10 -\> macos-upstream-clean: 103 commits touching 80 files.

- A large part of the latter delta consists of removing Wine/DXMT compatibility material rather than adding architecture.

- The final cleanup removed old bootstrap-name Metal reconstruction; 50855002b removed stale declarations and only two commits follow it on the current branch.

The branch documentation now draws the right architectural boundary: ordinary native Monado IPC plus generic macOS Metal resource handoff remain in Monado; Wine/OpenVR protocol translation, game policy and loopback transport belong to macos-wine-xr.

# 2. Code that should not be upstreamed

## 2.1 Wine, DXMT and OpenVR compatibility policy

This material should remain outside Monado. The clean branch has already deleted most of it, including comp_d3d11_wine_client.cpp, oxr_d3d11_wine.cpp, ipc_tcp_auth.\*, Wine HMD/audio test programs, DXMT patches, Wine/OpenComposite/xrizer launch and provisioning machinery, Wine-specific diagnostics and Alyx launch policy.

The remaining transitional per-session pacing mechanism should also be excluded from the upstream series: xrt_session_info.pacing_flags, XRT_SESSION_PACING_USE_MIN_FRAME_PERIOD_BIT, the per-session u_pacing hook and the OpenVR initializer. Only the external proxy needs it, while upstream already has a global minimum-frame-period option.

## 2.2 PS Sense synthetic tracking and controller impersonation

Keep PSSENSE_SYNTHETIC_POSITION, PSSENSE_SYNTHETIC_ARM_MODEL, PSSENSE_INDEX_PROFILE, fabricated HMD-relative controller positions and Valve Index impersonation out of upstream. They are useful compatibility experiments, not correct runtime behaviour.

The genuine Sense HID/input/calibration/orientation/haptics work is a separate matter and is potentially upstreamable. Optical 6DoF on macos-pssense-6dof should remain out until it is reliable.

## 2.3 Experimental timing evidence and historical experiment machinery

Do not upstream the research notebook wholesale. Keep detailed A/B ledgers, generated CSVs/manifests and fork-specific analysis scripts in the fork or external engineering notes. Their conclusions should be distilled into production code and MR descriptions.

- Examples: macos-env-toggles.md, judder evidence, latest-frame-worker history, stale-substitution history, generated PS VR2 prediction/continuity result sets.

## 2.4 Fork-specific workflow infrastructure

Do not submit AGENTS.md, AI-agent operational instructions, or GitHub Actions simply because they exist in the fork. Retain local/fork CI as a validation harness; change upstream GitLab CI only where the upstream series genuinely requires it.

## 2.5 Jailbreak-only headset haptics and retired external-broker override

PSVR2_HEADSET_HAPTICS should remain an experiment because it is understood to require a jailbroken headset. XRT_MACOS_METAL_XPC_EXTERNAL_BROKER is effectively retired and should be removed from the upstream slice rather than documented as a supported option.

# 3. Changes suitable for upstreaming essentially as-is

“As-is” here means that the substantive implementation does not need architectural redesign; it still needs rebasing, Monado formatting, DCO/provenance review, and focused tests where appropriate.

**Generic IPC shutdown ordering:** Strong first MR. Stop every client before joining, prevent late thread revival, cancel synchronous Windows pipe reads, and make repeated shutdown safe. The existing tests include a real synchronous named-pipe reader.

**Native Unix socket peer identity and endpoint protection:** getpeereid/LOCAL_PEERPID on macOS and SO_PEERCRED on Linux, safe lock-file ownership, preservation of live sockets and recovery of stale ones. Submit separately from Metal/XPC.

**Small portability fixes:** Group coherent fixes such as SOCK_CLOEXEC fallback, missing inttypes include, CJSON_HIDE_SYMBOLS support, and macOS config/JSON enablement; do not preserve each AI CI repair as its own commit.

**Generic dead-reckoning FIFO edge fix:** Conceptually independent. Add a targeted regression test, then submit as a small MR.

**IOKit HID backend:** A conventional platform implementation behind os_hid; suitable for the macOS foundations MR.

**macOS filesystem paths and time support:** Application Support/Cache paths, private atomic file replacement and Mach wait/time primitives are sensible platform foundations.

**Graphics-neutral FB/META foveation policy:** The split between generic foveation profiles, OpenXR policy parsing and tests is clean. Keep it separate from Metal.

**XR_KHR_generic_controller runtime support:** Independent standards work; upstream before PS Sense uses it.

# 4. Code that should be upstreamed, but needs work first

## 4.1 Core macOS presenter

The functionality is mature enough to pursue upstream, but comp_window_macos.m still mixes too many concerns: display discovery, CAMetalLayer presentation, display-link selection, timing conversion, pacing, drawable-slot worker, Vulkan/Metal synchronisation, IOSurface compositor images, refresh switching, passthrough rendering and diagnostics.

1.  Remove the PS VR2 passthrough renderer from the baseline presenter.

2.  Resolve or remove remaining A/B presentation switches, especially CVDisplayLink versus CADisplayLink.

3.  Remove trace-only knobs from production control flow.

4.  Move PS VR2-specific display policy away from generic presentation code where practical.

5.  Keep only the measured production presentation strategy.

## 4.2 Display selection

The frontend currently identifies PS VR2 by display name and then falls back to a 4000-pixel-wide display. That was acceptable for bring-up but should be replaced by an explicit relationship between the selected direct-display device and the macOS display frontend.

## 4.3 IOSurface/Metal graphics-handle plumbing

The architecture is correct: IOSurface is treated as an Apple-native graphics object; CoreFoundation ownership rules are used; MoltenVK consumes Metal/IOSurface resources; and VK_EXT_metal_objects provides interop. Before submission, perform a focused lifetime/ownership audit across all import/export paths, array swapchains, depth/stencil special cases and failure cleanup.

| **Concrete cleanup defect found: src/xrt/compositor/null/null_compositor.c contains a duplicate empty \#elif defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_IOSURFACE) branch after IOSurface has already been handled. Remove it before constructing the upstream series.** |
|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|

## 4.4 Native OpenXR Metal support

XR_KHR_metal_enable support is architecturally clean: report a Metal device, validate that the command queue uses it, expose XrSwapchainImageMetalKHR, and put a Metal client compositor behind the normal native compositor interface. It needs provenance/header cleanup, tests for binding validation and image enumeration, and isolation from experimental foveation. It may be a separate MR unless the minimum viable in-process macOS runtime genuinely depends on it.

# 5. Code requiring an explicit upstream design decision

## 5.1 launchd + XPC resource transport

The service architecture is technically strong: ordinary Monado IPC remains on Unix sockets; XPC is a narrow Apple-object side channel; peer PID/UID is kernel verified; tokens are PID-scoped and bounded; abandoned resources expire; disconnect cleanup and ownership tests exist.

The maintainers should nevertheless agree the architecture before a large MR. The key proposition is that XPC transports Apple graphics objects that cannot be faithfully represented using the existing FD-based native handle machinery; it does not become a second general-purpose Monado protocol.

## 5.2 Public external Metal handoff ABI

monado_metal_xpc_client.h exports an ABI for non-Monado processes. This is useful to macos-wine-xr and potentially sandboxed clients, but it is less clearly core Monado API. Exclude it from the basic service/XPC MR, get native Monado clients working without it, and ask maintainers whether such an ABI is desirable.

## 5.3 Game Mode client-side compositor

The measurements justify the feature, but it is not baseline macOS support and depends on private CAContext/CALayerHost interfaces. Keep runtime checks and fallback, but discuss the architecture in an issue first and submit it only after ordinary macOS service support is accepted.

# 6. PS VR2-specific feature strategy

## 6.1 Base HMD support

Keep the macOS delta to the existing PS VR2 driver small: Apple build enablement, libusb/prober integration, display path and generic correctness fixes. Do not mix prediction research, gaze calibration, passthrough and timing diagnostics into “PS VR2 works on macOS”.

## 6.2 Position prediction

The bounded-acceleration/continuity work has useful experimental evidence but still exposes many tuning parameters and large replay/trace machinery. Collapse it into a production algorithm with a small configuration surface and deterministic test. Submit it as a separate PS VR2 MR, not as macOS enablement.

## 6.3 Eye gaze

The XR_EXT_eye_gaze_interaction architecture is promising, including lazy activation and separation between public gaze and runtime-private foveation gaze. Before upstreaming, resolve the manufacturer calibration workflow: the current path depends on an externally supplied Sony eye_calibration.bin. Decide how calibration is acquired, what missing calibration means, and which user calibration belongs in the driver versus a utility.

## 6.4 PS Sense

Upstream genuine HID discovery, input, calibration, orientation, haptics and generic-controller mapping. Keep synthetic tracking, arm-model position and controller impersonation out. Optical 6DoF remains later work.

# 7. Passthrough is not currently upstream-ready

This is the clearest feature blocker in the branch. XRT_FEATURE_OPENXR_LAYER_FB_PASSTHROUGH currently defaults ON on Apple, but the OpenXR passthrough implementation still has geometry-related entry points that return XR_ERROR_RUNTIME_FAILURE with “not implemented”. The camera path is also described as an experimental room view rather than a calibrated MR system.

6.  Make the Apple passthrough build option default OFF now.

7.  Separate generic XR_FB_passthrough state/lifecycle from the PS VR2/macOS renderer.

8.  Determine exactly which extension functionality the runtime claims to support.

9.  Implement mandatory calls correctly or do not advertise the capability.

10. Move calibrated PS VR2 camera projection into an appropriate device/backend abstraction.

Missing technical pieces include camera intrinsics/extrinsics, frame/head-pose timestamp association, late correction, robust restart, complete style/color-map behaviour, projected passthrough and MR occlusion/depth integration.

# 8. Depth and foveation

## 8.1 Depth

Separate Metal depth swapchain interoperability from depth-aware reprojection. The former is useful platform plumbing and may be upstreamable after isolation/testing. The latter remains experimental after headset artifacts such as silhouettes, trails and disocclusion holes, and should remain out until demonstrably improved.

## 8.2 Foveation

Preserve the current layering. First upstream the registered graphics-neutral FB/META control plane and runtime policy. Treat the Metal rendering companion separately because XR_MNDX_foveation_metal is experimental. Decide with maintainers whether to carry an MNDX extension, wait for an EXT/KHR direction, or keep the Metal implementation internal until the public graphics-binding mechanism is standardized.

Before the Metal portion is proposed, complete outstanding validation of the direct/in-process Metal path, layer-squashed foveation, per-image map association on hardware, Chromium standard path and performance benefit.

# 9. Provenance and DCO

Treat provenance as an upstream blocker rather than bookkeeping. The integrated history includes automated follow-up commits that have not been human-certified under DCO. Some new source files also combine Collabora copyright notices with @author OpenAI, which requires a factual provenance audit rather than assumption.

- Preserve the integration branch as historical evidence.

- Reconstruct each upstream patch on a clean branch.

- Review the actual patch as a human contributor.

- Assign authorship/copyright only where factually justified.

- Add Signed-off-by only after a human contributor is willing to certify the patch under DCO 1.1.

- Do not manufacture AI or third-party sign-offs.

# 10. Interactive rebase and history reconstruction strategy

| **Do not interactively rebase the whole integrated branch. Preserve it intact and build clean topic branches from current upstream.** |
|---------------------------------------------------------------------------------------------------------------------------------------|

Recommended pattern:

git switch macos-upstream-clean

git branch archive/macos-upstream-clean-2026-10-02

git fetch upstream

git switch -c upstream/ipc-shutdown upstream/main

For each topic, cherry-pick only the commits that introduced relevant behaviour, or reconstruct the final diff directly. Then use autosquash within that small topic branch:

git rebase -i --autosquash upstream/main

For stacked topics, rebase onto the previous clean topic rather than onto the integration branch.

## 10.1 Folding AI-generated CI fixes

Rapid follow-up commits whose only purpose is to repair the build introduced by the immediately preceding feature should normally disappear into that feature commit. Examples include fcc2b313 (test initializer warning after adding Metal foveation flags) and a185abdb (Android guard / Windows test macro repair).

pick A comp/metal: add foveation layout flags

fixup B tests: initialise Metal foveation layout flags explicitly

fixup C build: fix warning from layout flag addition

fixup D ci: fix macOS build

Retain a separate commit only when it is independently meaningful, such as the generic dead-reckoning bug fix, generic IPC shutdown, or socket identity hardening.

| **Review test for every retained commit: could a reviewer understand why this commit exists without reading the next commit? If not, the next commit is probably a fixup.** |
|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------|

## 10.2 Tests and range-diff

Prefer implementation with its tests in one commit, or an immediately adjacent test commit when the test is genuinely separable. Every retained commit should ideally build and pass its relevant tests. Use git range-diff against the old development series to detect accidentally dropped functional changes while deliberately discarding CI-fix history.

git range-diff \<old-base\>..\<old-topic-tip\> \\

upstream/main..\<new-topic-tip\>

# 11. Proposed upstream merge-request series

| **MR** | **Contents**                                                                                                                    | **Depends on**  | **Recommendation**              |
|--------|---------------------------------------------------------------------------------------------------------------------------------|-----------------|---------------------------------|
| 0A     | Generic IPC shutdown ordering + Windows cancellation + tests                                                                    | —               | Submit first                    |
| 0B     | Unix peer identity / safe socket ownership                                                                                      | —               | Submit early                    |
| 0C     | Generic dead-reckoning FIFO edge fix + test                                                                                     | —               | Independent                     |
| 0D     | Small portability fixes needed by macOS builds                                                                                  | —               | Independent/grouped             |
| 1      | macOS platform foundations: Objective-C build, OS paths/time, IOKit HID, IOSurface handle lifetime, basic Apple build plumbing  | 0D              | Core macOS                      |
| 2      | Minimum viable macOS runtime: display frontend + CAMetalLayer presentation + MoltenVK/IOSurface compositor path + proven pacing | 1               | Larger cohesive MR              |
| 3      | XR_KHR_metal_enable and native Metal client/swapchains                                                                          | 1/2             | Separate if possible            |
| 4      | macOS service support: Unix IPC + launchd activation + PID-scoped XPC IOSurface/Metal/shared-event transport                    | 1,2; possibly 3 | Larger service MR               |
| 5      | Optional external Metal handoff ABI                                                                                             | 4               | Only after maintainer agreement |
| 6      | Game Mode client-side compositor + remote layer hosting                                                                         | 2,4             | Architecture issue first        |
| 7A     | XR_KHR_generic_controller runtime support                                                                                       | —               | Independent standards MR        |
| 7B     | PS Sense macOS HID/input/haptics + generic mapping                                                                              | 1,7A            | Exclude synthetic modes         |
| 8      | PS VR2 predictor improvements + deterministic tests                                                                             | PS VR2 base     | Separate hardware MR            |
| 9      | PS VR2 eye-gaze activation/calibration improvements                                                                             | PS VR2 base     | Needs calibration decision      |
| 10A    | FB/META graphics-neutral foveation API/policy                                                                                   | —               | Independent                     |
| 10B    | Metal foveation implementation/companion                                                                                        | 3,10A           | Standards decision first        |
| 11     | Metal depth swapchain interoperability                                                                                          | 3               | Keep reprojection out           |
| 12     | PS VR2/macOS passthrough                                                                                                        | 2/4             | Not ready today                 |

## 11.1 Dependency graph

generic IPC fixes ----------------------------------------------------+

\|

macOS foundations --\> minimum viable macOS runtime --\> service/XPC

\| \|

\| +--\> external Metal ABI \[optional\]

\| +--\> Game Mode hosted compositor

\|

+--\> XR_KHR_metal_enable --\> Metal foveation

\|

+--\> Metal depth interop

XR_KHR_generic_controller ----------+

macOS foundations ------------------+--\> PS Sense support

FB/META foveation control plane --------\> Metal foveation

PS VR2 eye tracking --------------------\> eye-tracked foveation

baseline macOS + mature camera path ----\> passthrough \[later\]

# 12. How large should the initial macOS MR be?

Do not split the port into meaningless fragments that individually build but cannot drive a headset. Define the first major milestone by functionality: after this MR, a native Apple Silicon build of Monado can run a standard OpenXR application on a PS VR2 with correct head tracking and headset presentation.

Preferred staging:

- MR 1 — foundations: mostly mechanical platform plumbing.

- MR 2 — macOS runtime enablement: the larger integration MR that turns the foundations into a functioning headset runtime.

The first runtime MR should exclude XPC if an in-process runtime can work without it, and should exclude launchd, Game Mode, private remote-layer APIs, passthrough, foveation, eye tracking, Sense compatibility hacks, prediction experiments and Wine/OpenVR.

If hardware testing shows that the service architecture is intrinsically required for a usable macOS runtime, merge the conceptual contents of runtime enablement and service support into one larger “Enable Monado/OpenXR on macOS” MR. A complete larger MR is preferable to an artificial half-port.

# 13. Pre-submission fixes to make now

11. Remove the duplicate IOSurface conditional in null_compositor.c.

12. Make Apple XR_FB_passthrough default OFF.

13. Remove the retired external-broker override from the upstream slice.

14. Remove the transitional per-session pacing flag from the upstream slice.

15. Remove PS Sense synthetic position/arm-model and Index impersonation from the upstream slice.

16. Resolve CVDisplayLink versus CADisplayLink and submit one production path if possible.

17. Remove remaining inert presentation experiment knobs.

18. Refactor PS VR2 passthrough out of comp_window_macos.m.

19. Audit all new copyright/author headers, especially @author OpenAI combined with Collabora copyright.

20. Add a regression test for the generic t_dead_reckoning change.

21. Ensure baseline macOS support does not depend on generated research CSVs or tracing.

22. Trim macOS documentation to material appropriate for upstream; keep detailed development history in the fork.

23. Rebase the proposed series onto current canonical Monado main rather than treating 045931d12 as permanent.

24. Run Linux, macOS, Windows and Android CI on the reconstructed stack.

25. Run a real PS VR2 smoke test on the final rebased runtime, including head movement and sustained presentation.

# 14. Upstream engagement sequence

Before the large platform MR, open one upstream issue describing the completed Apple Silicon port, PS VR2 as current validation hardware rather than an architectural requirement, Vulkan/MoltenVK composition with Metal/IOSurface presentation, the proposed staging, why XPC is an Apple-object transport rather than a second Monado protocol, and which experimental features are deliberately excluded.

Then submit the generic MRs while that architecture discussion occurs. This gives maintainers conventional changes to review first and lets them influence the boundary of the large macOS MR before substantial polishing work is spent on the wrong split.

# 15. Repository strategy for upstreaming: use a freedesktop.org GitLab fork

Monado’s canonical repository and merge-request workflow are on freedesktop.org GitLab. The upstream project directs contributors to that repository and its CONTRIBUTING.md, and Monado’s developer documentation strongly prefers changelog fragments in merged MRs. For the upstreaming programme, the clean submission workspace should therefore be a personal fork of monado/monado on freedesktop.org GitLab.

| **Recommended division of responsibility: GitHub = integration/research archive; GitLab fork = clean upstream-facing topic branches and merge requests.** |
|-----------------------------------------------------------------------------------------------------------------------------------------------------------|

| **Repository**                                       | **Role**                                                                                                                                                          |
|------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| NikNakk/monado on GitHub                             | Preserve macos-upstream-clean and historical branches, experiments, measurements, external-project cross-links and integration work. Do not rewrite this history. |
| Your fork of monado/monado on freedesktop.org GitLab | Clean branches based directly on current upstream main; DCO-reviewed commit series; GitLab CI; MR discussions; force-pushes during review where appropriate.      |
| monado/monado on freedesktop.org GitLab              | Canonical upstream remote and MR target.                                                                                                                          |

A practical local remote arrangement is:

git remote add upstream https://gitlab.freedesktop.org/monado/monado.git

git remote add gitlab \<your-freedesktop-GitLab-fork-URL\>

git remote add github https://github.com/NikNakk/monado.git

git fetch --all --prune

For each MR, branch from upstream/main, not from the GitHub integration branch. Import only the intended final changes from macos-upstream-clean, preserving provenance as needed. Push the reconstructed topic branch to your GitLab fork and create the MR against monado/monado:main.

This makes the upstream branch ancestry completely conventional, gives the freedesktop GitLab CI the exact branch that will be reviewed, and avoids carrying the historical GitHub merge topology into upstream.

# 16. Recommended final upstream shape

The eventual upstream Monado tree should contain normal macOS build support, IOKit HID, native Apple path/time primitives, IOSurface/Metal graphics handling, a working macOS compositor/presenter, PS VR2 usability on macOS, XR_KHR_metal_enable, optional service/XPC support, and generic security/lifecycle fixes. Standards/features such as generic controller, Sense, FB/META foveation, eye gaze, Metal foveation and calibrated passthrough should arrive independently as they mature.

It should not contain the history of how the port was discovered: Wine game launch scripts, DXMT patches, fake controller positioning, failed presentation modes, hundreds of experiment toggles, generated timing CSVs, AI workflow metadata or transitional RPC protocols.

| **Bottom line: upstreaming should now become a reconstruction-and-review exercise, not more integration-branch cleanup. Start with generic IPC shutdown, socket security, macOS foundations and a minimum viable macOS runtime; use a clean freedesktop.org GitLab fork for those branches.** |
|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
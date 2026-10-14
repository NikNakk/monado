<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Graphics integration review — 2026-10-14

Candidate: `codex/macos-graphics-integration`, in
`/private/tmp/monado-macos-graphics-integration`. It preserves both the previous
GPU synchronisation branch and the Vulkan follow-up by ancestry. The user
approved local integration after the Linux gate passed. `macos-upstream-clean`
was fast-forwarded from `805316beb` to reviewed commit `c2c608a3f`; the main
checkout now uses that branch. This documentation checkpoint follows the same
isolated development branch. Following separate installation approval, the
service and Wine x64 Monado runtime were deployed from `84b3749c2`, both carrying
`v25.1.0-2310-g84b3749c2`; see the [installation record](macos-service-direct-xpc.md#integration-service-and-matching-wine-client-installation--2026-10-14).
The remote branch and Wine/DXMT project code have not been changed.

## Sources and scope

| Source | Revision | Disposition |
| --- | --- | --- |
| Remote `macos-upstream-clean` | `bc9591f0313508cdb19fcb3ddf8e2c39c1e2232b` | Integration ancestor; checked against the live remote |
| Local `macos-upstream-clean` | `805316beb` | Also an ancestor; retains the macOS installed manifest correction |
| `codex/macos-gpu-sync-harness` | `95c0c6425` | Inherited GPU readiness/reuse, recovery, diagnostics and measured compositor work |
| General follow-up | `e393b5dff` | Ten intervening commits retained, including clock/recentre/build-script changes |
| `claude/vk-client-shared-event` | `b4dc8e554` | Standard Metal-object timeline handoff and its evidence |
| `codex/macos-vulkan-metal-swapchains` | `1b926f399` | Array/depth sharing, interoperability proofs, lifecycle guards and user visual acceptance |
| Review follow-ups | `041546532`, `ce9332355`, `6293ba232` | Defaults, pending-barrier/acquired-destroy fixes, raw-clock diagnostics and Linux warning/test-initialisation fixes |
| KosmicKrisp archive | `1025b3623230965052df1e3c1373adf24c52037b` | Local archival tag only; Mesa patches are not runtime dependencies |

At review commit `6293ba232`, the candidate was 129 commits ahead of the remote
integration ancestor, with 195 changed files, 22,441 insertions and 446 deletions.
Much of that scope is test tooling and dated evidence, but this is a cumulative
integration review, not merely the last Vulkan patch. The separately authorised
[Linux solver correction](macos-linux-solver-gate.md), `60bb084d7`, and macOS
warning cleanup, `4e468fe7d`, follow those commits.

## Decisions

Enable `XRT_MACOS_VK_CLIENT_SHARED_EVENT` and
`XRT_MACOS_VK_CLIENT_METAL_TEXTURES` by default on macOS. The user confirmed
service and hosted `hello_xr` and stereo-array output on PS VR2 without diagnostic
readbacks. Each option remains independently disableable with `=0`.

Ordinary single-layer colour keeps IOSurface. Vulkan arrays/depth reuse the
existing Metal texture provider, import its actual textures through
`VK_EXT_metal_objects`, verify pointer identity, and forward native swapchains
through the existing compositor. Native image counts remain compatible with
colour/depth pairing. No second compositor or intermediate texture is added.
The shared-event signal follows rendering on the application queue; compositor
readiness and image reuse use existing explicit GPU ordering. Capability/setup
failure preserves the CPU handoff fallback. Unsupported image shapes are
explicitly rejected rather than falling into the old IOSurface assertion.

Retain Metal and DXMT paths, the existing macOS conservative USB policy and
Linux camera defaults. No KosmicKrisp, Linux VM, D3D12 bridge or desktop game
presentation changes are part of this candidate. Vulkan support does not alter
which graphics API existing applications request.

Keep unvalidated compositor performance experiments opt-in: quad bounds,
projection-plus-quads specialization, queue priority changes, latency guard and
late-render delay are not promoted by the Vulkan default change. Historical
benchmarks establish particular workloads, not universal improvements.

## Review findings fixed

1. **Pending Vulkan barrier fence reuse.** Valid clients can acquire/wait/release
   repeatedly before submitting a frame. Reusing a pending fence previously
   failed. Retire it nonblockingly and allocate a fresh fence, polling completed
   retirements and bounding the list at 64 fences per swapchain. No new frame
   CPU wait is introduced. An intentionally GPU-gated 12-cycle fixture fails
   with the previous runtime and passes with the candidate.
2. **Destroying acquired images.** An acquired flag previously retained a
   healthy frontend indefinitely. A queue completion marker now covers prior
   submitted work before clearing those flags. Failures retain ownership
   conservatively. Twelve sequential sessions destroying acquired swapchains
   pass without exhausting the frontend budget.
3. **Diagnostic clock domains.** The inherited runtime uses macOS
   `CLOCK_UPTIME_RAW`; the service GPU fixture, Vulkan probe and tracking capture
   window still used POSIX monotonic time. After system sleep, comparing those
   timestamps produced false recovery failures. Align those diagnostics with
   the runtime and preserve analysis of older captures. Vulkan calibrated
   timestamp capabilities are unchanged.
4. **Linux build/test defects.** A debug mutex fixture lacked zero-initialised
   state; GCC reported benchmark deleter/unused-variable warnings. Correct these
   without changing production tracking behavior. The remaining inherited pose
   suite failures are handled in a distinct, user-authorised solver change,
   with frozen cross-platform regressions, corrected noisy-fixture contracts and an unchanged-code negative control.
5. **macOS warning gate.** Fully initialise OpenXR diagnostic structs and use
   consistent signedness in the optional late-delay timer. No diagnostic delay
   default changes.

The previous experimental branch's GPU resource-fence/reuse protections remain;
its black-corruption recurrence and recovery ledgers support retaining them.
The hosted-array black screen was an event-loop defect in the test client,
fixed by `d419f8527`; it did not require a texture transport change.

## Validation and performance evidence

| Check | Result / evidence |
| --- | --- |
| Full native macOS build | Pass, ARM64 RelWithDebInfo; timing diagnostics and service GPU fixtures enabled |
| macOS CTest after solver correction | 78/78; 21 GPU tests and three service recovery cases; 48.35 s |
| Diagnostic Python tests | 29/29 before solver correction; no Python changes in that correction |
| Default-options Vulkan probe | 12 GPU-gated barrier re-acquisitions, then 30 frames; pass |
| Acquired-image destruction | 12 sessions × 15 frames; pass |
| Default-options concurrent failure/replacement | Healthy 300-frame client plus failed overlay and replacement; pass |
| Default-options unmodified `hello_xr` | Eight-second offscreen session; pass |
| Frozen Linux regression input on macOS | Full solver: 1,089 assertions / 13 cases pass |
| Frozen Linux regression input on Ubuntu | Full suite passes; original solver reproduces the re-acquisition defect |
| Ubuntu 24.04 x86_64 default build | 48/48 pass, 331.37 s under emulation; no compiler warnings |
| PS VR2 visual checkpoint | User confirms service/hosted `hello_xr` and stereo arrays at the earlier Vulkan checkpoint; no fresh candidate hardware run |

Raw offscreen results are under `/private/tmp/macos-integration-tests`;
compact durable evidence is in [macos-graphics-integration-results](macos-graphics-integration-results/).
The initial sandbox-restricted macOS GPU/socket run failed access permissions;
all 78 tests passed when rerun with ordinary host access.

MoltenVK shared-event timing evidence is in
[the environment ledger](macos-env-toggles.md#vulkan-client-shared-event-frame-handoff-2026-10-14):
loaded PS VR2 probes improved roughly 34 to 88–90 frames/s with service composition
and 45 to 90 with hosted composition. Those are synthetic workloads and
predicted-frame timing, not a real-game performance promise. Exact texture
identity, independent-process pixels at event values 1–4 and a missing-wait
negative control are documented in [the interoperability note](macos-vulkan-metal-swapchains.md).

## Remaining scope and integration plan

The local integration fast-forward is complete. The live remote remained
`bc9591f0313508cdb19fcb3ddf8e2c39c1e2232b` at integration; recheck it immediately
before any later publication. Keep the Linux solver
fix separately revertible from the graphics changes. No upstream merge request
or DCO certification is created: the project's contribution policy requires
human sign-off for upstream submission. Source remains separable from WineOpenXR,
DXMT and the archived Mesa experiment; no external project code was adapted in
these review corrections.

The separately approved installation is complete: normal ARM64/runtime and
x86_64/client builds match at `84b3749c2`, the prefix is updated and the existing
LaunchAgent is re-registered with its preferences preserved. Hardware runs remain user-operated. A fresh four-case headset repeat
on the final installed revision provides the installation checkpoint; existing
Metal/Wine clients should then get their normal regression runs. Intel Mac
hardware, combined real-game performance and PS Sense replay/performance for
the solver correction remain unmeasured. D3D12/VKD3D-Proton and a DXVK/DXMT XR
comparison remain separate follow-ups, with DXMT retained.


## Installed-build headset gate before publication

**Pending: do not push the default-on Vulkan integration before this check.**
The user confirmed the original implementation (`f1841a3bc` with probe fix
`d419f8527`). That hardware checkpoint precedes `041546532`, which changes
barrier-fence retirement and acquired-swapchain teardown. The offscreen proofs,
recovery tests and passing builds do not establish headset acceptance of those
later changes.

The installed service/native client and Wine x64 client currently match
`v25.1.0-2310-g84b3749c2`. All commits since that installation are documentation
only. The user-approved default-on configuration is retained locally while
publication awaits the user's four-case repeat on this installed version:

- Vulkan `hello_xr`, service composition.
- Vulkan stereo arrays, service composition.
- Vulkan `hello_xr`, hosted composition.
- Vulkan stereo arrays, hosted composition.

Leave both Vulkan sharing switches unset to exercise their defaults and disable
source/presentation diagnostic readbacks. Confirm eye colours, depth/occlusion,
presentation, shutdown and a subsequent clean session; record the tested git
tag, run configuration and user observations. The runner's default mode starts
an isolated build-tree service, so do not describe such a run as an installed
service test without confirming the actual runtime/service paths. Hardware runs
remain user-operated. There is no new headset result in this review follow-up.
If the source/runtime changes again, repeat against the newly installed matching
binaries before publication.

## Acquire-wait event polling concurrency follow-up

Static inspection confirms a potential race in the macOS Vulkan wait path:
`vk_implicit_acquire_image` calls `oxr_session_poll` after a 50 ms image-wait
timeout, while another application thread can enter the same function through
`xrPollEvent`/`oxr_poll_event`. `oxr_session_poll` drains XRT events and mutates
`state`, visibility/focus, presence and `has_lost` without a session-level poll
lock. The instance event mutex protects queue push/pop, but is acquired after
session polling in `oxr_poll_event`; it does not serialize those mutations.
Reading `has_lost` in the acquire loop also needs a synchronization strategy.
The isolated [loss-status fix](macos-vulkan-acquire-loss-status.md) now removes
the acquire-side event dispatcher and makes loss access atomic, with focused
concurrency regression coverage. It is not integrated or installed. The original
finding was static inspection, not a reproduced full-runtime race.

Do not simply remove polling and restore an unbounded acquire wait: the loop
was added for failed-producer recovery. Prefer a loss-aware/cancellable XRT
image wait that does not dispatch OpenXR events from the acquire thread, or a
consistent session/event synchronization design covering all relevant state
accesses. Validate concurrent event polling, pending GPU ownership, session
loss and teardown with a targeted concurrency regression, ideally under a
race detector. A lock around only the acquire-side call would not protect the
other poller or all state readers. The user classified this as nonblocking for
the local merge. See the linked fix record for validation and deployment status.

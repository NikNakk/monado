<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Graphics WSI acquisition phase — 31 October 2026

> **Removed, 7 November 2026.** The headset pilots below did not support the
> candidate, and the 6 November wake-timer and adaptive-margin work addressed
> late targets another way. The `XRT_MACOS_WSI_ACQUIRE_AT_RENDER` code was
> removed; this note remains as the experiment record.

An isolated, default-off experiment for native Alyx's validated KK/graphics WSI
stack. Its 29-minute recording associated 1,281 skipped-target episodes with
preceding post-submit image waits (median 12.09 ms). This is software scheduling
evidence, not physical presentation feedback or a complete jitter diagnosis.

## Previous experiments determine scope

| Existing result | Constraint here |
| --- | --- |
| [Judder ledger](macos-psvr2-judder-evidence.md): acquisition on the compositor thread can lose cadence; native newest-frame worker superseded legacy substitution. | Blocking placement is established history. Do not revive the unsuccessful one-slot worker. |
| [GPU efficiency](macos-compositor-gpu-efficiency.md): direct prefetch, callback output and two renderer resource sets lost physical cadence or increased buffering. | Add no prefetch worker, resource set, native output replacement or queued frame. |
| [WSI KK follow-up](macos-wsi-kk-followup.md): generic WSI owns FIFO, KK lacks GOOGLE timing. | Keep FIFO and three images; invent no presentation feedback. |
| [Adaptive pacing](macos-wsi-generic-experiment.md): corrected feedback traded latency for tails. | Keep CV fallback pacing and fixed quality; no unsupported adaptive feedback or copied calibration. |
| [Timing ledger](macos-psvr2-timing-diagnostics.md): current-frame timestamp waits confounded worker tests; stale target poses and early source reuse caused separate visual defects. | Preserve deferred readback, fences, source GPU-read protection and fresh warp poses. |
| Two-drawable/minimum-hold trials changed latency/stability; WSI compute shimmer persisted despite good cadence. | Keep image count, app pacing and frame hold fixed; restrict the experiment to graphics. |

## Minimal candidate

`XRT_MACOS_WSI_ACQUIRE_AT_RENDER=1` skips post-present fallback acquisition only
for graphics `wsi` without supported present-completion waiting. The next frame's
existing `acquired_buffer < 0` branch acquires at render entry, before fence
retirement and fresh warp-pose queries. Native, compute, adaptive, real
completion-wait paths and Linux retain their original behavior. The effective
path logs once; the option defaults off.

The hypothesis concerns when the next target is chosen. An image can miss the
pacer's conservative wake budget while still leaving enough render time. Choosing
the target only after that wait can skip a usable slot. A real-fake-pacer test
demonstrates this tradeoff and checks that retaining a target cannot rescue an
image arriving after its deadline. Acquisition can still block at render entry:
this is not a nonblocking architecture or a claimed performance fix.

No Vulkan semaphore, layout, fence, source ownership, shader or image-count
policy changes. An inactive-display error message additionally reports the
display ID/online state to help diagnose failed setup.

## Build and checks

Base `f9546dc08b963875d8e5ef178245e697d0cf4e36` plus this patch. Parent-workspace
output: `build/monado-wsi-acquire-phase-20261031`. A CMake seed copies 115 explicit
options from the working headset build, including RelWithDebInfo and full timing
diagnostics off. That build and installed binaries are untouched. The new source
also contains root-to-stage recording improvements; an off/on pair uses the same
new binary to control those differences.

Build passes with one pre-existing `%u`/`size_t` warning in the recorder, outside
changed files. Seven targeted CTests pass: pacing, presenter parsing, four
fresh-process presenter selections and GPU reuse. The placement/deadline case
passes seven assertions. Linux was not built.

Five independent simulated desktop sessions, control/candidate/candidate/
compute-negative/control, submit **900 OpenXR frames**, exit 0 and have no service
error lines. All confirm simulated devices, KK service composition, three actual
images and asynchronous Vulkan client submission. Joined acquisitions after the
same frame's submit are **242/243 and 122/123** for controls, **0/244 and 0/242**
for candidates, and **63/64** for the compute negative, which logs inactive.
These establish path selection, submission and teardown, not pixel equality,
Vulkan synchronization conformance, physical cadence or perceived latency.

Earlier failed captures are retained. A sleeping desktop was online but inactive;
the unchanged control faulted during initialization before the option ran.
A temporary display-awake assertion enabled the successful matrix. Early marker
assertions also failed because generic info logging was filtered, although traces
already showed changed acquisition placement. The final build uses the normal
compositor logger. Failed captures are not included in the passing count.

Final service SHA-256:
`4c6d2cd5ed60c8fbfbdefef75a965f961122c8390fd96ae55f3c0067b9cf97d6`.
Runtime SHA-256:
`6781df62337d40d41c95d62e9935cdd96128fb85b2c8a3bbf968902ffeb4b823`.
Private build/test logs and the final frame-join audit are retained in the parent
workspace's `maintenance/wsi-acquire-phase-*` files.

## Headset gate

Use the same query-fixed host, xrizer, drivers, save, image count, gaze, MSAA and
fidelity with the new runtime's switch off/on. Load the save, left-trigger
unpause, then perform similar slow/fast turns and canister shots. Compare skipped
targets, acquisition tails, GPU durations, source reuse/age, jitter, latency and
visuals. Reverse order after a promising first pair before promotion. KK still
provides no actual-time feedback: software targets are not physical panel cadence.
No headset result or default promotion is claimed by these desktop checks.

## First headset pilots: not accepted

The off/control pilot reached its 600-second limit without a guest fault; the
user said “Much better.” The on/candidate pilot, configured for 1,800 seconds,
aborted after about 172.6 seconds; the user said “About the same.” The game
process's `VKRenderThread` failed with SIGABRT and an invalid-free report inside
AGX framebuffer/render-encoder creation, reached through KK command replay.
The service remained alive until client disconnect, then exited 0. This is
different from the original Alyx query-list fault. An earlier Metal encoder
fault shared that path but had a different exception; causality is unresolved.

No start/end marks were made and the control generated new saves. These are
free-play pilots, not a matched benchmark. They do not justify promotion.
Settings were restored and all test host/service processes exited. Runtime
defaults remain unchanged. The parent workspace retains the crash report,
binary identities and `maintenance/acquire-headset-candidate-a-failure-20261031.json`.

## Marked same-start follow-up, 31 October 23:23

The user confirms a quicksave made immediately before A's first mark was loaded
immediately before B's first mark, giving the same starting scene/position.
Both successful marked runs exit 130 without guest faults and restore convars.
Runtime/driver hashes and other rendering settings match. The 44.09/50.05-second
trimmed windows have similar mean head angular speed (43.05/43.10 degrees/s).

A/B application completion is 88.12/88.66 frames/s, source reuse 20.81%/26.53%,
and compositor GPU median 0.513/1.937 ms. The software source-target timestamp
99th percentile is 11.122/22.244 ms. Long wake gaps are 4/3,960 versus 0/4,499,
too few for a robust tail conclusion. Reuse remains higher in B within quiet,
slow and fast motion bins. The user reports about the same subjective result.
This one short pair supports leaving the option off, not promotion. GPU elapsed
duration includes contention; software timestamps are not physical latency.

The first B attempt also failed during startup in `strlen`, passed a bad string
address `0x67c` by `libnetworksystem.so`. That is a separate failure from AGX
encoder creation and the old query crash. Pointer origin and the acquisition
option's causal role remain unresolved. Its successful retry is the marked
measurement; the failed attempt remains part of the stability record. Parent
workspace evidence is `maintenance/pacing-ab-marked-analysis-20261031.json` and
`maintenance/pacing-ab-startup-failure-20261031.json`.

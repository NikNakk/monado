<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# SteamVR independent-vsync trial — 2026-10-13

**The notification gap can be removed, but the tested heartbeat does not resolve
loaded Home's keyed-mutex delays. The experiment is removed.** The exact
pre-trial bridge source and driver DLL are restored; no new clock toggle or
synchronization policy remains. Monado's installed runtime was not replaced.

## Implementation and isolated checks

The trial added a driver notification thread anchored by successful OpenXR frame
prediction, without calling OpenXR or accessing images on that thread. It skipped
missed ticks rather than replaying bursts, synchronized its clock/counter state,
stopped on deactivation and failed frame begin, and expired after 250 ms without
an anchor. Source ownership, GPU fences and the wait/begin/end sequence remained
unchanged. A refined variant retained normal PostPresent notifications and only
supplemented stalls after two refresh periods, suppressing duplicate refreshes.

The driver cross-compiled. Clock fixtures passed on macOS and CrossOver/Wine:
notifications during a 100 ms source stall, duplicate suppression, reanchoring,
suspension, lease expiry, stop and restart. Production PostPresent extracted into
a fixture kept one notification per refresh and the original OpenXR sequence.
The existing six-case production Present lifecycle test passed, retaining balanced
image release, source protection and failure behavior. MinGW's timed condition
wait was too coarse in the first Wine fixture; the subsequent trial used native
relative timers. These clock fixtures do not establish physical scanout timing.

## Foreground and measurement controls

Early exploratory comparisons lacked renderer verification and cannot support
Home performance claims. Home's loading/system scene, dashboard and adaptive
image dimensions varied. A size-based readiness check was also insufficient.
The final pair checked OpenVR's scene key, loading state, dashboard visibility
and last-renderer PID, requiring Home to be the renderer throughout nine
five-second checks. Dashboard `enableDashboard` was temporarily false in both
runs and its original absent/default value was restored afterwards. This is a
Home-only result, not validation with the dashboard active.

Both used a temporary build/arm64 service, native bridge runtime, GPU priority 0
(logged foreground readback 0), four-image copy fallback and the same finite paced
GPU competitor. Actual QPC windows were 55.944 / 56.274 seconds, including the
foreground-check overhead. The nominal 4 ms/8.33 ms competitor is not a reserved
GPU share: elapsed batch medians under contention were 15.220 / 15.461 ms.
Home's adaptive fidelity remained enabled, so rendering cost was not fixed.

Source acquire and notification clocks came from buffered driver CSVs. Pose
intervals were unioned per PID/TID to avoid counting nested proxy wrappers twice.
These are one sequential A/B pair with diagnostic overhead, not unbiased
population statistics or a repeated performance acceptance study.

## Verified loaded result

| Metric | Original notifications | Stall-only heartbeat |
| --- | ---: | ---: |
| Source acquires | 868 | 915 |
| Acquire failures | 23 | 19 |
| Acquire median | 0.310 ms | 0.381 ms |
| Acquire p95 | 27.679 ms | 33.032 ms |
| Acquire maximum | 100.868 ms | 106.594 ms |
| Median notification gap | 64.303 ms | 8.371 ms |
| Notification gap p95 | 90.750 ms | 18.383 ms |
| Median Home WaitGetPoses | 49.997 ms | 45.394 ms |
| Long acquires (>20 ms) with a notification during the wait | 0 / 67 | 111 / 111 |

The intended decoupling works: notification continues during blocked source
acquires. Nevertheless, long ownership/acquisition waits persist, acquire tails
worsen, and the small failure-count reduction does not establish a reliable fix.
A modest pose-wait reduction is insufficient to promote a new display clock.
Both native traces contain physical callbacks near 120 Hz over their own longer
spans, including reprojection; these clocks were not joined to the QPC analysis
window. Neither game FPS, absence of black frames nor latency/visual improvement
is inferred from that scanout rate.

This rules out missing vsync notifications as a sufficient explanation/remedy
for this loaded case. The next attribution should focus on producer ownership
under GPU backpressure and Wine handoff, using the ownership history and producer
wait counters. The experiment does not prove which remaining component dominates.

## Failed attempts and restoration

One attempt crashed SteamVR's server before Home/clock readiness; dump fault
addresses were in kernel32, which does not establish the root cause. A retry ran.
A utility-client readiness probe lacked compositor access; it was corrected to
a checked background client. A desktop-control attempt timed out. Those attempts
provide no performance evidence and were excluded.

All capture-owned XR processes and temporary services stopped; Valve's DLL was
restored. Existing Windows Steam remains running and display sleep is inhibited
for follow-up. The registered driver SHA-256 is restored to
`fb06bc1433fca684db9ef3420874b97b9b279b8456e6362f420c3465ab147bf2`.
External pre-existing bridge/DXMT changes are preserved byte-for-byte for the
modified driver source. The next build is forced to recompile that restored
source rather than reuse the trial object. No failed heartbeat code is retained.

Raw files and trial backups remain only in `/tmp/mwxr-vsync-trial/` and the
`/tmp/mwxr-vsync-*-20261013/` capture directories. Final evidence is in
`mwxr-vsync-home-only-control-20261013` and `mwxr-vsync-home-only-fallback-20261013`.
Only this concise result is retained in Git.

<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# macOS compositor thread policy after session recreation

The user authorized this correction on 2026-10-03 after analysis of the
Underture PID 77379 traces. OpenComposite recreates its temporary session for
the application's graphics device. Monado's replacement hosted compositor
starts a new thread, but a process-global refresh-period cache skipped applying
Mach time-constraint policy at the unchanged refresh rate. The bootstrap
compositor used policy 2 / priority 97; gameplay samples used policy 1 / priority
31. The diagnostic thread ID was also cached globally, obscuring the replacement.

The configured period cache and realtime diagnostic state are now thread local.
Each replacement thread applies its policy, initializes its own CPU/wall deltas,
and samples its own thread identity and detailed policy. Trace-file ownership and
row counters remain process-wide. This changes only macOS code; it does not
continually reassert policy or change the existing scheduling budgets.

## Build and verification

Compiled the ARM64 service and OpenXR client, plus the x86-64 OpenXR client,
from this workspace, including its existing unrelated worktree changes:

- ARM64 build: `.build/in-process-native-hardware-current/monado-arm64`.
  Targets: `monado-service`, `openxr_monado`. Its settings are copied from
  `build-wine`, with architecture explicitly `arm64` and PS VR2/PS Sense enabled.
- x86-64 build: `.build/in-process-native-hardware-current/monado-x64`.
  Target: `openxr_monado`; hardware drivers remain disabled in this client build.

Both clients include the optional hosted compositor. The isolated ARM64 build
leaves the installed LaunchAgent's `build-wine` binary untouched. Rebuild both
architectures after the commit so their generated IPC version tags match. The
new x64 client must be paired with the matching newly built service; Monado's
normal IPC version check remains enabled.

A standalone lifecycle probe compiled the actual policy-update function from the
header and started/joined three native threads sequentially at the same refresh
period. Each thread calls the helper twice. On ARM64 and x86-64, all three receive
a non-default Mach time-constraint policy, with exactly one application per thread.
A negative control using the previous process-global cache fails on the second
thread with default policy and period zero. This validates thread initialization,
not Underture's eventual display rate or subjective smoothness.

Build and probe logs are under `/tmp/monado-thread-policy-*`; archived evidence
is in the bridge checkout's `docs/results/compositor-thread-policy-2026-10-03/`.
Underture's physical cadence and moving-head smoothness must still be retested.
See [the judder evidence](macos-psvr2-judder-evidence.md) for the measured delay
and [the presentation defaults](macos-psvr2-timing-diagnostics.md) for current
pacing settings. No hardware run or service installation accompanies this build.

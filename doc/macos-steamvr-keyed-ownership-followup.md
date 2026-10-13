<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# SteamVR keyed ownership and OpenVR waits — 2026-10-13

The previously blocked OpenVR ownership trace now runs successfully in Home.
It identifies pose waiting during keyed ownership as a substantial contributor,
but does not establish one cause for every timeout or a proven synchronization
fix. No keyed ownership, GPU-fence, pose or scheduling policy changed.

## Capture and results

The existing Wine/DXMT diagnostic installation and opt-in OpenVR shim were used
with `MWXR_OPENVR_SHIM_TRACE=1`, pose correction off, a temporary build/arm64
service and installed GPU priority 0. The launcher reused the existing Windows
Steam because its XR environment matched; it did not need to restart Steam.
Home's trace confirms PID 2772 and QPC frequency 10 MHz. The locally verified
CrossOver QPC/Wine continuous-clock epoch allows joining call intervals to
Wine server tick_us. This clock equivalence is not portable to arbitrary Wine.

The new [analyzer](../scripts/macos/analyze-steamvr-keyed-ownership.py) deduplicates
reprinted server history, uses server held_us rather than coarse DXMT tick
boundaries, unions nested call intervals, excludes incomplete windows and
trace-flush overlaps, and associates timeout snapshots with their owner rather
than the caller. These are failure-triggered history windows, not all-frame
statistics. Overlap is not proof that the blocked call caused the whole hold.

| Selected records | Count | Median ownership duration | Median WaitGetPoses overlap | Median fraction per interval |
| --- | ---: | ---: | ---: | ---: |
| Successful Home releases in retained server windows | 30 | 22.227 ms | 13.468 ms | 63.5% |
| Timeout snapshots whose current owner is traced Home | 10 | 20.045 ms | 4.069 ms | 20.3% |

A timeout snapshot's held_us measures the current owner's hold at the timeout;
it is not the caller's entire 100 ms wait. Owner changes during waiting can
explain a shorter held_us. The two rows must not be pooled or treated as a
population estimate. DXMT's coarser long-hold samples suggested roughly 21 ms
pose overlap in a 24 ms median hold; use the exact server join above for the
retained result. Rendering, scheduling and other untraced waits remain possible.

## Concrete next intervention

`driver.cpp` direct-mode `Present` acquires the sync texture with a 100 ms
limit. Its `PostPresent` then calls `WaitAndBeginFrame` and emits `VsyncEvent`.
A blocked source acquisition therefore delays that notification. Home can
already own its source texture while blocked in `WaitGetPoses`. This makes a
feedback loop plausible, but the present trace contains no driver vsync-event
clock proving that causal ordering on individual stalls.

The next controlled trial should separate display-clock vsync notification
from source acquisition. Preserve the OpenXR wait/begin/end sequence on its
existing frame thread, keep source ownership and GPU fence protection intact,
and send no duplicate vsync events. A separate notification thread needs a
stable phase/refresh anchor, shutdown/session-loss handling and synchronized
clock state; a free-running timer is not proven physical scanout timing.
Before changing production scheduling, instrument Present/PostPresent/vsync
boundaries and test an isolated heartbeat implementation against delayed source
acquisition. Then A/B Home at priority 0 and check wait overlap, acquisition
failures, source correctness and headset cadence. Shortening the timeout alone
would create more empty-layer frames; releasing an application-owned mutex from
the shim would violate its ownership contract. Neither was implemented.

## Retained validation and cleanup

Three Python tests cover nested calls/reprinted histories, owner-vs-waiter
attribution, incomplete windows and trace-flush exclusion. No C/C++ code changed
and no new game-performance or visual acceptance result is claimed. The capture
owned only SteamVR/Home and the temporary service; all stopped after the run.
Valve's original DLL was restored (SHA-256
`d421bbaf847361e4129e25f20f9e8575e72e404cbca73405b5569a8db4807658`). Windows Steam
remained running. The installed Monado runtime was not replaced. Display sleep
is inhibited for follow-up testing.

Raw trace and summary are in `/tmp/monado-steamvr-keyed-followup-20261013/`;
Wine history came from the existing prefix server's
`~/Windows/logs/run-mwxr-20261013-183706/steam.log`. Only code and this concise
record are retained in Git. External bridge/DXMT uncommitted work was preserved.

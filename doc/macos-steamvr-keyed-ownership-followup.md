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

The subsequent [controlled vsync trial](macos-steamvr-vsync-trial.md) implements
and tests this hypothesis: notifications continue through source stalls, but
loaded Home acquisition delays persist. The unsuccessful clock is removed.

## Intervention proposed before the trial

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

## Handoff order and the 90 Hz start — 2026-10-13

Interleaving the 21:40 run's server history with the shim trace shows the
same cycle every Home frame (the mutex is SteamVR's sync texture, key 0):

| ms | Event |
| ---: | --- |
| 0 | Home enters WaitGetPoses and releases |
| 0.02–0.4 | vrcompositor acquires and releases |
| 0.6–0.8 | driver Present acquires and releases |
| 1.2 | Home re-acquires, still in WaitGetPoses |
| 13–15 | WaitGetPoses returns; render; two Submits |
| ~18 | vrcompositor queues an acquire (`0x103`) right after Submit |
| 22–24 | next WaitGetPoses; release |

Home holds the token about 97% of the time, almost all of it inside
WaitGetPoses, and only gives it up at its next WaitGetPoses, 4–7 ms after its
last Submit. It never calls PostPresentHandoff. The cycle averages about
22.7 ms, about 2 × 11.1 ms rather than a 120 Hz multiple. Every SteamVR start
in `vrcompositor.txt` logs `Updated HMD Prop_DisplayFrequency_Float to
90.000000`, and Home's session stats report `FPS Average Target 90`. The bridge
driver started from its `displayFrequency` default of 90 and only reported
119.88 Hz about 0.6 s after the compositor had initialised; the compositor
logs no change. The working hypothesis is that SteamVR throttles Home against
a 90 Hz budget (half rate under load), and that the long holds are that
throttle. The heartbeat trial kept vsync events flowing but could not change
it.

Separately, the driver's "vsync to photons" moved 16.7 → 25 → 91.8 ms
(12 refreshes) in the 22:41 run. The driver's OpenXR frame begins in
PostPresent and ends in the next Present, after its up-to-100 ms source
acquire, so Monado's app pacer counts Home's hold as app frame time. Not yet
changed.

Two bridge changes are built for a hardware trial (not committed or
tested on hardware):

- The driver reports the runtime's refresh rate from activation; the
  `displayFrequency` setting now defaults to 0 and only overrides it. The
  first version used `xrGetDisplayRefreshRateFB`, which the bridge's Wine-side
  runtime does not pass through, so run `run-mwxr-20261013-232237` (isolated
  `build/arm64` service at `6393b6592`) still started SteamVR at 90 Hz. The
  driver now falls back to the shortest period of eight empty frames. Check that
  `vrcompositor.txt` logs 120 at start-up and Home's stats target 120.
- The OpenVR shim traces PostPresentHandoff, and with
  `MWXR_OPENVR_SHIM_HANDOFF=1` calls it after both eyes are submitted. Its
  fixture passes. Run it as a separate A/B after the frequency change, since
  Home may still draw its desktop window after Submit.

Compare Home's cycle, WaitGetPoses duration, ownership holds and driver/
compositor acquire timeouts against this run.

That 90 Hz run is a second baseline with the shim tracing PostPresentHandoff:
Home made no PostPresentHandoff calls. The shim's five-second "calls/s" counts
each frame twice, because Home's IVRCompositor_024 and _025 wrappers nest;
the figures below merge nested calls from the trace.

### Refresh-rate A/B — 2026-10-13

Both runs: isolated `build/arm64` service at `6393b6592`, installed LaunchAgent
environment, shim tracing, handoff injection off, Home only. Steady state
excludes the first 60 s. One run each, with different session lengths and
Home activity, so this is indicative rather than a controlled benchmark.

| | `232237`, 90 Hz start | `233833`, 120 Hz start |
| --- | ---: | ---: |
| SteamVR compositor frequency | 90 | 120.000008 |
| Home frames/s (steady) | 25.0 | 34.9 |
| Frame period p10 / p50 / p90 | 24.3 / 34.4 / 54.6 ms | 16.7 / 25.1 / 34.1 ms |
| Periods > 50 ms | 694 | 229 |
| WaitGetPoses p50 / p90 | 20.5 / 36.9 ms | 19.4 / 25.1 ms |
| SteamVR ApplicationTime CPU / GPU | 10.82 / 1.58 ms | 8.04 / 0.95 ms |
| SteamVR "Timed out" presents | 173 | 272 |
| Driver vsync-to-photons | up to 75 ms | 42–67 ms typical |

Starting SteamVR at the real refresh rate is an improvement: more frames,
fewer long gaps, a tighter wait tail. Home still runs at a median of three
refreshes per frame. Its measured CPU time (8.04 ms) sits at the 8.33 ms
budget, while its GPU time is about 1 ms. That CPU time runs from
WaitGetPoses returning until the next WaitGetPoses, because Home never hands
off, so it includes Home's post-Submit work. The driver still logged 100 ms
source-acquire failures (empty frames, seen as black) at 23:39:41, 23:40:03,
23:40:06, 23:40:15 and 23:40:32; the log samples after five. The next run
adds `MWXR_OPENVR_SHIM_HANDOFF=1` with the same refresh-rate fix.


### Handoff A/B — 2026-10-13

Run `234437` repeats `233833` (120 Hz start) with `MWXR_OPENVR_SHIM_HANDOFF=1`.
The shim injected about 40 handoffs per second, one per frame; Home made no
calls of its own. It ran for about 2.5 minutes (76 s steady), shorter than
`233833`. Wine ownership history was not captured, so mutex holds are not
compared.

| | `233833`, no handoff | `234437`, injected handoff |
| --- | ---: | ---: |
| Home frames/s (steady) | 34.9 | 34.4 |
| Frame period p10 / p50 / p90 | 16.7 / 25.1 / 34.1 ms | 23.2 / 25.1 / 36.0 ms |
| Periods > 50 ms (per second) | 229 (1.16) | 108 (1.42) |
| WaitGetPoses p50 / p90 | 19.4 / 25.1 ms | 17.6 / 21.7 ms |
| SteamVR ApplicationTime CPU / GPU | 8.04 / 0.95 ms | 5.07 / 1.07 ms |
| SteamVR "Timed out" presents | 272 | 76 |
| Driver 100 ms acquire failures logged | 5+ | 0 |
| vrcompositor WaitForAcquire warnings | 26 | 35 |
| Driver vsync-to-photons | 42–67 ms | 17–58 ms |

The handoff does what it should for SteamVR's accounting: Home's measured CPU
time falls by about 3 ms, and this shorter run logged no driver acquire
failures, the empty-frame (black) path. It does not change Home's cadence:
still a median of three refreshes per frame, with about 18 ms of each spent
in WaitGetPoses. So the CPU budget was not what held Home at three refreshes.
The remaining suspect is the pacing loop through Monado: the driver's OpenXR
frame spans SteamVR's whole Home-gated frame, Monado predicts display 5–8
refreshes ahead, and SteamVR schedules Home's wake from that prediction.

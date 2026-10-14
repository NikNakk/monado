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

### Monado's app pacer holds the driver at three refreshes — 2026-10-14

Monado's `app_pacing` timing trace (written with `PSVR2_TIMING_TRACE=1`,
which isolated runs inherit from the LaunchAgent) has one session, the
bridge driver. Excluding the first 60 s, the pacer's period for it is mostly
three refreshes:

| Run | Pacer period, refreshes (frames) | Monado "GPU" p50 / p90 | "Draw" p50 / p90 |
| --- | --- | ---: | ---: |
| `232237`, 90 Hz start | mostly 3–5 | 27.5 / 40.9 ms | 17.6 / 34.1 ms |
| `233833`, 120 Hz | 3: 5206, 2: 1341, 4: 505 | 18.4 / 23.8 ms | 8.6 / 21.9 ms |
| `234437`, + handoff | 3: 2621, 4: 219, 2: 99 | 19.1 / 24.1 ms | 6.5 / 24.4 ms |

Monado's "GPU" time runs from the driver's xrEndFrame until the frame's
GPU fence signals; "draw" runs from xrBeginFrame to xrEndFrame. SteamVR
reports about 1 ms of GPU work for both Home and its compositor, yet the
driver's frames take 18–19 ms to signal. Draw plus GPU is about 25–27 ms, so
the pacer gives the driver a three-refresh slot, which is Home's cadence.
The driver's copy fence follows the GPU wait on SteamVR's sync texture, so
a late release signal from DXMT would explain it. Not yet attributed.

The bridge driver now has an opt-in frame trace for this:
`MWXR_DRIVER_FRAME_TRACE=1` writes the run's `driver-frames.csv` with each
OpenXR frame's CPU stages (wait, begin, destination preparation, source
acquire, sync release, xrEndFrame), ID3D11Fence timestamps after the source's
keyed-mutex GPU wait and after the copies, and each vsync event. The fence
path was checked against the rig's DXMT in a scratch prefix.
[`analyze-steamvr-driver-pacing.py`](../scripts/macos/analyze-steamvr-driver-pacing.py)
joins it with `app_pacing.csv`. Monado's XrTime is its monotonic clock minus
a per-instance constant, and Wine's QPC is not on Monado's epoch here
(about 5 s apart), so frames are matched by exact predicted display time;
a cadence too regular to align is refused.
[Its fixture](../tests/test_macos_steamvr_driver_pacing.py) covers the
alignment, stage arithmetic and refusal cases.

Next run (handoff kept on, as it removed acquire failures):

```sh
MWXR_MONADO=isolated MONADO_SERVICE_BUILD=$HOME/Code/monado-2/build/arm64 \
  MWXR_OPENVR_SHIM_INSTALL=1 MWXR_OPENVR_SHIM_TRACE=1 MWXR_OPENVR_SHIM_HANDOFF=1 \
  MWXR_OPENVR_SHIM_FIX_POSE=0 MWXR_DRIVER_FRAME_TRACE=1 ~/Windows/run-mwxr.zsh
python3 scripts/macos/analyze-steamvr-driver-pacing.py \
  ~/Windows/logs/<run>/driver-frames.csv ~/psvr2-trace/monado_psvr2_<pid>_app_pacing.csv
```

If `gpu_source_after_acquire` carries the 18 ms, the delay is the GPU wait for
the previous owner's release (DXMT/Wine signalling); if
`gpu_copies_after_source` does, it is the copy or its flush; if neither and
`monado_gpu_done_after_driver_copies` is large, it is in Monado's fence wait.

### Driver frame trace: the delay is the GPU wait on the sync texture — 2026-10-14

Run `run-mwxr-20261014-000314` (120 Hz start, handoff injection, driver
frame trace; Monado trace `monado_psvr2_37798_app_pacing.csv`). The analyser
aligned 7377 steady-state frames (Monado id = driver frame + 8); Monado's
draw time matches the driver's begin-to-end time (p50 5.50 vs 5.53 ms),
confirming the join.

| Stage (after the first 60 s) | p50 | p90 |
| --- | ---: | ---: |
| Driver CPU: source acquire | 0.12 ms | 0.83 ms |
| Driver CPU: copies and sync release | 0.25 ms | 0.73 ms |
| GPU: source wait passed, after the CPU acquire | **17.4 ms** | 20.1 ms |
| GPU: copies done, after that | 0.43 ms | 1.0 ms |
| Monado sees the fence, after the copies | 2.0 ms | 2.9 ms |
| Monado "GPU" time | 17.5 ms | 20.2 ms |
| Pacer period, refreshes | 3: 5235, 2: 1782, 1: 186 | |

DXMT's AcquireSync returns on the CPU at once and queues a GPU wait for the
previous owner's release value; each ReleaseSync queues a GPU signal after
that owner's earlier work, then flushes. So the driver's wait covers
vrcompositor's release, which itself waited on Home's. Joined with the shim
trace, the driver's source wait completes 13.3 / 17.0 / 19.4 ms (p10/50/90)
after Home's injected handoff, a few ms before Home's next Submit. Home
releases promptly on the CPU, but the release takes about 17 ms to reach the
driver on the GPU, while SteamVR reports about 1 ms of GPU work for Home.

The signal times show no concentration at the built-in display's 60 Hz
(resultant length 0.01), so a 60 Hz drawable wait is not indicated. Their
moderate lock to the 120 Hz refresh (0.38) is inherited from the driver's
xrWaitFrame pacing. Not yet known: whether Home's or vrcompositor's command
buffer carrying the release signal is committed late, scheduled late, or
waits behind other work. A Metal System Trace of a Home session would show
the commit and execution times of those command buffers directly.

### Metal System Trace: Home is GPU-bound, and DXMT's timestamps hide it — 2026-10-14

User-recorded `/tmp/steam-vr-home.trace` (MonadoInstrumentsTemplate, all
processes, 61 s, during `run-mwxr-20261014-001027` with the same settings as
the previous run). Top-level Metal GPU intervals, union per process:

| Process | Command buffers/s | GPU busy | Submit → GPU start p50 / p90 |
| --- | ---: | ---: | ---: |
| Home (`steamtours.exe`) | 133 | **628 ms/s** | 5.0 / 19.1 ms |
| Monado service | 197 | 172 ms/s | 0.22 / 1.24 ms |
| vrserver (bridge driver) | 28 | 8 ms/s | 22.4 / 26.3 ms |
| vrcompositor | 80 | 0.6 ms/s | 23.3 / 31.9 ms |

Home uses about 18 ms of GPU per frame at its ~35 frames/s, 68% of it
fragment shading; its largest command buffer per frame takes 19.7 ms at p90.
The driver and vrcompositor submit almost nothing but wait about 22 ms for
the sync texture behind Home's work. So the 17 ms the driver trace attributed
to the sync-texture wait is Home's own GPU time, and the pacer's three-refresh
slot follows from it (two refreshes are 16.7 ms).

SteamVR reported about 1 ms of GPU for Home because DXMT 756a657 resolves
`D3D11_QUERY_TIMESTAMP` with `TimestampReadbackCBuf`: every timestamp in a
command buffer is that buffer's `gpuEndTime`, so begin/end pairs within one
buffer read nearly zero. Source 2's adaptive fidelity sees the same
near-zero GPU time and raises resolution: SteamVR recommended 1252×1276 per
eye (render target scale 0.20 from its 333 MP/s GPU speed measurement), but
Home rendered up to 1770×1804, about twice the pixels.

Earlier "Home ownership" holds and keyed-mutex timeouts in these notes are
consistent with this: Home's GPU-side release follows its own long GPU work.
No synchronisation defect is indicated for the steady state. Next:
confirm by starting Home with adaptive fidelity off at the recommended
resolution, and fix DXMT's timestamp queries (sample-buffer timestamps at
encoder boundaries) so that Source 2, SteamVR and other D3D11 games see real
GPU times.

### Fixed Home fidelity confirms the diagnosis — 2026-10-14

SteamVR's `tools.vrmanifest` now starts Home with `-condebug
+vr_fidelity_level_auto 0 +vr_fidelity_level_spew 1` (original saved as
`~/Windows/backups/tools.vrmanifest.before-fidelity-20261014`; a SteamVR
update may restore it). Home's console log shows its fidelity table comes
from `steamtours/pak01.vpk:vr_fidelity_levels.csv` with the `nvidia` profile,
default level 3 of 9, and a top level of 8x MSAA at 1770×1804. With auto
off it stayed at level 3, 1248–1252 × 1272–1276 per eye (SteamVR's
recommendation), in run `run-mwxr-20261014-002745` (same settings as
`000314`; the user reports it looks better).

| | `000314`, adaptive | `002745`, level 3 fixed |
| --- | ---: | ---: |
| Monado pacer period (refreshes: frames) | 3: 5235, 2: 1782 | 2: 3620, 3: 1554 |
| Home frames/s (steady) | ~35 | 47.6 |
| Home frame period p10 / p50 / p90 | — | 15.9 / 17.2 / 26.0 ms |
| Driver GPU source wait p50 / p90 | 17.4 / 20.1 ms | 13.5 / 23.2 ms |
| Driver 100 ms acquire failures | 0 | 1 |
| SteamVR ApplicationTime CPU / GPU | 5.07 / 1.07 ms (`234437`) | 3.98 / 0.94 ms |

Home moves from three to two refreshes per frame. At level 3 it still needs
about 13 ms of GPU per frame, so one refresh (8.3 ms) is out of reach without
a lower level. The proper fix is DXMT timestamps that measure GPU time, so
Source 2's auto-fidelity and SteamVR's timing work as on Windows.

### Home's own GPU timings under-read — 2026-10-14

DXMT branch `claude/timestamp-queries` (worktree `~/Code/dxmt-timestamp`:
756a657, the installed diagnostics patch, then `df4e91d`) resolves D3D11
timestamp queries from Metal counter samples instead of each command
buffer's `GPUEndTime`. Its fixture under Wine agrees with the old readback
except where work is queued before the first query in the same command
buffer (render-target clears: old 2.885 ms, new 6.582 ms, for about 7 ms of
work). Not yet installed.

Run A, `run-mwxr-20261014-004534`, kept the installed DXMT and turned Home's
auto-fidelity back on (`+vr_fidelity_level_auto 1`, with spew and
`-condebug`). Home's console log shows its GPU frame-time readings are
mostly 0.3–2.7 ms, against about 18 ms measured by Metal System Trace. It
climbs a level every five frames to level 8 (8x MSAA, 1770×1804). Occasional
correct readings (8–30 ms) drop it a few levels, after which the next
sub-millisecond readings take it straight back to 8. Run B, with the new
`d3d11.dll` installed, is next.

Run B, `run-mwxr-20261014-004923`, with `df4e91d` installed (backup and
hashes in `~/Windows/backups/dxmt-756a657-diag/`): Home's readings are
unchanged, mostly 0.22–0.28 ms, and it still returns to level 8 within a
second of each drop. So the counter-sample fix is not reaching Home's
measurement, or Home's GPU time does not come from its own timestamp
queries. Home's DXMT log shows two D3D11 devices, feature levels 11_1 and
11_0; the second is probably SteamVR's client library's, whose queries
would run on a separate Metal queue and could not see Home's work.
`4641e69` adds logging of timestamp query sources and readback outcomes and
is installed for the next run.

### The cause: deferred-context timestamps sampled before the work — 2026-10-14

Run C, `run-mwxr-20261014-010446`, with the diagnostic build `4641e69`:
all of Home's timestamp queries come through one immediate context on its
feature level 11_0 device (whose command buffers carry its rendering), and
every readback resolved from a counter sample, with no fallbacks. Home
still read 0.22–0.29 ms. Readbacks such as one sample for four queries, and
pairs inside command buffers with 12–57 µs of GPU work, pointed at the
queries' placement rather than their resolution.

DXMT's deferred context only listed `D3D11_QUERY_TIMESTAMP` queries;
`ExecuteCommandList` ended them all on the immediate context before
replaying the list. Source 2 records its frame in deferred contexts, so its
begin/end pairs always sampled the same point ahead of the work. DXMT
`b18b48a` records a sample op at each query's place in the command list, as
occlusion queries already do. The fixture's deferred cases read 0.000 ms for
10 and 40 render-target clears without it and 4.170 / 17.473 ms with it
(5.799 / 19.464 ms flush-to-event). Installed; SteamVR and Steam need a
restart to load it.

### Home reads SteamVR's total render GPU — 2026-10-14

Further DXMT changes on `claude/timestamp-queries`, installed in turn:
`b18b48a` samples deferred-context timestamps at their place in the command
list (fixture: 0.000 ms before, 4.170 / 17.473 ms after, for 10 / 40
clears); `198540b` logs encoder sequences; `afee1cb` makes each sample also
wait for the previous command buffer's end signal. None changed Home's
sub-millisecond readings in the first minute of runs E–G.

The encoder log shows Home's frame as `render, S, W, "L M P", "M S B M S"` on
one queue: its timestamps sit in small command buffers around Submit and
present, not inside the rendering. The OpenVR shim's new frame-timing log
(`MWXR_OPENVR_SHIM_FRAME_TIMING=1`, run H `run-mwxr-20261014-071827`)
shows why: Home's auto-fidelity "last frame" equals SteamVR's
`m_flTotalRenderGpuMs` (07:20:16, 5.15 ms in both), while SteamVR's pre- and
post-submit GPU times for the app are always 0.00. Total render GPU is
vrcompositor's GPU time plus 0.2 ms, measured in vrcompositor's process.

In run H that figure rose from 0.4–0.9 ms to a median of 5.1 ms after about
two minutes, and Home settled at levels 2–4, cycling every few frames, at
about 100 frames/s after 60 s. Run G (same DLL, no frame-timing calls) kept
Home at level 8 at 75–81 frames/s. These runs followed about five hours of
sleep, so frame rates are not comparable with the overnight runs (35–48
frames/s at level 8 then) without thermal control. Not yet known: why the
figure rose in run H, whether the DXMT changes in vrcompositor or the shim's
calls matter, and how vrcompositor's timing spans Home's work across
separate Metal queues.

### SteamVR's client measures Home's GPU time but it never arrives — 2026-10-14

Run I (`run-mwxr-20261014-072947`, DXMT `4e179d7` diagnostics) logged each
GetData in a window of Home's timing queries. Per frame there are three
timestamps and a disjoint query, double-buffered and read one frame later:
A after WaitGetPoses returns, B after the second Submit, C inside
PostPresentHandoff (which only SteamVR's client sees, as the shim injects
it), so these are SteamVR's client `CGpuTiming` queries (strings in
`vrclient_x64.dll`; `gputiming_d3d11.cpp` in vrcompositor). With the DXMT
fixes their values are right: B − A = 11.9–13.8 ms, C − B ≈ 0.13 ms. About
half the reads found C or the disjoint not yet ready.

Yet SteamVR's frame timing reports pre- and post-submit GPU as exactly 0.00,
total render GPU is vrcompositor's own GPU time plus 0.2 ms (0.3–0.5 ms in
run I), and Home's auto-fidelity, which follows total render GPU, returned
to level 8. Run H's 5 ms figure was vrcompositor's own GPU time rising,
probably from contention with Home, not a measurement of Home.

Run J tested readiness: a temporary DXMT change made not-ready timing reads
flush and wait up to 20 ms. More than 8,000 waits all completed in time,
but pre-submit GPU stayed 0.00. So SteamVR discards or never uses its
client's correct measurements for another reason, without logging it. The
experiment was dropped from the branch; `4e179d7` is installed again.

The DXMT branch still holds two fixes confirmed by the fixture
(`df4e91d` counter samples, `b18b48a` deferred-context placement) and one
precaution (`0ebe00b`, wait for earlier command buffers). They correct D3D11
timestamps for any title, but do not by themselves fix SteamVR Home, whose
resolution follows SteamVR's figures. The fixed-fidelity workaround remains
the dependable setting.

### Why SteamVR's pre-submit GPU time is zero — 2026-10-14 (in progress)

Reverse-engineering `vrclient_x64.dll` (the installed SteamVR build) with
`objdump`, plus read-only probes from the OpenVR shim, gives:

- `CGpuTiming::GetDeltas` (`0x1802d21d0`) reads a disjoint query and N
  timestamps through `ID3D11DeviceContext::GetData` (flags 0, spinning to a
  deadline) and writes `(tsᵢ − tsᵢ₋₁) / Frequency × 1000` ms; missing
  results, a zero frequency or a disjoint frame give 0.
- The compositor client (vtable RVA `0x3b4db8`; the only instance is the
  global at RVA `0x49cd98`) calls it through `0x180143f50`, writing pre- and
  post-submit GPU ms to `+0x130c`/`+0x1310`, which go into the per-frame
  message to vrcompositor at `+0x1f0`/`+0x1f4`.
- In Home, that client's `CGpuTiming` (at `+0xd68`) is constructed but never
  initialised: its device slot (`+0xe00`) stays null, its frame and query
  counts stay 0, and so do pre- and post-submit. A memory scan found no
  other instance. So SteamVR never measures Home's GPU time; the A/B/C
  queries seen earlier belong to some other timer.
- Initialisation (`CGpuTiming::Init`, `0x1802d2610`, 3 queries, "Compositor
  Client") happens in the shared-texture Submit path, function `0x180153d20`
  in `vrcompositorsharedtextures.cpp`. For the D3D11-style path (internal
  code 3/4) it should run on the first Submit, unless an earlier exit is
  taken. The function's "Setting max texture dimensions …" log line, which
  precedes one initialisation site, never appears in `vrclient_steamtours.txt`,
  while its earlier "Found Windows 10 or newer…" line does. Which exit is
  taken is not yet identified.

The shim's probe code for this is uncommitted, with hard-coded RVAs for this
SteamVR build only.

### Root cause and fix: Home was classified as a D3D10 app — 2026-10-14

A one-shot breakpoint trace of the shared-texture Submit function (648
basic blocks; method and patch in the bridge's
`docs/results/steamvr-gpu-timing-20261014/`) showed Home's Submit taking
the internal API code 2 branch, which creates "Scene … D3D10" textures. The
classifier (`vrclient_x64.dll` `0x18014eec0`) first asks a DirectX texture
for `ID3D10Texture2D` and, if that works and the texture has a device,
treats the app as D3D10; only otherwise does it try `ID3D11Texture2D`. DXMT's
D3D11 resources answered every D3D10 interface, unlike Windows, and the
D3D10 path never initialises the client's GPU timing. Hence pre-submit GPU
0.00, total render GPU without Home's work, and auto-fidelity at level 8.

DXMT `8624164` exposes D3D10 interfaces only on resources whose device was
created through `D3D10CoreCreateDevice` (`GetDirectXVersion() == 10`); its
fixture shows a D3D11 texture refusing them (previously accepted) and a D3D10
texture keeping them. Run `run-mwxr-20261014-083339` with it installed:

| | Before (`082539` etc.) | With `8624164` |
| --- | ---: | ---: |
| SteamVR pre-submit GPU | 0.00 | 5.8–8.0 ms |
| SteamVR total render GPU | 0.3–0.5 ms | 6.4–8.5 ms |
| Home auto-fidelity | climbs to level 8 | settles at level 0 within a second |
| Home frames/s after 60 s | 75–81 (cool machine) | 114.3 |
| Home frame period p10/50/90 | — | 7.5 / 8.5 / 11.1 ms |

Home now holds about one refresh per frame at 120 Hz, at the lowest fidelity
level, because its GPU frame at level 0 (6–8 ms) nearly fills the 8.33 ms
budget. One run, not thermally controlled. The installed `d3d11.dll` is the
`claude/timestamp-queries` branch including its diagnostic logging commits;
which of the timestamp fixes (`df4e91d`, `b18b48a`, `0ebe00b`) this result
also depends on has not been isolated. The fixed-fidelity manifest arguments
are no longer needed; Home's arguments still carry auto-fidelity spew and
`-condebug` for observation.

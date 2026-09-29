<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# macOS: client-side compositing for Game Mode

**Status:** design, 2026-09-29. Nothing here is implemented yet. The evidence
comes from `macos-layer-host-probe`; see
[`macos-remote-layer-hosting.md`](macos-remote-layer-hosting.md) for every
measurement cited below.

## Problem

When a game has Game Mode, macOS backgrounds `monado-service` from outside the
process (`ext_darwinbg=1`: RunningBoard or gamepolicyd). Every thread in the
service is clamped to priority 4 and moved to E-cores. That includes the
compositor, the IPC threads and the PS VR2 driver threads. XPC importance and
`ProcessType` cannot undo an external background request, so the XPC
importance lease, the captured-context adoption and the `Adaptive` default do
not help.

The only processes Game Mode favours are the game and its coalition. Anything
the headset needs frame by frame must therefore run in the game's process.

## Evidence

From the probe (PS VR2, 119.88 Hz):

| Question | Result |
| --- | --- |
| Does Game Mode throttle a LaunchAgent host? | Yes: 97 → 4 as soon as the game is fullscreen, `ext_darwinbg=1`, for the whole session. |
| Can the throttled host still render? | No: 23 frames submitted in 58 s under load. |
| Can the game render to the headset through the host? | Yes: through `CAContext`/`CALayerHost`, 0.51 % long intervals and 15 ms latency with the host throttled throughout. |
| Does cross-process hosting cost anything? | Not measurably: same intervals and latency as a direct window (unloaded). |
| Can clients be handed off seamlessly? | Yes: every switch is one refresh period. |
| Does the handoff depend on the host? | Client-driven visibility makes the switch independent of it. Leaving two hosted layers unhidden costs about a frame, so the hybrid protocol below has the host arm and tidy. |

### Real service under Unreal with Game Mode (2026-09-29)

A background `--role query` log during an Unreal session (service pid 15173,
game pid 15044, 2 s samples):

- Windowed: service `ext_darwinbg=0 adaptive=1 adaptive_important=1`, game
  `game_mode=off`.
- Fullscreen from 23:59:27: game `game_mode=on`, service `ext_darwinbg=1`,
  **while still `adaptive_important=1`**. The service held an importance boost
  the whole time and was backgrounded anyway. This is direct confirmation that
  an XPC importance boost cannot override the external background request.
- The service returned to `ext_darwinbg=0` within 2 s of the game exiting.

About 21 s into Game Mode, Unreal aborted. `xrWaitSwapchainImage` →
`ipc_call_swapchain_wait_image` returned `XRT_TIMEOUT` ten times in about 1 s,
and Unreal's OpenXR plugin treats that as fatal ("Failed to wait on acquired
swapchain image"). The wait is a round trip to the throttled service. So
throttling does not only cause judder: any per-frame IPC call can stall long
enough to kill the app. That is further reason why swapchain waits, frame
timing and pose queries all have to become local to the client.

**Interim workaround.** Game Mode is opt-out per app through its `Info.plist`.
Setting `LSSupportsGameMode` (and `GCSupportsGameMode`, which the probe's game
bundle sets to true) to false in a VR build should keep macOS from engaging
Game Mode, and so from backgrounding the service. Verify with the same policy
log: the game should stay at `game_mode=off` in fullscreen.

## Architecture

Today:

```text
game process                          monado-service (throttled under Game Mode)
------------                          -------------------------------------------
OpenXR state tracker                  multi-client compositor (comp_multi)
Metal client compositor  -- IPC -->   main compositor (Vulkan distortion/timewarp)
xrWaitFrame, poses        -- IPC -->   comp_window_macos -> CAMetalLayer -> PS VR2
                                      PS VR2 driver (USB, SLAM, IMU)
```

Proposed:

```text
game process (Game Mode, unthrottled)        monado-service
-------------------------------------        --------------
OpenXR state tracker                          device drivers (PS VR2 USB/SLAM/IMU)
in-process system compositor                  publishes tracking into shared memory
  comp_multi + comp_main (Vulkan)             headset NSWindow: one CALayerHost per client
  hosted presenter: CAMetalLayer in a         handoff controller (arm / tidy)
  CAContext, id sent to the service           session, focus and lifecycle management
poses read from shared memory      <- shm --  own compositor for service-rendered content
```

In short: each client composites and presents its own frames. The service keeps
the headset window and hosts each client's layer. The service stays in charge of
devices, sessions and focus, but nothing per-frame waits on it.

## Components

### 1. In-process compositor in the client

`ipc_client_instance.c:create_system_compositor()` currently calls
`ipc_client_create_system_compositor()`, which forwards every compositor call
to the service. On macOS, when hosted compositing is available, it instead
creates the same stack the in-process build uses (`target_instance.c` calls
`comp_main_create_system_compositor(head, …)`), with `head` being the IPC HMD
device:

- `comp_main` (Vulkan/MoltenVK distortion and timewarp) and `comp_multi`
  (app pacing, layer latching) run in the client. The existing per-frame IPC
  (`compositor_predict_frame`, `compositor_wait_frame`, `layer_sync`, swapchain
  import) disappears for this client.
- Swapchains are native to the client. The Metal client compositor wraps
  `comp_main` directly, and the IOSurface import happens in-process, with no
  Metal XPC broker.
- Distortion comes from `xrt_device_compute_distortion()` over IPC, once, when
  the distortion mesh is built.
- The compositor thread keeps the existing time-constraint policy (35/70 % of
  the period). In the game's process that is honoured (probe: priority 97
  throughout).

The in-process compositor itself is already proven on macOS: the
`-DXRT_FEATURE_SERVICE=OFF` build (direct Metal swapchains) has been run on
hardware and is built in CI (see `macos-openxr-foveation.md`). What is new is
combining it with IPC devices in one build. Today `XRT_FEATURE_SERVICE` chooses
at compile time between an IPC runtime and a fully in-process one (including
the device prober). The hosted build needs both at run time: devices, sessions
and focus from the service, with `comp_main` and the direct Metal swapchain path
linked into the runtime library and created in the client.

### 2. Hosted presenter (client side)

`comp_window_macos.m` currently creates the `NSWindow` and owns the
presentation pipeline: CVDisplayLink pacing, the Vulkan → Metal shared-event
handoff, the single pending present job, the drawable slot and
`afterMinimumDuration`. Split it into:

- **presenter core**: everything from the Metal textures to
  `presentDrawable`, independent of where the layer lives;
- **window target** (today's behaviour, used by the service and as fallback);
- **hosted target**: creates the `CAMetalLayer` inside a container layer on a
  `CAContext` (`contextWithCGSConnection:CGSMainConnectionID() options:@{}`),
  sends the `contextId` to the service, and shows or hides the container on
  handoff commands.

The client uses the headset's `CGDirectDisplayID` (from the service) for its
CVDisplayLink, so pacing is unchanged.

### 3. Display host (service side)

The service keeps creating the headset `NSWindow` (same configuration as now).
Its root layer holds:

- one `CALayerHost` per attached client, hidden unless that client is shown;
- the service's own `CAMetalLayer`, used when the service itself renders
  (launcher/home content through today's path, or no client attached).

The service's own compositor stops rendering while a client owns the display.

### 4. Handoff protocol (hybrid)

The service decides focus, so it starts each switch, but the switch itself is
done by the clients. The ordering keeps the display valid even when the service
is late.

1. **Arm (service):** unhide the incoming client's `CALayerHost` (its content is
   still hidden, so nothing visible changes), then signal "show *k*".
2. **Show (incoming client):** show its container, wait until one of its frames
   has been presented, then signal "shown *k*".
3. **Hide (outgoing client):** on "shown *k*", hide its container, then signal
   "hidden *k*".
4. **Tidy (service):** on "hidden *k*", hide the outgoing `CALayerHost`, unless
   a newer switch has re-armed it.

The incoming client never shows before the arm is committed. The outgoing client
never hides before the incoming frame is on screen. A late service therefore
delays a switch or the tidy, but can never blank the display. Measured: switches
are one refresh period, the client steps take 30–40 ms, and the arm and tidy are
0.1 ms when the service can run.

**Signalling.** The probe used sockets. In Monado, the signals go through the
shared memory the service already maps read-write into every client
(`struct ipc_shared_memory`, `ipc_client_connection.c`). A new block holds
atomic generation counters:

```c
struct ipc_display_handoff
{
	_Atomic uint64_t arm_generation;   // written by the service
	_Atomic uint32_t incoming_client;  // written by the service with arm
	_Atomic uint64_t shown_generation; // written by the incoming client
	_Atomic uint64_t hidden_generation;// written by the outgoing client
};
```

Each client's presenter checks the block once per vblank, so no extra thread or
wakeup is needed and reactions land within a frame. The service checks
`hidden_generation` from its main loop to tidy; lateness there only costs
latency (two hosted layers unhidden for longer).

**New IPC calls** (`proto/50-compositor.json`):

- `compositor_hosted_attach(context_id)`: the client registers its `CAContext`;
  the service creates the `CALayerHost` (hidden).
- `compositor_hosted_detach()`: on session end; the service removes the host.

Focus policy itself (which client is shown) is unchanged: it is the existing
multi-client active/focused logic, which now drives an arm instead of a layer
merge.

### 5. Tracking without round trips

`ipc_client_xdev_get_tracked_pose()` is a synchronous IPC call
(`ipc_call_device_get_tracked_pose`). With compositing in the client, both the
compositor's timewarp and the app's `xrLocateViews` would wait on a throttled
service thread every frame. Unix-socket IPC gives no priority inheritance, and
Mach turnstile boosts are clamped for externally backgrounded tasks, so the wait
can be long. Moving the compositor alone is not enough.

The service instead publishes tracking into shared memory, and the client
predicts locally:

- The PS VR2 driver writes each fused SLAM pose and the IMU samples since then
  into a lock-free ring in shared memory (sequence counter, device timestamps).
- The client runs the same prediction the driver runs today (SLAM pose
  integrated forward through the IMU samples, then extrapolated) over that ring.
  `get_tracked_pose` for the HMD becomes a local read.
- Other devices keep the IPC path initially.

**The driver's clock mapping hides delivery delay.** The VTS-to-host offset
(`hw2mono_vts`) comes from `m_clock_offset_a2b()`, an exponential filter over
receipt times with a time constant of about 80 ms. A sustained delivery delay is
absorbed into the offset within a fraction of a second. The driver then treats
late samples as fresher than they are, and the delay becomes pose lag. It also
disappears from the trace's "mapped" columns. A shared-memory ring should carry
raw device timestamps, and the client should map them with an estimator anchored
on the fastest deliveries, such as a lower envelope, not a mean.

**Open risk: the driver threads are throttled too.** USB completion and the SLAM
and IMU reader threads run in the service at priority 4 under Game Mode. Device
timestamps keep prediction correct, but samples may reach shared memory late,
which lengthens the prediction horizon. This needs measuring first (see phase 1).
If it is bad, the options are:

- run the PS VR2 USB reader inside the focused client, as GAV's player does
  (one process owns the headset at a time, and the service hands it over on
  focus change);
- or read USB in a small helper process spawned by the client, so it inherits
  the game's coalition.

### 6. Pacing and latency

- **One render per vblank for the newest target.** Monado's presenter already
  keeps a single pending present job, so it does not have the probe's
  counting-semaphore burst. Keep it that way in the hosted target.
- **Latency guard.** GAV's player drops one frame when presents have been two
  or more periods late for a sustained time. The probe showed the same
  start-up offset (24 ms instead of 16 ms until the first reset). Add the guard
  behind an option, and decide its default from Monado's own timing traces over
  long sessions.

### 7. What moves where

| Area | Plan |
| --- | --- |
| Projection, quad and other layers of the focused client | Client |
| Distortion, timewarp, depth reprojection | Client (same `comp_main` code) |
| Foveation (`XR_FB_foveation`, runtime-owned gaze) | Client; gaze from the device over IPC, not per frame |
| Passthrough (`XR_FB_passthrough`) | Stays on the service path at first: the camera frames live in the service. Later, share them as IOSurfaces. |
| Launcher/home and service-rendered content | Service, through its own layer |
| Overlay clients over a game | Phase 3 (below) |
| Refresh-rate switching | Service (owns the display mode); clients request it over the existing IPC |
| Wine clients | Service path at first; hosting needs a `CAContext` inside the Wine process, to be verified |
| Chromium WebXR | Service path at first; its GPU process already creates `CAContext`s, so likely feasible later |

### 8. Fallbacks

- **Private API missing.** Check it as Chromium's `RemoteLayerAPISupported()`
  does (class and selector checks). If it fails, use today's service-composited
  path and log once.
- **Kill switch.** `XRT_MACOS_CLIENT_COMPOSITOR=0` forces the service path.
- **Client crash.** Its `CAContext` disappears and its host layer shows nothing.
  The service sees the IPC disconnect and shows its own layer. The game has gone,
  so Game Mode ends and the service is no longer throttled. Still worth
  measuring the gap, and whether an opaque client layer stacked over a visible
  service layer keeps direct scanout. If it does, the service layer can stay
  underneath permanently as an instant fallback.

## Clean-up once this works

- Remove the XPC importance lease (`XRT_MACOS_XPC_IMPORTANCE`), the captured XPC
  context adoption (`os_macos_xpc_context_*`) and the `Adaptive` `ProcessType`
  default. They address a cause (adaptive-daemon background) that Game Mode
  does not use.
- The process-activity and external-broker diagnostics are already gone.

## Phases

**Phase 1: measure the tracking path under Game Mode.**
Record the service's PS VR2 traces (`PSVR2_TIMING_TRACE=1`) while Unreal runs
windowed, then fullscreen with Game Mode, then windowed again. Analyse them with
`scripts/psvr2_delivery_delay.py`. See [Running phase 1](#running-phase-1).
*Decides:* whether tracking can stay in the service (shared-memory ring) or must
move into the client.

**Phase 2: single focused client, hosted.**
Presenter split, hosted target, in-process compositor for the focused client,
`compositor_hosted_attach`. Poses stay on IPC (phase 1 showed tracking delivery
is unaffected); the shared-memory pose ring moves to phase 4. The service shows the
client's host layer when the session becomes visible and hides it at session
end, with no mid-session handoff yet.
*Accept:* with Unreal and Game Mode, the client compositor thread stays at 97,
presents keep the unloaded baseline (about 0.5 % long intervals, 16 ms latency),
and there is no regression without Game Mode.

**Phase 3: handoff.**
The shared-memory handoff block, arm and tidy in the service, show and hide in
the client presenter. Handoff between the launcher (service layer) and a game,
and between two hosted clients.
*Accept:* one-period switches, no blank frames, and a throttled service only
delays the start of a switch.

**Phase 4: overlays and the rest.**
Two options for overlay clients over a game:

- the focused client composites the overlay's layers, read from shared memory
  and IOSurfaces, which is today's multi-client merge moved into the client;
- or each client composites its own distorted layer with alpha, and
  WindowServer blends the stacked host layers. That is simpler, but the probe
  measured about one extra frame of latency with two visible host layers.

Then passthrough frame sharing, Wine and Chromium.

## Risks and open questions

- **Tracking under throttling** (phase 1). This is the largest unknown.
- **Private API stability.** `CAContext`/`CALayerHost` have no documentation or
  compatibility promise; Chromium and WebKit depend on them. Mitigated by the
  runtime check, the kill switch and the fallback path.
- **GPU contention.** The client compositor's Vulkan work now shares the game's
  GPU queue priority, rather than the service's. That is probably better under
  Game Mode, but needs measuring with a heavy game.
- **Per-process cost.** Every OpenXR client carries a compositor: memory,
  shader compile at session start, and MoltenVK start-up.
- **Composition-mode hitches at handoff.** The hybrid method showed one
  repeated frame around some switches. That is acceptable for focus changes,
  but worth checking with the real presenter.

## Running phase 1

**What is measured.** `received_ns` is the host clock read in the driver's
libusb transfer callback, that is, when the service's reader thread handled the
data. Throttling delays exactly that. `scripts/psvr2_delivery_delay.py` compares
receipt time with device time (VTS) against the lower envelope of their
difference over a rolling window. That cancels the fixed offset and slow clock
drift, so reader-thread delay shows in full. It reports, for IMU and SLAM:

- delivery delay percentiles, overall and per 10 s bucket;
- arrival gaps (IMU normally 1 ms, SLAM 16.7 ms);
- for IMU, the delay absorbed into the driver's own mapping, which is the pose
  lag the current driver adds.

The envelope needs some prompt deliveries inside each window, so also run with a
window covering the whole session (`--window-s 120`). The windowed-only control
run shows how much clock drift that long window lets through.

**Setup.** Build this branch. Make sure no other `monado-service` is loaded
(persistent install: `monado-service-xpc-control uninstall`), then register a
development service that picks up the trace variables:

```sh
export PSVR2_TIMING_TRACE=1
export PSVR2_DRIVER_TIMING_TRACE=1   # the IMU/SLAM/pose files need both
export PSVR2_TIMING_TRACE_DIR="$HOME/psvr2-trace"
mkdir -p "$PSVR2_TIMING_TRACE_DIR"
build/src/xrt/targets/service/monado-service-xpc-control bootstrap
```

The service starts on the first client connection. Each service process writes
its own `monado_psvr2_<PID>_*.csv`. Check that `_imu.csv` and `_slam.csv`
appear once the headset is tracking. `bootstrap` copies every `PSVR2_*`
variable from the shell, so a `PSVR2_DRIVER_TIMING_TRACE=0` left over from
another setup silently disables them.

**Run A: control, no Game Mode.** Start the Unreal app windowed on the Mac
display. Wear the headset and move your head naturally for about 100 s. Quit the
app, then `monado-service-xpc-control bootout` so the service exits and flushes
its traces.

**Run B: Game Mode.** `monado-service-xpc-control bootstrap` again, then start
the same Unreal app windowed. Switching to Terminal ends Game Mode, so start a
background policy log *before* going fullscreen:

```sh
P=build/src/xrt/targets/macos_layer_host_probe
SVC=$(pgrep -x monado-service); GAME=<Unreal app pid>
sudo -v   # cache credentials so the loop can read the Game Mode flag
while true; do
  for pid in $SVC $GAME; do
    printf '%s ' "$(date +%T)"; sudo -n $P/macos-layer-host-probe --role query --pid $pid
  done
  sleep 2
done > ~/psvr2-trace/policy.log 2>&1 &
```

Then, without returning to Terminal, go fullscreen and toggle Game Mode from
its menu (the menu-bar item), rather than by changing the window:

1. About 20 s with Game Mode off.
2. Game Mode on for about 15 s, then off for about 15 s; repeat two or three
   times. Keep moving your head throughout. Short periods get data before
   Unreal's swapchain-wait abort, which hit about 21 s into Game Mode.
3. Quit the app (or let it abort), then stop the loop (`kill %1`) and
   `bootout`. The service keeps running after an app abort, so `bootout` still
   flushes its traces.

`policy.log` should show the service switching to `ext_darwinbg=1` and the
game to `game_mode=on` for the fullscreen minute, with wall-clock times that
also mark the switches. The `sudo` credential cache normally lasts 5 minutes,
which is enough for the run.

**Analysis.**

```sh
scripts/psvr2_delivery_delay.py --window-s 30 \
    ~/psvr2-trace/monado_psvr2_<PID_A> ~/psvr2-trace/monado_psvr2_<PID_B>
scripts/psvr2_delivery_delay.py --window-s 120 --bucket-s 0 \
    ~/psvr2-trace/monado_psvr2_<PID_A> ~/psvr2-trace/monado_psvr2_<PID_B>
```

**Reading it.**

- Run A sets the noise floor: IMU delivery delay p99 should be a few ms, with
  few gaps over 5 ms.
- In run B, compare the Game Mode buckets with the windowed ones either side.
- If Game Mode adds no more than a few ms at p99 (tens of ms max), tracking can
  stay in the service with a shared-memory ring.
- If p95 rises into tens of ms, or gaps reach hundreds of ms, the reader thread
  is being starved. USB reading then has to move into the focused client or a
  helper process in its coalition.
- The "absorbed into mapping" figure during Game Mode is the extra pose lag the
  current driver already adds today.
- The `query` output should show `ext_darwinbg=1` on the real service, matching
  the probe's host.

## Phase 1 result (2026-09-29)

One Unreal session of about 97 s with Game Mode toggled from its menu. Service
pid 19721, `--window-s 30`. The throttled spans come from `compositor_rt.csv`
(compositor thread at priority 4): 8.0–10.4, 25.2–37.7, 46.3–57.8 and
63.9–74.4 s, about 37 s in all.

| | Not throttled | Throttled by Game Mode |
| --- | --- | --- |
| IMU delivery delay, median / p95 / p99 / max | 1.37 / 1.67 / 2.12 / 17.7 ms (n=80 231) | 1.43 / 1.69 / 2.15 / 76.7 ms (n=73 691) |
| SLAM delivery delay, median / p95 / p99 / max | 4.16 / 8.59 / 10.3 / 18.2 ms (n=2 416) | 3.97 / 8.39 / 9.31 / 61.1 ms (n=2 216) |

Whole session: IMU delay absorbed into the driver's mapping, median 1.43 ms,
p99 2.75 ms, max 23.7 ms; 9 IMU arrival gaps over 5 ms; 6 SLAM gaps over 30 ms.

- **Typical delivery is unaffected.** Median, p95 and p99 are the same with and
  without Game Mode, to within about 0.1 ms. The PS VR2 reader's work per
  wake-up is tiny, and it still gets CPU time promptly at priority 4 under a
  real game's load.
- **Game Mode adds rare spikes.** The worst delays (77 ms IMU, 61 ms SLAM,
  against 18 ms otherwise) and the few long gaps fall in throttled periods.
  The driver's 80 ms clock filter turns such a spike into a short pose-lag
  excursion (up to 24 ms in this session).
- **Decision: tracking stays in the service.** No USB reader moves into the
  client or a helper. Phase 2 is essentially moving the compositor.
- **The shared-memory pose ring moves to a later phase.** It is still worth
  having: mapped with a lower-envelope estimator, it would ignore late samples
  instead of absorbing them, removing the pose-lag excursions, and it removes
  IPC from pose queries. But phase 2 does not depend on it.

The throttled share of compositor samples (12.7 %) is much lower than the
throttled share of time (about 38 %), because a starved compositor takes few
samples; the time spans are the right measure.

The 120 s window is not useful. Its per-bucket p95 creeps from 1.2 to 3.1 ms
over the session, the signature of about 20 ppm drift between the headset
clock and the host clock, which a 30 s window keeps under about 0.6 ms.

## Phase 2 status (2026-09-29)

All of phase 2 is in the tree, behind `XRT_MACOS_CLIENT_COMPOSITOR=1` in the
client's environment. Without it, clients take exactly the old service path.

- **Service side** (`ipc_server_macos_display_host.c`, `comp_window_macos.m`):
  a table of client `CALayerHost`s above the presenter's own layer, with
  `compositor_hosted_attach`, `_set_visibility` and `_detach`. The layer is
  removed when the client disconnects.
- **Hosted presenter** (`comp_window_macos.m`): in a client, the
  `CAMetalLayer` sits in a `CAContext` rather than in a window. It shows
  itself in the handoff order (shown, then exclusive after the first present).
- **Metal swapchains** (`comp_metal_glue.c`): service builds carry both paths
  and pick the direct one, with a local semaphore pair, when hosted.
- **IPC client** (`ipc_client_macos_hosted.c`, `ipc_client_system.c`): when
  asked, the client registers the host functions and creates
  `comp_main_create_system_compositor()` on the IPC head device. Sessions are
  headless on the service side. The local compositor's events replace the
  compositor events of the service's unused per-session compositor. If the
  local compositor cannot be created, everything is unregistered and the
  client falls back to the service.
- **IPC head device**: the shared memory now carries the screen size, frame
  interval, viewports, rotations and distortion FoVs. A hosted client builds
  its distortion mesh once at start-up, by computing it through the service's
  device. That is one IPC round trip per mesh vertex, about 8,500 at the
  default `XRT_MESH_SIZE`, so start-up takes a little longer.

The shared-memory layout changed, so rebuild the Wine client alongside the
service.

Not done yet:

- The service does not see `xrSessionBegin`/`End` from a hosted client. Its
  app list and focus logic treat the client as idle.
- There is one hosted client at a time and no mid-session handoff (phase 3).

### Testing phase 2

1. Start the service as usual. Its presenter must own the headset window.
2. Run the application with `XRT_MACOS_CLIENT_COMPOSITOR=1`, for example
   `XRT_MACOS_CLIENT_COMPOSITOR=1 hello_xr -g Metal`. For a bundled game, set
   the variable in the launch environment.
3. The client log should say `Compositing in-process; the service hosts this
   client's layer`. The service log should show the attach and the visibility
   changes. `U_LOG_W` "In-process compositor unavailable" means it fell back.
4. Repeat the phase 1 Unreal run with `PSVR2_TIMING_TRACE=1`, with and without
   Game Mode. The compositor RT trace now comes from the client process.

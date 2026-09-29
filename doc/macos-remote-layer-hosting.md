# macOS: Game Mode demotion and cross-process layer hosting

## Problem

With Game Mode active for an OpenXR game (Unreal), the service's Multi Client
Module compositor thread drops from realtime priority 97 to timeshare
priority 4, runs mostly on E-cores and shows effective QoS Background. See the
2026-09-17 entry in `macos-psvr2-timing-diagnostics.md`.

## Why the XPC importance lease cannot fix it

Checked against XNU source (xnu-12377, macOS 26):

- Priority 4 is `MAXPRI_THROTTLE`, the task-wide ceiling applied when the task
  is effectively DARWIN_BG (`task_policy.c`, `tep_lowpri_cpu`).
- Lowering the task ceiling demotes realtime threads to timeshare
  (`thread_policy.c`, `sched_thread_mode_demote(TH_SFLAG_THROTTLED)`).
- DARWIN_BG forces effective QoS to Background (`thread_policy.c`).

So the whole service task is being backgrounded; the compositor thread is not
targeted individually.

A task can become DARWIN_BG in six ways. An XPC importance boost
(`trp_boosted`) cancels only one: an `Adaptive` launchd job with no boost held.
It does not cancel externally requested background, the background task role,
coalition suppression, a Background QoS clamp or runaway mitigation.

A hardware A/B with `XRT_MACOS_LAUNCHD_PROCESS_TYPE=Interactive` and no lease
made no difference. The service is therefore being backgrounded externally
under Game Mode (RunningBoard or gamepolicyd), and no XPC mechanism can prevent
that. Kernel Game Mode itself (`TF_GAME_MODE`) is only a CLPC hint on the game's
own task and coalition thread group.

Also ruled out: `os_workgroup` / work intervals (DARWIN_BG clears
work-interval-driven priority) and turnstile inheritance (clamped for every
DARWIN_BG cause except the `Adaptive` one).

## Direction: composite in the game process

Apple's own designs keep frame-critical work in the game's process or
coalition. The plan is to move timewarp, distortion and presentation into the
focused client, while `monado-service` keeps devices, tracking, session
management and a fallback presenter.

Two ways to get client frames onto the headset display:

1. **Public API, window-ordering handoff.** Each owner creates its own
   fullscreen headset window. Handoff pre-warms the new owner's window behind
   the current one, then swaps window order at an agreed frame. The service's
   window stays underneath as the fallback. Cross-process window stacking and
   the gap during a swap are the risks.
2. **Private API, `CAContext` / `CALayerHost`.** The service owns the only
   headset window. Each client renders into a `CAMetalLayer` attached to a
   `CAContext` and sends the 32-bit `contextId` to the service, which shows it
   in a `CALayerHost`. Frames go straight to WindowServer, so throttling the
   service does not delay them. Handoff is an atomic sublayer swap, which
   `createFencePort` / `setFencePort:` can align with the new owner's first
   commit.

Option 2 is what Chromium (and every Chromium-based browser) and WebKit use for
their GPU processes. Chromium declares the API in
`ui/base/cocoa/remote_layer_api.h`, checks for it at runtime in
`RemoteLayerAPISupported()` and has a feature-flag kill switch. When the API is
unavailable it falls back to sending an IOSurface to the browser process.
Electron's Mac App Store builds strip it
(`patches/chromium/mas_avoid_private_macos_api_usage.patch`), reporting mainly
a power cost. Notarization does not check API use, so Developer ID
distribution is unaffected.

If option 2 is used, follow Chromium: use only the calls it uses, check for
them at runtime, provide an environment kill switch and fall back to option 1.

## The open question: present timing when hosted

Chromium's evidence is about power at 60 Hz. For VR we need to know whether a
hosted `CAMetalLayer` keeps precise present timing and direct scanout at
120 Hz. `macos-layer-host-probe` answers that.

### Probe

`src/xrt/targets/macos_layer_host_probe`, built on Apple only. It opens a
borderless window on the headset display with the same window and
`CAMetalLayer` configuration as Monado's legacy headset window: CVDisplayLink
pacing, 3 drawables, display sync on, and `afterMinimumDuration` of 8000 µs by
default. It draws a moving white bar so judder is visible.

| Mode | Renderer | Display |
| --- | --- | --- |
| `direct` | this process, own window's layer | baseline |
| `hosted-local` | this process, via `CAContext` | `CALayerHost` in the same process |
| `hosted` | spawned child process, via `CAContext` | `CALayerHost` in the host process |

The rendering process records, per drawable, the CPU submit time, the
CVDisplayLink vblank target and `presentedTime`. It writes
`<prefix>_<mode>_<role>_<pid>.csv` and prints a `LAYER_HOST_PROBE summary`
with present-interval percentiles, the share of intervals over 1.5 periods,
not-presented drawables and presented-minus-target / presented-minus-submit
latency.

```sh
macos-layer-host-probe --mode direct       --seconds 30
macos-layer-host-probe --mode hosted-local --seconds 30
macos-layer-host-probe --mode hosted       --seconds 30
# Optional: --display N, --present at-time|immediate, --min-duration-us US
```

`--display` defaults to the last screen, which is usually the headset. Stop
`monado-service` first so it does not own the display.

### Reading the results

- `hosted` matches `direct` (same interval distribution and latency within
  noise, no extra not-presented frames): option 2 is viable for timing.
- `hosted-local` is already worse than `direct`: hosting itself costs
  something (for example, direct scanout is lost), independent of processes.
- Only `hosted` is worse: the cost is in the cross-process commit path.
- A consistent one-period shift in presented-minus-submit means hosting adds a
  frame of latency.

Instruments' Display trace can confirm whether the headset surface is still
scanned out directly in each mode.

### Results, 2026-09-29

PS VR2 at 4000x2040, 119.880 Hz, 30 s per mode, default `min-duration` 8000 µs:

| Mode | Submitted | Not presented | Interval median / p95 / p99 ms | >1.5× period | Presented − target median / p95 ms | Presented − submit median / p95 ms |
| --- | ---: | ---: | --- | ---: | --- | --- |
| `direct` | 3547 | 0 | 8.342 / 8.342 / 16.683 | 1.41% | 8.342 / 8.342 | 16.141 / 16.234 |
| `hosted-local` | 3537 | 2 | 8.342 / 8.342 / 16.683 | 1.44% | 8.342 / 8.342 | 16.125 / 16.264 |
| `hosted` (child) | 3573 | 0 | 8.342 / 8.342 / 8.342 | 0.70% | 8.342 / 8.342 | 16.080 / 16.202 |

Cross-process hosting adds no measurable cost. Latency from submit to present
is the same in all three modes to within about 0.1 ms, so there is no extra
frame. Interval distributions match, and the hosted run had fewer missed
vblanks, which is within run-to-run noise. `presented − target` is exactly one
period in every mode, so the CVDisplayLink output time relates to the actual
present the same way whether or not the layer is hosted.

The probe renders almost nothing, so these numbers do not yet cover a
GPU-loaded client. They also do not show whether the surface was scanned out
directly; that needs an Instruments Display trace.

Since the Game Mode test was added, render threads use a realtime
time-constraint policy by default, like Monado's compositor thread. Pass
`--rt 0` to repeat the runs above exactly.

## Game Mode test

The timing runs above keep both processes in the terminal's coalition, so
Game Mode never separates them. The Game Mode test reproduces the real
arrangement:

- **Host:** `macos-layer-host-probe --role bootstrap-host` registers the probe
  with launchd as a LaunchAgent (`org.freedesktop.monado.layer-host-probe`,
  `ProcessType=Interactive` by default, `--process-type Adaptive` to match the
  service's current default). It therefore runs in its own coalition, like
  `monado-service`. It opens the headset window and waits on
  `/tmp/monado-layer-host-probe.sock`.
- **Game:** `macos-layer-host-probe-game.app` is the same program built as an
  app bundle with `LSApplicationCategoryType=public.app-category.games` and
  `GCSupportsGameMode`. It goes fullscreen on the Mac's own display so Game
  Mode engages, waits `--warmup` seconds, connects to the host and runs for
  `--seconds`. `--cpu-load N` adds N busy threads at user-interactive QoS in
  place of a game's workers.

Two modes, chosen when the host is bootstrapped:

| Mode | Who renders to the headset | Expected under Game Mode |
| --- | --- | --- |
| `game-direct` | the host, as `monado-service` does today | reproduces the problem |
| `game-hosted` | the game, through `CAContext`; the host shows it in a `CALayerHost` | the proposed fix |

To make throttling of the host visible whichever process renders, the host
runs a realtime canary thread. It wakes every refresh period and logs its
priority class (`realtime` at 97, `throttled` at 4) and both processes'
Darwin-background state: the `darwinbg` and `ext_darwinbg` flags, `adaptive`
and `adaptive_important`, and the Darwin role. It logs on every class change
and every 5 s. It starts before the game connects, which gives a baseline.
Each rendered frame also records its render thread's priority.

`macos-layer-host-probe --role query --pid PID` prints the same state for any
process, for example the real `monado-service` while Unreal runs. Run it with
`sudo` to also read whether Game Mode is on for that process.

### Running it

Stop `monado-service` first so it releases the headset display. From the build
directory:

```sh
P=src/xrt/targets/macos_layer_host_probe

# 1. Host under launchd, in its own coalition. It must outlast the game.
$P/macos-layer-host-probe --role bootstrap-host --mode game-direct --seconds 120

# 2. Game half: goes fullscreen on the Mac display. Check that the Game Mode
#    icon appears in the menu bar.
open -n $P/macos-layer-host-probe-game.app --args \
    --seconds 60 --warmup 5 --cpu-load "$(sysctl -n hw.ncpu)"

# 3. When the game exits (about 65 s), read both logs.
cat /tmp/layer_host_probe_host.log /tmp/layer_host_probe_game.log

# 4. Repeat steps 1-3 with --mode game-hosted.

# 5. Remove the LaunchAgent.
$P/macos-layer-host-probe --role bootout-host
```

The logs append, so clear them between runs or read from the latest `host
pid=` line. CSVs are written to `/tmp/layer_host_probe_<mode>_{host,game,canary}_<pid>.csv`.

### Reading the results

- **Canary moves `realtime` → `throttled` once the game is fullscreen:** the
  probe reproduces the demotion. The host flags show the mechanism:
  `ext_darwinbg=1` means another process set it; `role=darwin-bg` means a task
  role; `darwinbg=1` with neither points to coalition suppression or a QoS
  clamp; `adaptive=1 adaptive_important=0` would mean the `Adaptive` cause.
- **`game-direct`:** the host's render summary should show throttled frames
  and worse intervals than the 2026-09-29 baseline. This is today's service.
- **`game-hosted`:** the game's render summary is the key result. If it
  matches the baseline while the canary shows the host throttled, the design
  works: frames no longer depend on the host's priority.
- **Canary never throttled:** either Game Mode did not engage (check the menu
  bar icon), or it does not throttle this LaunchAgent the way it throttles
  `monado-service`. Try `--process-type Adaptive`, and compare with
  `query --pid` on the real service while Unreal runs.

### Results, 2026-09-29

LaunchAgent host with `ProcessType=Interactive`, game app fullscreen on the Mac
display with Game Mode engaged, `--cpu-load 10`, 60 s after a 5 s warm-up. PS VR2
at 119.880 Hz.

**The demotion is reproduced, and its mechanism identified.** In both runs
the host's canary went `realtime` (97) → `throttled` (4) while the game was
still warming up in fullscreen, before it connected. It stayed throttled until
the game quit, then returned to 97 within about 30 ms. Throughout:

```
host{darwinbg=0 ext_darwinbg=1 adaptive=0 adaptive_important=0 app=0 role=default}
game{darwinbg=0 ext_darwinbg=0 adaptive=0 adaptive_important=0 app=1 role=ui-focal}
```

`ext_darwinbg=1` is `trp_ext_darwinbg`, Darwin background requested *from
outside* the process (by RunningBoard or gamepolicyd under Game Mode). The
role is untouched, the job is not `Adaptive`, and there is no internal request.
An XPC importance boost does not cancel `trp_ext_darwinbg`, which is why the
lease had no effect.

| | `game-direct` (host renders, today's design) | `game-hosted` (game renders, host shows `CALayerHost`) |
| --- | --- | --- |
| Host throttled | ~64.5 s of ~78 s | ~65 s of ~70.5 s |
| Host canary wake lateness p99 / max | 348 ms / 8.2 s | 252 ms / 706 ms |
| Renderer | host, priority 4 for 95.7 % of frames | game, priority 97 for every frame |
| Frames submitted / presented | 23 / 8 in ~58 s | 7098 / 7043 in 60 s |
| Present interval median / p95 / p99 | 41.7 ms / 9.6 s / 9.6 s | 8.342 / 8.342 / 8.342 ms |
| Intervals > 1.5× period | 71.4 % | 0.51 % |
| Presented − submit median / p95 | 132 ms / 5.05 s | 15.15 / 16.25 ms |

(Throttled times are from the canary transition timestamps. The summary line
in these runs weighted by samples, which understates throttling because a
starved thread takes few samples; it is now weighted by time.)

`game-direct` collapses completely under this load: the host renders a handful
of frames seconds apart. `game-hosted` matches the unloaded timing baseline
(0.51 % long intervals against 0.70–1.41 %, the same one-period target offset,
slightly lower latency) while the host is throttled to priority 4 for the whole
session. Frame delivery no longer depends on the hosting process's scheduling.

The one difference from the baseline is 55 not-presented drawables (0.77 %,
against 0–2). Whether they cluster at attach or teardown or are spread through
the run can be read from `presented_s == 0` in the game CSV.

The load here (10 busy threads at user-interactive QoS on every core) is harsher
than a typical game, so `game-direct` is a worst case. The qualitative result
holds either way: with the service externally backgrounded, only the process
Game Mode favours can keep frame timing.

## Handoff test

In the proposed design, the service keeps the headset window and switches
which client's `CALayerHost` is shown when focus changes. The next client is
already rendering (pre-warmed), and the service is Darwin-backgrounded while a
game has Game Mode. The `handoff` mode tests exactly that swap.

The host spawns two clients, A (red) and B (blue), which render continuously
into their own `CAContext`s. Every `--swap-every` seconds (default 2) the host
swaps which `CALayerHost` is shown in one `CATransaction`:

- `--swap-method reparent` (default) adds the new host layer and removes the
  old one, as Chromium does.
- `--swap-method hidden` keeps both attached and toggles `hidden`.

`--host-background 1` puts the host into Darwin background after spawning the
clients. That is the priority-4 clamp Game Mode applied in the test above, via
`setpriority(PRIO_DARWIN_PROCESS, 0, PRIO_DARWIN_BG)`, without needing the game
app. `--cpu-load N` adds N busy threads in each client.

After both clients exit, the host rebuilds what was on screen from the two
client CSVs and its own swap log, and prints a `handoff summary`:

- **Max on-screen gap around each swap:** the longest interval between
  presented frames of whichever client was shown, from 2 periods before to 10
  after the swap commit. One period (8.34 ms) means a seamless swap.
- **Swap commit → first new-client present:** how quickly the new client's
  content appears.
- **Swap timer lateness:** how late the host ran each swap. Under
  `--host-background 1` this shows the cost of the throttled service.
- **Steady-state long intervals**, away from swaps, for comparison.
- **Hidden-client frames reporting `presentedTime`:** if a hidden client's
  drawables still report being presented, `presentedTime` cannot say what was
  visible and the gap figures are unreliable. The summary warns when this
  happens; then judge the swap by eye (a black or stale frame at the colour
  change).

Each client also prints its usual render summary. With the `reparent` method,
watch the hidden client's `nil_drawables`: if a detached context never releases
its drawables, the hidden client stalls, and pre-warming needs the `hidden`
method instead.

Per-swap figures go to `/tmp/layer_host_probe_handoff_swaps_<pid>.csv`.

```sh
P=src/xrt/targets/macos_layer_host_probe
$P/macos-layer-host-probe --mode handoff --seconds 30
$P/macos-layer-host-probe --mode handoff --seconds 30 --swap-method hidden
$P/macos-layer-host-probe --mode handoff --seconds 30 --host-background 1
$P/macos-layer-host-probe --mode handoff --seconds 30 --host-background 1 --cpu-load "$(sysctl -n hw.ncpu)"
```

### Handoff results, 2026-09-29

30 s per run, swap every 2 s, PS VR2 at 119.880 Hz. By eye, every swap looked
clean in all four runs: no black or frozen frame at the colour change.

| Run | Swap timer lateness median / max | Commit → first new present median / max | Client presented − submit median (A / B) |
| --- | --- | --- | --- |
| `reparent` | 0.9 / 1.1 ms | 44.4 / 57.6 ms | 16.0 / 16.0 ms |
| `hidden` | 1.1 / 1.1 ms | 33.8 / 40.9 ms | 16.0 / 16.0 ms |
| `reparent`, host backgrounded | 55.9 / 82.8 ms | 49.3 / 57.7 ms | 23.7 / 16.1 ms |
| `reparent`, host backgrounded, `--cpu-load 10` | 12 328 / 13 250 ms | 55.6 / 57.6 ms | 24.3 / 32.5 ms |

- **Hidden clients never report `presentedTime`** (0 of about 3500 frames in
  each unloaded run; 20 of 2980 under load, where swaps ran seconds late). So
  `presentedTime` is a reliable record of what was on screen.
- **Pre-warming works with both methods.** Hidden clients kept getting
  drawables (`nil_drawables=0`); their frames were simply not presented.
- **The swap takes effect 34–55 ms after the host commits it**, a little
  sooner with `hidden` than with `reparent`. The old client stays on screen
  until then, so there is no visible gap, matching what was seen.
- **The first analysis reported a 42–58 ms "gap" at every swap. That was an
  artifact:** it assigned frames to clients by the host's commit time, and so
  discarded the old client's frames that were still on screen while the swap
  was pending. The analysis now treats every presented frame as on screen, and
  reports the real switch gap (old client's last present → new client's first)
  and how long the old client stayed after the commit.
- **A throttled host is slow to perform the swap.** Darwin-backgrounded, its
  main thread ran swaps a median 56 ms late with no load, and about 12 s late
  with 10 busy threads in each client. Frame delivery was unaffected, but
  focus changes decided and committed by the service would be delayed the same
  way under Game Mode with a heavily loaded CPU.
- **Open question: client latency rose 1–2 frames with the host backgrounded**
  (A 23.7 ms; under load A 24.3 and B 32.5 ms, against 16.0 ms unthrottled).
  The Game Mode `game-hosted` run did not show this (15.2 ms). The summary now
  splits each client's latency into before and after the first swap, to show
  whether it comes from the host's state or from swaps committed by a
  throttled host. A run with `--host-background 1 --swap-every 1000` (no swaps)
  isolates the host's state.

**Design implication.** The display path is immune to the service being
throttled, but anything the service itself must do, such as committing a
handoff, is not. One way round this is to keep every client's `CALayerHost`
attached permanently and have each client show or hide its own layer inside
its own `CAContext`. The commit then happens in unthrottled client processes,
and the two clients' commits can be made atomic with a shared fence port.
The service would only tell clients about focus, which still goes through the
throttled service, but only as a small message rather than a layer-tree commit.

### Client-driven visibility

`--swap-method client` removes the service from the swap entirely. The host
attaches both `CALayerHost`s once, A below and B stacked above, both left
visible, and never commits again. Each client's `CAContext` holds a container
layer around its `CAMetalLayer` that the client shows or hides itself.

At swap *k* (from a shared start time, every `--swap-every` seconds):

1. The incoming client shows its container, committing from its own process.
2. It waits until one of its own frames has been presented, then sends *k* to
   the outgoing client over a socket pair connecting the two clients directly.
3. The outgoing client hides its container.

The stacking order makes the two commits safe without a fence port. Switching
to B (on top), B covers A as soon as it appears. Switching to A (underneath), A
is already shown under B before B hides. Either way one valid layer is always
on screen.

The swap schedule runs in the clients, standing in for a focus decision made
somewhere unthrottled. In Monado the decision would come from the service,
whose message to the clients would still be delayed by throttling. What this
removes is the service's layer-tree commit, which the `reparent` and `hidden`
methods need at every swap.

The summary adds the outgoing client's hide lag (its hide commit after the
incoming client's show commit). The switch gap now runs from the old client's
last present before the next swap to the new client's first present after it,
which also catches a gap when an upper layer disappears.

```sh
$P/macos-layer-host-probe --mode handoff --seconds 30 --swap-method client
$P/macos-layer-host-probe --mode handoff --seconds 30 --swap-method client --host-background 1 \
    --cpu-load "$(sysctl -n hw.ncpu)"
```

Expected: swaps as clean as the host-driven ones, and with the host
backgrounded under load, swap lateness staying near zero instead of the ~12 s
seen with host-driven swaps.

### Client-driven visibility results, 2026-09-29

The first runs failed: a file-descriptor collision in `spawn_client` meant
neither client received the peer socket (fixed in `2c7fabc`). After the fix,
30 s per run, 14 swaps each:

| | Unthrottled host | Host Darwin-backgrounded, `--cpu-load 10` |
| --- | --- | --- |
| Switch gap median / max | 8.342 / 8.342 ms | 8.342 / 8.342 ms |
| Max on-screen interval around swaps | 8.342 ms | 8.342 ms |
| Swap lateness median / max | 2.3 / 5.0 ms | 5.0 / 5.0 ms |
| Incoming show commit → own first present, median | 36.6 ms | 32.3 ms |
| Outgoing hide after incoming show, median | 37.6 ms | 33.4 ms |
| Hidden frames reporting `presentedTime` | 0 of 3498 | 0 of 3380 |

- **Every swap was seamless:** the new client's first frame followed the old
  client's last by exactly one refresh period, in both runs.
- **Host throttling no longer matters:** under the same load that delayed
  host-driven swaps by ~12 s, client-driven swaps ran within 5 ms of schedule.
  The host made no commits after setup.
- **The overlap works as designed.** The outgoing client hides about 33–38 ms
  after the incoming client's show commit, once the incoming frame is on screen.

**Latency.** Clients' presented − submit was 24 ms before the first swap and
28–32 ms after (1–2 frames above the 16 ms of the single-client and unthrottled
host-driven runs). The same +1 or +2 frames appeared in the host-driven runs
with a backgrounded host, but not consistently. The values are quantised at
16, 24 and 32 ms with a matching 8.3 → 16.7 → 25.0 ms step in presented −
vblank target, and they stay raised rather than recovering.

This looks like a latency ratchet in the presentation pipeline rather than a
cost of hosting: with CVDisplayLink pacing, three drawables and
`afterMinimumDuration`, any hiccup that delays one present leaves one more
drawable queued, and nothing drains it again. Monado's legacy presenter uses
the same configuration, so this may matter beyond the probe. Checks:

- the per-frame CSVs should show `present_minus_submit_ms` stepping up at
  discrete moments and then staying level;
- `--present at-time` should not ratchet if the cause is the queue.

### Not yet covered

- Confirming the real `monado-service` gets the same `ext_darwinbg=1` under
  Unreal (`--role query --pid <service pid>`).
- Handoff when the new client is not pre-warmed (context created at the
  swap), and whether fence ports are needed for that case.
- A GPU-heavy renderer, and whether direct scanout is kept.
- Integration with the multi-client compositor.

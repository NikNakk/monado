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

### Not yet covered

- Handoff between two hosted clients, and fence-port alignment.
- Behaviour under Game Mode. The probe's child inherits the host's coalition,
  so it is not a Game Mode test.
- Integration with the multi-client compositor.

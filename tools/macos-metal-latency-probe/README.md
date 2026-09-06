# macOS Metal latency probe

A standalone Swift/Metal harness for measuring presentation latency on the PS VR2 display under macOS. It does **not** link to Monado, Vulkan, OpenXR, or the PSVR2 tracking driver.

The program targets the display named `PS VR2` (falling back to a 4000-pixel-wide display), covers it with a borderless `CAMetalLayer`, alternates the frame black/white, and records GPU/completion/presentation timestamps to CSV.

## Requirements

- Apple Silicon Mac recommended
- macOS 14 or later
- Xcode / Swift toolchain
- PS VR2 visible to macOS as a display

## Build

```bash
swift build -c release
```

Run the built executable directly so each experiment has an explicit command line:

```bash
BIN=.build/release/metal-latency-probe
```

## Core experiments

Use three drawables initially: the Monado experiment showed that two drawables can starve `nextDrawable()` at 120 Hz.

### 1. Immediate presentation (GAV-like)

```bash
$BIN --mode immediate --drawable-count 3 --frames 2400 --trace /tmp/immediate.csv
```

This is paced by `CVDisplayLink`, obtains a drawable, renders, and calls:

```swift
commandBuffer.present(drawable)
```

### 2. Timed presentation

```bash
$BIN --mode timed --drawable-count 3 --frames 2400 --trace /tmp/timed.csv
```

This calls `present(drawable, atTime:)` using the `CVDisplayLink` output host time. Sweep the requested phase with e.g.:

```bash
$BIN --mode timed --present-offset-us -4000 --frames 1200 --trace /tmp/timed_m4ms.csv
$BIN --mode timed --present-offset-us -2000 --frames 1200 --trace /tmp/timed_m2ms.csv
$BIN --mode timed --present-offset-us     0 --frames 1200 --trace /tmp/timed_0.csv
$BIN --mode timed --present-offset-us  2000 --frames 1200 --trace /tmp/timed_p2ms.csv
```

### 3. CAMetalDisplayLink, one-frame requested latency

```bash
$BIN --mode metal1 --drawable-count 3 --frames 2400 --trace /tmp/metal1.csv
```

The app sets `preferredFrameLatency = 1`, uses the drawable supplied by `CAMetalDisplayLink`, logs both `targetTimestamp` and `targetPresentationTimestamp`, commits the Metal work, and calls ordinary `drawable.present()` as required by Apple.

### 4. CAMetalDisplayLink, two-frame requested latency

```bash
$BIN --mode metal2 --drawable-count 3 --frames 2400 --trace /tmp/metal2.csv
```

## Moving the latch deadline

Two independent controls let us test what matters:

- `--cpu-delay-ms N`: delays submission after drawable acquisition/callback.
- `--gpu-burn-passes N`: inserts synthetic compute work *before* the visible render pass. This is deliberately specified in passes rather than nominal milliseconds; use the logged `gpu_duration_ms` as the real measurement.

For example:

```bash
for d in 0 1 2 3 4 5 6; do
  $BIN --mode immediate --cpu-delay-ms "$d" --frames 600 --trace "/tmp/immediate_delay_${d}.csv"
done
```

and a GPU-load sweep:

```bash
for p in 0 1 2 4 8; do
  $BIN --mode metal1 --gpu-burn-passes "$p" --frames 600 --trace "/tmp/metal1_gpu_${p}.csv"
done
```

The useful question is not the requested burn value but where `gpu_end_to_presented_ms` jumps by one 8.34-ms refresh.

## Other controls

```text
--drawable-count 2|3
--wait-completed
--no-vsync
--refresh-hz N
--display-name NAME
```

`--wait-completed` intentionally perturbs pacing and exists only to reproduce synchronous architectures.

## CSV fields

The trace includes:

- CVDisplayLink callback, `inNow`, and `inOutputTime`
- CAMetalDisplayLink deadline and estimated target presentation time
- `nextDrawable()` start/end and wait
- encoding and commit times
- requested timed-present host time
- Metal GPU start/end and command-buffer completion
- drawable presented-handler callback and `presentedTime`
- computed intervals including GPU-end→presented and requested/target→presented

Timestamps are host-time seconds (`CACurrentMediaTime` / CoreVideo host time) and are directly comparable.

## Quick summary

```bash
./analyze.py /tmp/immediate.csv
./analyze.py /tmp/timed.csv
./analyze.py /tmp/metal1.csv
./analyze.py /tmp/metal2.csv
```

For PSVR2 at ~119.88 Hz, one refresh is about 8.342 ms. The central comparison is whether the different APIs land on the immediately available refresh or systematically one refresh later.

## Suggested first run set

Keep workload at zero and use 3 drawables:

```bash
$BIN --mode immediate --frames 2400 --trace /tmp/immediate.csv
$BIN --mode timed     --frames 2400 --trace /tmp/timed.csv
$BIN --mode metal1    --frames 2400 --trace /tmp/metal1.csv
$BIN --mode metal2    --frames 2400 --trace /tmp/metal2.csv
```

Only after those four runs should you sweep CPU delay / GPU load / timed-present phase.

## Safety / comfort

The default visible frame alternates the full display black/white every submitted frame to make dropped/repeated frames and future photodiode testing unambiguous. Avoid wearing the headset while running this pattern if flicker is uncomfortable.


## Low-latency CAMetalLayer matrix

The probe now defaults to `CAMetalLayer.framebufferOnly = true`, because the drawable is only used as a render target. Use `--no-framebuffer-only` to reproduce the earlier configuration. Both `--vsync` and `--no-vsync` are explicit switches for `displaySyncEnabled`. The chosen values are recorded in each CSV as `framebuffer_only` and `display_sync`.

Suggested comparison, with zero artificial delay/load and three drawables first:

```bash
$BIN --mode metal1 --framebuffer-only --vsync    --frames 2400 --trace /tmp/fb_sync.csv
$BIN --mode metal1 --framebuffer-only --no-vsync --frames 2400 --trace /tmp/fb_nosync.csv
$BIN --mode metal1 --no-framebuffer-only --vsync --frames 2400 --trace /tmp/nonfb_sync.csv
$BIN --mode metal1 --no-framebuffer-only --no-vsync --frames 2400 --trace /tmp/nonfb_nosync.csv
```

If `framebufferOnly=true` + `displaySyncEnabled=false` is promising, repeat with two drawables:

```bash
$BIN --mode metal1 --framebuffer-only --no-vsync --drawable-count 2 --frames 2400 --trace /tmp/fb_nosync_2.csv
```

And use immediate presentation as a control:

```bash
$BIN --mode immediate --framebuffer-only --no-vsync --frames 2400 --trace /tmp/immediate_fb_nosync.csv
```

`framebufferOnly=true` remains compatible with the probe because the visible pass renders directly into the drawable. If this mode proves materially better, Monado's final blit would need to be replaced by a render pass before the same optimization could be used there.


## Native fullscreen / Direct-to-Display comparison

`--window-mode borderless` preserves the original borderless screen-covering window. `--window-mode fullscreen` uses AppKit native fullscreen via `NSWindow.toggleFullScreen(_:)` and waits for `windowDidEnterFullScreen` before starting either display link, so transition frames are excluded from the trace. The requested mode is recorded as `window_mode` in every CSV row.

For a clean comparison keep the Metal settings identical:

```bash
$BIN --mode metal1 --window-mode borderless --framebuffer-only --vsync --frames 2400 --trace /tmp/borderless_dtd.csv
$BIN --mode metal1 --window-mode fullscreen --framebuffer-only --vsync --frames 2400 --trace /tmp/fullscreen_dtd.csv
```

Profile both with Instruments and compare Direct to Display, CPU-to-display latency, surface duration, GPU completion, and GPU-done-to-display/on-glass latency.

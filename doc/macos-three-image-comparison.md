<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Three versus four service Metal images, 2026-10-12

The opt-in three-image path passes the bounded native offscreen and physical
headset workloads. The post-fix headset comparison below maintains about 120 Hz
submission and passes sampled pixels, but repeats more client-frame latches with
three images. SteamVR workload and headset-worn visual acceptance remain
unrecorded. Keep the four-image default and the compatibility mode opt-in.

The subsequent [production SteamVR-backend GPU test](macos-steamvr-resolve-copy-test.md)
exports all three resolve images across Windows processes and reduces the
backend's per-eye copies from 36 to zero over 18 stereo frames. Actual SteamVR
deployment and headset acceptance still require the
[hardware tests](macos-gpu-sync-hardware-tests.md).

This concerns **client swapchain images**, not display drawables. The
CAMetalLayer drawable pool already uses three. With
`XRT_MACOS_METAL_THREE_IMAGE_SWAPCHAINS=1`, the service enables latest-completed
scheduling as well as allocating three images. Producer readiness, source-image
claims and observed consumer GPU completion still guard reuse. The comparison
therefore tests the whole compatibility mode, not an isolated allocation count.

## Workload and evidence

Runtime source: `cf117ba24`, with the new test fixture and runner working-tree
changes. Runtime synchronization and presentation code were unchanged. Artifact
hashes and the working-tree diff hash are retained in the capture manifest.
Raw recordings are outside Git at `/tmp/macos-three-image-comparison-20261012`.

The native Metal client allocates a 1024×1024 two-slice BGRA array through the
production shared-Metal service path. Simple 2D BGRA IOSurface swapchains already
have three images by default and would make an unsuitable count comparison.
The fixture asserts the enumerated count is exactly three or four, clears both
slices on the real producer queue and alternates the slice submitted as a quad.
Separate 240-frame correctness runs verify the service scheduler marker and
composed Vulkan readbacks of the expected panel in both eyes. On the headset,
proof runs lock the quad to the head (`GPU_SYNC_VIEW_LOCKED=1`, 2 m at 1.5 m).
A 1 m quad fixed in the room is carried off the sample points when the headset
moves. Timing runs keep the room-fixed 1 m quad. The proof runs log
a sample every 30 frames. Samples taken within 30 frames of the first client
latch are excluded, because the first composed headset frames can be blank.
At least four samples must remain, with at most one blank. Each run's
`pixel-check.json` lists every sample, and any blank frame with the client frame
it latched. These constant
colour samples establish import/composition, not per-frame stale-image detection;
the M1–M5 patterned reuse tests remain separate evidence.

Four timing runs use four–three–three–four order, 1200 frames each, approximately
20 seconds per run. Diagnostic pixel readbacks and per-image logging are disabled
for these measurements. CSV timing remains enabled identically in both modes.
The simulated HMD/debug-image compositor runs at 60 Hz with its normal render
scale. This is a small producer workload, not GPU saturation or SteamVR Home.
All recorded Foundation thermal samples were nominal, low-power mode was off,
and Game Mode was not requested. CPU/GPU frequencies were not measured.
The first 30 client frames are excluded from timing distributions.

| Run | Images | Submitted Hz | Fresh latch Hz | Acquire/wait p95 / p99 (µs) | End-frame entry→latch p50 / p99 (ms) |
| --- | ---: | ---: | ---: | ---: | ---: |
| A1 | 4 | 60.008 | 54.401 | 249 / 285 | 14.411 / 17.285 |
| B1 | 3 | 59.988 | 54.966 | 244 / 278 | 14.395 / 17.267 |
| B2 | 3 | 59.999 | 53.116 | 251 / 291 | 14.301 / 17.231 |
| A2 | 4 | 59.955 | 52.192 | 113 / 177 | 14.388 / 17.145 |

Fresh-latch gaps reached approximately 33.36 ms in all four runs; both modes
reprojected some frames. Three images did not introduce a prolonged starvation
episode. Acquire/wait maxima were 355, 357, 834 and 429 µs in run order.
The last four-image run had a 9.386 ms previous-fence wait (9.540 ms draw
dispatch); the three-image maxima were 5 and 14 µs. Its other CPU medians also
fell, so nominal thermal state does not establish identical scheduling/frequency.
Do not infer either a three-image improvement or a universal performance cost
from this small sample. GPU pixel runs and all four timing clients passed;
the existing IPC session/scheduler lifecycle test also passed.

## Post-fix physical headset comparison, 2026-10-12

The user ran `/tmp/monado-three-image-headset-new-new` on clean source
`06e01fc2a7c1c7e7c8bbb58f8e22826ffcadd015`, including the render-resource
lifetime fix `e20c4150b`. The manifest records no service overrides, background
load or startup retries. Both 240-frame proof clients and all four 2400-frame
timing clients exited successfully; the service logs contain no GPU-hang or
device-loss messages. Both proof checks matched all seven eligible samples,
with no blanks. This is sampled composition evidence, not per-frame image
freshness or a headset-worn visual judgement.

| Run | Client images | Submitted Hz | Fresh latch Hz | Reused latches | Physical gaps >12.5 ms | Enqueue→display p50 (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| A1 | 4 | 119.881 | 115.425 | 89 | 1 | 27.627 |
| B1 | 3 | 119.882 | 112.744 | 141 | 3 | 27.693 |
| B2 | 3 | 119.830 | 113.455 | 130 | 8 | 27.629 |
| A2 | 4 | 119.882 | 116.945 | 60 | 3 | 27.575 |

Latch statistics are from `comparison.json`. Physical timing joins each
`presented.csv` frame to `present.csv`'s `host_call_ns` (the presentation enqueue
time), excludes compositor frames ≤30 and zero physical timestamps, and uses
successive physical presentation times for gaps. There are 2363–2372 eligible
presentations per timing run. All four physical gap medians are 8.342 ms;
the maximum is 25.025 ms for B2 and about 16.683 ms otherwise. All four
enqueue→display p99 values are 27.815–27.827 ms. Presenter rows use
`metal_shared_event`; the CAMetalLayer drawable count is three in every arm.

Three images do not cause sustained starvation in this light workload, but
fresh client latches are consistently less frequent in these two repetitions.
Physical presentation cadence and median latency are similar between modes.
The roughly 27.6 ms enqueue→display latency is one refresh above the earlier
19.23–19.24 ms post-fix comparison recorded in the hardware ledger; this capture
does not explain that common shift. Do not attribute it to the client image count
or claim latency is unchanged across sessions. Nominal thermal state does not
establish identical scheduling. Loaded and SteamVR trials remain separate gates.

Follow-up trace comparison localises the extra refresh after GPU completion.
In `/tmp/monado-cmp-fencefix-light-2/A1-four`, physical presentation minus the
chosen output slot is 8.319 ms median; in the new A1 it is 16.668 ms. The
chosen target relative to desired presentation remains 8.341 ms in both, and
the Metal request remains 2 ms before that target. Presentation completion
callback latency from enqueue falls from 6.066 to 3.664 ms, while physical
latency rises from 19.236 to 27.627 ms. Thus the added delay is not an 8 ms
render/fence stall or a changed requested presentation offset.

The old A1 has queue-depth diagnostic values 1/2 on 2353/15 eligible frames;
the new A1 has values 1/2/3 on 268/1422/682. This diagnostic measures refreshes
after the selected output slot, not an independently measured count of queued
drawables. The new A1 starts near the old offset and later enters the deeper
presentation state. Its worker-start delay median falls from 3.182 to 0.016 ms.
These observations suggest presentation queueing/pacing; they do not identify
the trigger. Both earlier fence-fix comparisons retained about 19.2 ms latency,
and the new proof runs also have about 19.25 ms medians. Retain the mandatory
resource-lifetime fence: changing it back is not a justified latency remedy.

### Matched drawable-slot diagnostic, 2026-10-12

At the user's explicit request, Codex ran the hardware fixture first with
`--service-env XRT_MACOS_DRAWABLE_SLOT=0`, then with the value `1`, in
`/tmp/monado-slot-{off,on}-20261012-codex`. Each comparison used two proof runs
and four 2400-frame timing runs. All three binary hashes exactly match the
user's `monado-three-image-headset-new-new` manifest; no rebuild or runtime code
change was made. Source HEAD remained `06e01fc2a`; only comparison documentation
was modified. No inherited MVK, MTL, GPU_SYNC or compositor environment overrides
were present. The runner held the display awake; no competing stress load ran.

All twelve clients exited successfully, no startup retry was needed, and no
GPU hang, device loss or asynchronous Metal presentation failure was recorded.
All four proof checks matched 7/7 eligible samples with no blanks. No
headset-worn visual judgement was collected. Thermal samples were nominal.

| Run | Slot off: enqueue→display p50 / p99 (ms) | Slot on: enqueue→display p50 / p99 (ms) | Physical gaps >12.5 ms, off / on |
| --- | ---: | ---: | ---: |
| A1-four | 36.107 / 36.174 | 27.756 / 27.825 | 0 / 0 |
| B1-three | 27.744 / 27.834 | 19.400 / 19.478 | 0 / 0 |
| B2-three | 27.742 / 27.829 | 27.748 / 27.832 | 1 / 0 |
| A2-four | 27.754 / 27.840 | 19.424 / 27.818 | 2 / 7 |

Submission rates remain 119.823–119.882 Hz. Fresh latch rates are
111.574–113.904 Hz with slot off and 111.372–113.297 Hz with slot on: this pair
does not reproduce a consistent three-image freshness penalty. Physical
cadence alone therefore cannot establish low latency. A1 slot off remains at
three refreshes after the chosen slot throughout all ten 240-frame trace
blocks. Slot-on B1/A2 reach the lower latency state; A2 also has a substantial
upper tail. Presentation completion callback latency from enqueue has medians
3.467–5.418 ms across the eight runs, excluding a full-refresh fence/GPU
completion stall as the measured added delay. `physical-comparison.json` in each
output folder retains distributions, queue-depth diagnostics and block medians,
using the same physical timing definitions above.

Disabling drawable prefetch does not recover the old latency. Extra buffering
can occur without the slot worker, and the same fixed binaries can yield about
19.4, 27.7 or 36.1 ms. Keep slot mode enabled by default. The trigger for the
different presentation states remains unresolved; this sequential, one-session
comparison does not prove an intrinsic latency benefit for either client image
count. Investigate bounded presentation backlog and pacing separately from
the mandatory render-resource lifetime fence.

Catch-up scope: slot mode replaces an older **pending** presentation job when
a newer job arrives before the worker consumes it. The recent slot-on A1/B1/B2
runs recorded no supersessions; A2 recorded six. This does not implement an
active physical-latency threshold or drain already-submitted presentations, so
the worker can sustain the display rate while remaining an extra refresh behind.
The earlier standalone stale-substitution path was removed during toggle cleanup.
Also, slot off disables the presentation worker itself and runs presentation on
the compositor thread; this diagnostic compares those complete execution paths,
not solely the cost of prefetching a drawable.

## Opt-in latency guard trial, 2026-10-12

The native presenter now has an independently implemented opt-in
`XRT_MACOS_PRESENT_LATENCY_GUARD=1`. It observes physical presentation at two or
more refreshes after the chosen slot for one second, then withholds presentation
for a full refresh, with a ten-second cooldown. This suppresses one or two
arriving jobs, rather than allowing an immediate catch-up burst. It does not
skip Vulkan rendering or weaken completion/reuse guards: skipped outputs retire
behind their captured render-complete timeline values. Ref-counted callback
state and a mutex protect feedback through asynchronous teardown. Out-of-order
callbacks and old pre-drain frames cannot retrigger; a feedback gap exceeding
four refreshes resets the sustained-depth observation. Feedback exists with
traces disabled, without also turning on trace-driven pacing calibration.

Codex ran `/tmp/monado-guard-{off,on}-20261012-codex`, using the same rebuilt
binaries in both arms, slot mode explicitly on, no competing load and 2400
frames per timing run. Both manifests identify HEAD `06e01fc2a` plus development
changes, with service SHA-256 `8934517882b4ff6d7e6ce799e197107a44f6cccb01c4ba71449f63f5e0e574ae`
and runtime SHA-256 `4b0b9d6d555145f1e39c15cc62ea52bed00788e021b4e6a3b0c8e35b50c2c1b4`.
These hashes differ from the preceding slot diagnostic; compare within this
pair. All twelve clients passed without hangs, device loss, retirement errors
or retries, and all four proof checks matched 7/7 samples without blanks. No
headset-worn visual judgement was collected.

| Run | Guard off: enqueue→display p50 / p99 (ms) | Guard on: enqueue→display p50 / p99 (ms) | Drains / skipped jobs, on | Physical gaps >12.5 ms, off / on |
| --- | ---: | ---: | ---: | ---: |
| A1-four | 27.752 / 27.831 | 27.569 / 27.816 | 2 / 3 | 0 / 2 |
| B1-three | 19.387 / 19.486 | 19.402 / 27.820 | 2 / 3 | 0 / 2 |
| B2-three | 27.748 / 27.822 | 19.407 / 19.488 | 0 / 0 | 0 / 0 |
| A2-four | 27.744 / 27.823 | 27.558 / 27.776 | 2 / 4 | 0 / 5 |

B2's lower latency with zero drains is session/phase variation, not evidence of
guard benefit. However, all six actual timing drains show immediate recovery:
compare frames `[drop−60, drop−5)` against `[drop+5, drop+60)` to exclude the
transition itself. Median enqueue→display latency fell from 27.61–27.80 ms to
19.28–19.43 ms in all six. `guard-recovery.json` retains these windows and
subsequent windows, while `physical-comparison.json` retains whole-run metrics.
The first A1 recovery rebounds within the following two-second window; other
recoveries last several seconds, and A2 later rebounds too. The ten-second
cooldown intentionally leaves such rebounds untreated until the next permitted
drain. Physical long gaps rise from zero throughout the control to 0–5 per
guard timing run; this cost includes deliberate drains and other stalls.

This validates that deliberate withholding can recover one refresh in the
native display path, but does not establish a stable whole-session latency
improvement or justify a default change. Keep the guard opt-in. The service and
runtime build, guard policy tests, GPU-reuse tests and IPC swapchain-wait tests
pass. Linux behavior is unchanged by the macOS-only implementation; Linux CI
was not run here.

Further analysis narrows the scheduling question. The default helper uses
`presentDrawable:afterMinimumDuration:8000 us`, not `atTime:`: the absolute
`metal_request_ns` logged in `present.csv` is not passed to Metal in this mode.
Apple defines the duration relative to the previous drawable's visible time.
The selected target is therefore a diagnostic phase reference, not an enforced
deadline, and the queue-depth diagnostic is not a measured drawable count.
Earlier hosted-probe work found that switching to `atTime:` did not cure its
latency ratchet; do not assume that API switch fixes this native case either.

Using the existing presentation-stall analyser's validated host-clock GPU-end
conversion, A1 frames `[1275,1330)` versus `[1340,1395)` have median
enqueue→GPU-end 3.339/3.342 ms, but GPU-end→physical 24.241/15.970 ms.
For the first recovery and rebound, `[140,195)` versus `[195,435)` have
enqueue→GPU-end 3.609/3.353 ms and GPU-end→physical 15.830/24.287 ms.
All 405 joined samples pass clock validation. The extra refresh
is after GPU completion, even at the rebound. These windows are retained in
`A1-four/latency-phase-clues.json`.

Present-offset calibration is a separate possible confounder: traced callbacks
feed an EMA of physical-minus-desired back into the fake pacer's predicted
display time. The A1 predicted-minus-desired median changes from about 25.02 ms
before the first drain to 16.90 ms after it, then 25.01 ms after the rebound.
This offset changes the app-facing prediction, not the compositor's desired
render deadline directly. It follows observed latency and is not independent
proof of correct prediction or evidence that it caused the rebound. A test that
freezes this calibration while retaining physical traces would isolate its
influence on client pacing; simply disabling traces also removes the measuring
instrument. Runtime defaults were not changed by this analysis.

A final 2400-frame four-image hardware smoke in
`/tmp/monado-guard-no-trace-20261012-codex` used a temporary copy of the isolated
fixture wrapper with `PSVR2_TIMING_TRACE=0`. The final build places guard
validation after normal job validation and preserves the old pacing-feedback
condition when there is no trace file; those changes do not alter the valid,
traced path compared above. The client passed, two drains were logged and no
CSV files, GPU errors or retirement errors were produced. Its manifest retains
the final binary/source hashes. This checks guard operation without tracing;
it does not measure physical latency or provide visual acceptance.

To repeat the matched trial, use fresh directories:

```sh
python3 scripts/macos/run-three-image-comparison.py --headset --frames 2400 \
  --service-env XRT_MACOS_DRAWABLE_SLOT=1 \
  --service-env XRT_MACOS_PRESENT_LATENCY_GUARD=0 \
  --output /tmp/monado-guard-off-new
python3 scripts/macos/run-three-image-comparison.py --headset --frames 2400 \
  --service-env XRT_MACOS_DRAWABLE_SLOT=1 \
  --service-env XRT_MACOS_PRESENT_LATENCY_GUARD=1 \
  --output /tmp/monado-guard-on-new
```

## Reproduction and headset gate

From the Monado checkout, after an incremental native build:

```sh
cmake --build build/arm64 --target tests_macos_service_gpu_sync --parallel 4
python3 scripts/macos/run-three-image-comparison.py \
  --output /tmp/monado-three-image-offscreen-new
```

The runner creates fresh private launchd/socket fixtures, checks actual image
counts and correctness, records binary hashes, and writes a compact
`comparison.json` with acquisition, readiness, renderer and fresh-latch timing.
Use a new output directory for each comparison.

Hardware runs are user-operated. Close other headset owners, stop the installed
Monado service if necessary, then run the same comparison wearing the headset:

```sh
python3 scripts/macos/run-three-image-comparison.py \
  --headset --frames 2400 --output /tmp/monado-three-image-headset-new
```

Record the commit and whether the panel remains stable, including head motion,
black frames, checkerboards and stalls. The wrapper rejects an unavailable
headset instead of accepting an offscreen fallback. This native test still
does not exercise the Windows/DXMT producer, SteamVR resolve handles, workload
or Game Mode. Follow the separate
[SteamVR trial](macos-psvr2-timing-diagnostics.md#three-image-steamvr-compatibility-prepared-2026-10-10)
after the native headset check. Compare fixed-allocation copying and actual
three-image zero-copy with their actual transport markers; changing both
transport and scheduler measures a deployment choice rather than the isolated
image-count cost.

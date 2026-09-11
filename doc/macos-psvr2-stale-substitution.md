# macOS PS VR2 legacy-worker stale-frame substitution

## Motivation

The 2026-09-11 legacy present-worker control was objectively close to a true 120 Hz presentation cadence and subjectively one of the best runs, but it had one narrow weakness: after a rare long `CAMetalLayer nextDrawable` stall (roughly 15 ms rather than the normal ~6.5–8.4 ms), a newer compositor frame could become pending while the worker was blocked. The worker could then remain one compositor frame behind for many subsequent 120 Hz presentations.

This experiment preserves the legacy worker's useful pacing and changes only that long-stall case.

A clean baseline also established an important experimental requirement: use deferred compositor GPU timestamp readback while judging cadence. Omitting `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1` caused roughly 3.7 ms of current-frame renderer blocking and produced misleading ~70–73 fps / ~8 ms renderer runs.

## Diagnostic mode

Enable:

```sh
XRT_MACOS_PRESENT_STALE_SUBSTITUTE=1
```

Requirements:

```sh
XRT_MACOS_ASYNC_PRESENT=1
XRT_MACOS_PRESENT_WORKER=1
XRT_MACOS_METAL_SHARED_EVENT_WAIT=1
XRT_MACOS_DRAWABLE_SLOT=0
XRT_MACOS_EARLY_DRAWABLE=0
XRT_MACOS_DEFER_GPU_TIMESTAMPS=1
```

The normal legacy worker behaviour is unchanged for ordinary drawable waits.

After `nextDrawable` returns, the worker checks whether:

1. the drawable acquisition took at least **1.25 measured display refreshes**; and
2. a newer pending compositor frame exists.

At ~119.88 Hz this is approximately 10.4 ms. The 1.25-refresh threshold deliberately avoids treating the normal ~8.35–8.4 ms waits seen in the corrected 120 Hz baseline as stale-frame events, while still selecting the abnormal ~15–16.8 ms stalls.

Only if both conditions are true does it:

- remove the newer pending frame from the one-deep worker queue;
- retire the stale active source image behind its render-complete timeline;
- reuse the already-acquired drawable for the newer frame;
- preserve the legacy worker's normal blocking `nextDrawable` position and cadence.

The substitution is deliberately restricted to the exported `MTLSharedEvent` path, so the replacement source can safely wait on its own render-complete timeline without a second CPU-side Vulkan wait.

## Why this differs from the acquire-first newest-frame worker

`XRT_MACOS_PRESENT_LATEST_FRAME=1` acquires a drawable first and chooses the newest suitable frame after acquisition. Its first headset test appeared to regress to ~71.5 fps with ~8 ms renderer duration.

That result is now known to be **confounded**: the run omitted `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1`, and later A/Bs showed that omission alone adds roughly 3.7 ms of blocking current-frame GPU timestamp readback and produces the same ~70–73 fps regime. The acquire-first architecture therefore still needs a clean rerun.

Stale substitution is more conservative because it leaves `nextDrawable` in exactly the legacy worker position and changes frame selection only after an exceptional stall has already occurred.

## Trace events

Existing `present_worker.csv` gains these event names when a substitution occurs:

- `stale_substitute_old`
- `stale_substitute_new`
- `active_stale_substituted`

With `PSVR2_TIMING_TRACE=1`, a new file is written:

```text
monado_psvr2_<pid>_stale_substitute.csv
```

Columns:

```text
event_ns,drawable_wait_ns,threshold_ns,old_frame_id,new_frame_id,
old_timeline_value,new_timeline_value,old_image_index,new_image_index,
old_enqueue_ns,new_enqueue_ns,new_source_age_ns
```

The `threshold_ns` column records the exact 1.25-refresh cutoff used.

## Successful headset result — 2026-09-11

`new-stale.zip` (PID 4385) behaved as intended and was subjectively reported as probably even better than the preceding known-good baseline.

Measured result:

- physical cadence ~119.48 fps;
- 12/3560 physical intervals >12 ms (~0.34%);
- renderer median ~4.239 ms;
- intentional late-render wait median ~4.163 ms;
- renderer residual after subtracting that wait ~0.073 ms;
- `nextDrawable` median ~6.699 ms;
- Metal commit-to-complete ~1.366 ms;
- exactly **9 stale substitutions**;
- all 9 waits were ~14.9–15.1 ms against the ~10.43 ms threshold;
- ordinary ~6–8.4 ms waits did not trigger;
- every substitution advanced the selected source by one frame, N → N+1.

The important backlog measurements improved dramatically versus the preceding corrected legacy baseline:

- `drawable_end` with a newer pending frame: **1,679 → 9**;
- desired-to-physical latency >30 ms: **1,708 frames (~41.1%) → 19 frames (~0.53%)**;
- the previous long ~33 ms latency episodes lasting hundreds of frames were replaced by brief ~two-frame recovery episodes back to the normal ~25 ms region.

Head angular speed was somewhat higher than in the preceding baseline, so the subjective improvement is not explained by gentler motion.

### Interpretation

The conservative threshold cleanly separates normal worker pacing from genuine drawable starvation. It preserves the near-120 Hz cadence while preventing a rare long drawable stall from leaving presentation one compositor frame behind indefinitely.

This is the **best current presentation baseline** and should be retained as the reference until another architecture demonstrates a measurable advantage.

## Reference command

```sh
PSVR2_FILTERED_LINEAR_PREDICTION=0 \
PSVR2_FULL_LINEAR_HORIZON=1 \
PSVR2_ACCELERATION_PREDICTION=1 \
PSVR2_CONTINUITY_PREDICTION=1 \
PSVR2_CONTINUITY_TAU_MS=4 \
PSVR2_CONTINUITY_LIMIT_MM=5 \
PSVR2_TIMING_TRACE=1 \
XRT_MACOS_LATE_RENDER_DESIRED_OFFSET_US=2000 \
XRT_MACOS_PRESENT_MIN_LEAD_US=2000 \
XRT_MACOS_PRESENT_PRELATCH_US=2000 \
XRT_MACOS_ASYNC_PRESENT=1 \
XRT_MACOS_METAL_SHARED_EVENT_WAIT=1 \
XRT_MACOS_PRESENT_WORKER=1 \
XRT_MACOS_PRESENT_STALE_SUBSTITUTE=1 \
XRT_MACOS_EARLY_DRAWABLE=0 \
XRT_MACOS_DRAWABLE_SLOT=0 \
XRT_MACOS_MAX_DRAWABLES=3 \
XRT_MACOS_SKIP_BLOCKING_GPU_TIMESTAMPS=0 \
XRT_MACOS_DEFER_GPU_TIMESTAMPS=1 \
./build-macos-psvr2-display/src/xrt/targets/service/monado-service
```

The next architecture A/B is the acquire-first `XRT_MACOS_PRESENT_LATEST_FRAME=1` mode with stale substitution disabled and deferred GPU timestamps explicitly enabled.

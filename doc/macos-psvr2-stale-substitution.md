# macOS PS VR2 legacy-worker stale-frame substitution

## Motivation

The 2026-09-11 legacy present-worker control was substantially smoother than the earlier acquire-first newest-frame worker experiment and was objectively close to a true 120 Hz presentation cadence.

The legacy worker has one narrow weakness: after a rare long `CAMetalLayer nextDrawable` stall (roughly 15 ms rather than the normal ~6.5–8.4 ms), a newer compositor frame can become pending while the worker is blocked. The worker then presents the old active frame and can remain one compositor frame behind for many subsequent 120 Hz presentations.

This experiment preserves the legacy worker's useful pacing and changes only that long-stall case.

A later clean baseline also established an important experimental requirement: use deferred compositor GPU timestamp readback while judging cadence. Omitting `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1` caused roughly 3.7 ms of current-frame renderer blocking and produced misleading ~70–73 fps / ~8 ms renderer runs. Those runs must not be used to judge the presentation architecture.

## New diagnostic mode

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

The earlier `XRT_MACOS_PRESENT_LATEST_FRAME=1` experiment acquired a drawable in a separate wrapper path and then handed that drawable into the existing Metal copy/present code. Active-frame supersession worked, but the first test appeared to show roughly doubled renderer time and mixed 60/120 Hz presentation.

That conclusion is now **confounded**: the test omitted `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1`, and later A/Bs showed that omission alone adds roughly 3.7 ms of blocking current-frame GPU timestamp readback and drives the compositor into the same ~70–73 fps regime. The acquire-first architecture therefore needs a clean rerun before it can be accepted or rejected.

The stale-substitution mode remains the more conservative next experiment because it does **not** move drawable acquisition. It leaves `nextDrawable` exactly where it was in the smooth legacy worker and substitutes only after an unusually long wait has already happened.

## Trace events

Existing `present_worker.csv` gains these event names when a substitution occurs:

- `stale_substitute_old`
- `stale_substitute_new`
- `active_stale_substituted`

A new file is also written when `PSVR2_TIMING_TRACE=1`:

```text
monado_psvr2_<pid>_stale_substitute.csv
```

Columns:

```text
event_ns,drawable_wait_ns,threshold_ns,old_frame_id,new_frame_id,
old_timeline_value,new_timeline_value,old_image_index,new_image_index,
old_enqueue_ns,new_enqueue_ns,new_source_age_ns
```

The `threshold_ns` column records the exact 1.25-refresh cutoff used for that run.

For the next headset run, the key questions are:

- does renderer time stay near the corrected baseline (~4.25 ms total, of which ~4.16 ms is the intentional late-render wait);
- does physical cadence remain near 120 Hz;
- do ordinary ~8.35–8.4 ms drawable waits remain untouched;
- do rare ~15–16.8 ms waits generate one `stale_substitute` row;
- does the substitution immediately prevent the prolonged one-frame backlog seen in the legacy control;
- does `presentedTime - desired_present` remain near the normal ~25 ms rather than stepping to ~33 ms for many frames;
- does source-image reuse remain effectively unblocked.

## Recommended next run

Use the corrected known-good predictor and presentation baseline, adding only stale substitution:

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

The earlier `XRT_MACOS_PRESENT_LATEST_FRAME` mode is not part of this experiment. It should be rerun separately with deferred GPU timestamps before drawing any further conclusion about that architecture.

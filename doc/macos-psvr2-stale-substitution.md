# macOS PS VR2 legacy-worker stale-frame substitution

## Motivation

The 2026-09-11 legacy present-worker control was substantially smoother than the earlier acquire-first newest-frame worker experiment and was objectively close to a true 120 Hz presentation cadence.

The legacy worker has one narrow weakness: after a rare long `CAMetalLayer nextDrawable` stall (roughly 15 ms rather than the normal ~6.5–7 ms), a newer compositor frame can become pending while the worker is blocked. The worker then presents the old active frame and can remain one compositor frame behind for many subsequent 120 Hz presentations.

This experiment preserves the legacy worker's useful pacing and changes only that long-stall case.

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
```

The normal legacy worker behaviour is unchanged for ordinary drawable waits.

After `nextDrawable` returns, the worker checks whether:

1. the drawable acquisition took at least one measured display refresh; and
2. a newer pending compositor frame exists.

Only if both are true does it:

- remove the newer pending frame from the one-deep worker queue;
- retire the stale active source image behind its render-complete timeline;
- reuse the already-acquired drawable for the newer frame;
- preserve the legacy worker's normal blocking `nextDrawable` position and cadence.

The substitution is deliberately restricted to the exported `MTLSharedEvent` path, so the replacement source can safely wait on its own render-complete timeline without a second CPU-side Vulkan wait.

## Why this differs from the failed newest-frame worker

The earlier `XRT_MACOS_PRESENT_LATEST_FRAME=1` experiment acquired a drawable in a separate wrapper path and then handed that drawable into the existing Metal copy/present code. Although active-frame supersession worked, drawable acquisition frequently became immediate, Vulkan renderer time roughly doubled, and physical presentation fell into a mixed 60/120 Hz cadence.

The new mode does **not** move drawable acquisition. It leaves `nextDrawable` exactly where it was in the smooth legacy worker and substitutes only after an unusually long wait has already happened.

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

For the next headset run, the key questions are:

- does normal `nextDrawable` remain ~6.5–7 ms and physical cadence remain ~120 Hz;
- do rare ~15 ms waits generate one `stale_substitute` row;
- does the substitution immediately prevent the prolonged one-frame backlog seen in the legacy control;
- does `presentedTime - desired_present` remain near the normal ~25 ms rather than stepping to ~33 ms for many frames;
- does source-image reuse remain effectively unblocked.

## Recommended first run

Use the same predictor and presentation settings as the smooth legacy control, adding only the new flag:

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
./build-macos-psvr2-display/src/xrt/targets/service/monado-service
```

The earlier `XRT_MACOS_PRESENT_LATEST_FRAME` mode is not part of this experiment.

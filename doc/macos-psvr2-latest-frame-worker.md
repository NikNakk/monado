# macOS PS VR2 newest-frame drawable worker experiment

## Rationale

The 2026-09-09 and 2026-09-11 traces demonstrate two failure modes around `CAMetalLayer::nextDrawable`:

- synchronous / slot-off acquisition blocks the compositor. In the slot-off A/B, every drawable wait over 1 ms was followed by a skipped desired-present interval; ordinary waits around 6.5-7 ms were enough to push the path beyond the 8.34 ms 120 Hz budget;
- the asynchronous one-drawable slot prevents that compositor block, but explicitly drops the current frame whenever a prefetched drawable is unavailable.

The existing present worker improves decoupling but still binds a presentation job **before** calling `nextDrawable`. If `nextDrawable` blocks, a newer compositor frame can arrive while the worker remains committed to the older active frame. When acquisition finally returns, that stale frame can still be presented.

`XRT_MACOS_PRESENT_LATEST_FRAME=1` adds an opt-in diagnostic mode intended to separate drawable availability from frame selection.

## Design

The mode requires:

```text
XRT_MACOS_ASYNC_PRESENT=1
XRT_MACOS_PRESENT_WORKER=1
XRT_MACOS_METAL_SHARED_EVENT_WAIT=1
```

and an exported Vulkan render-complete `MTLSharedEvent`.

The worker does the following:

1. take the current pending presentation job as an initial candidate;
2. call `nextDrawable` on the serial presentation worker, where it may block without blocking the compositor thread;
3. after the drawable arrives, atomically inspect the one-deep pending slot again;
4. if a newer frame is pending, select that newer frame for the acquired drawable and retire the older active source image;
5. encode the existing Metal shared-event wait, IOSurface-to-drawable blit, scheduled present and completion handling for the selected frame;
6. retire overwritten or superseded source images independently of the blocking drawable worker, using a Metal shared-event wait where available. This prevents stale source images from filling the three-image compositor pool while `nextDrawable` is blocked.

The existing modes remain available for A/B comparison. The new mode is disabled unless `XRT_MACOS_PRESENT_LATEST_FRAME=1` is explicitly set.

## Trace

With `PSVR2_TIMING_TRACE=1`, the mode adds:

```text
monado_psvr2_<PID>_latest_drawable.csv
```

Columns are:

```text
event,event_ns,drawable_ptr,acquire_begin_ns,acquire_end_ns,acquire_wait_ns,
initial_frame_id,selected_frame_id,superseded_frame_id,selected_timeline_value,
selected_image_index,selected_enqueue_ns,source_age_ns
```

Important events include:

- `acquire_begin` / `acquired`: duration and identity of each drawable acquisition;
- `supersede_active`: acquisition began for one frame but a newer pending frame was selected after the drawable became available;
- `selected`: frame actually bound to the acquired drawable;
- `execute_return` / `execute_error`: return from the existing Metal copy/present path.

`present_worker.csv` also gains mode-specific event names such as `latest_enqueued`, `latest_drawable_begin`, `latest_drawable_end`, `active_superseded_after_drawable`, and `superseded_pending` through the existing worker trace schema.

## First headset test

Rebuild the existing macOS PSVR2 display tree after pulling the branch:

```sh
cmake --build build-macos-psvr2-display --target comp_main monado-service --parallel 4
```

For the first A/B, retain the previous run-1 pre-latch value so the only intentional presentation-architecture change is the newest-frame worker:

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
XRT_MACOS_PRESENT_LATEST_FRAME=1 \
XRT_MACOS_EARLY_DRAWABLE=0 \
XRT_MACOS_DRAWABLE_SLOT=0 \
XRT_MACOS_MAX_DRAWABLES=3 \
./build-macos-psvr2-display/src/xrt/targets/service/monado-service
```

Use the same `hello_xr` invocation as the preceding tests.

## Expected evidence if the design is working

A useful run should show all of the following:

1. `nextDrawable` may still block for ~6-7 ms or occasionally longer, but frame generation/rendering should continue rather than inheriting that wait on the compositor thread.
2. During a long acquisition, `latest_enqueued` should continue to record newer frames.
3. When a newer frame arrived during the wait, `latest_drawable.csv` should show `selected_frame_id > initial_frame_id` and a corresponding `supersede_active` event.
4. The older source image should be retired without causing a large `image_reuse_wait_ns` or exhausting all three compositor images.
5. The selected frame's `source_age_ns` at binding should remain low compared with the legacy worker's stale active-frame age after long drawable waits.
6. `presentedTime` should ideally remain valid, allowing actual presentation cadence to be compared with the slot-off runs.

The primary perceptual question is not whether drawable waits disappear — CoreAnimation may still impose them — but whether those waits cease to produce the characteristic stale-frame hold/catch-up motion.

## Failure modes to watch

Stop and preserve the trace if any of these occur:

- service deadlock or compositor image acquisition stalls;
- repeated `latest_drawable_error` or `execute_error` events;
- source-image reuse waits grow to approximately a refresh or more;
- `selected_frame_id` stops advancing while newer jobs are being enqueued;
- `presentedTime` becomes consistently zero as it did in the one-drawable-slot experiment;
- visual output freezes while tracking/service logging continues.

This is deliberately a diagnostic architecture. If it improves cadence and perceptual stability, the next step should be to fold the logic cleanly into `comp_window_macos.m` rather than retaining the wrapper/include arrangement used to keep this experiment isolated and reversible.

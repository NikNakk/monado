# macOS PS VR2 newest-frame drawable worker experiment

## Rationale

The macOS traces demonstrate several distinct `CAMetalLayer::nextDrawable` failure modes:

- synchronous acquisition on the compositor thread blocks frame production and deterministically causes missed refreshes;
- the asynchronous one-drawable slot avoids that block but explicitly drops the current frame whenever a prefetched drawable is unavailable;
- the legacy off-thread present worker sustains near-120 Hz cadence, and conservative post-acquisition stale substitution now fixes its rare long-stall one-frame latency backlog.

`XRT_MACOS_PRESENT_LATEST_FRAME=1` is a more architectural alternative: decouple drawable availability from frame selection by acquiring a drawable first, then bind the newest suitable pending compositor frame.

Its first headset run appeared to regress to ~71.5 fps with ~8 ms renderer time. That result is now **invalid as an architecture comparison**, because the run omitted `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1`. Later A/Bs showed that omission adds roughly 3.7 ms of blocking current-frame Vulkan GPU timestamp readback and independently produces the same ~70–73 fps state.

The experiment therefore needs a corrected rerun against the current best stale-substitution baseline.

## Design

The mode requires:

```text
XRT_MACOS_ASYNC_PRESENT=1
XRT_MACOS_PRESENT_WORKER=1
XRT_MACOS_METAL_SHARED_EVENT_WAIT=1
```

and an exported Vulkan render-complete `MTLSharedEvent`.

For a clean A/B, also require:

```text
XRT_MACOS_PRESENT_STALE_SUBSTITUTE=0
XRT_MACOS_DEFER_GPU_TIMESTAMPS=1
```

The worker does the following:

1. take the current pending presentation job as an initial candidate;
2. call `nextDrawable` on the serial presentation worker, where it may block without blocking the compositor thread;
3. after the drawable arrives, atomically inspect the one-deep pending slot again;
4. if a newer frame is pending, select that newer frame for the acquired drawable and retire the older active source image;
5. encode the Metal shared-event wait, IOSurface-to-drawable blit, scheduled present and completion handling for the selected frame;
6. retire overwritten or superseded source images independently of the blocking drawable worker, using a Metal shared-event wait where available.

The mode is disabled unless `XRT_MACOS_PRESENT_LATEST_FRAME=1` is explicitly set.

## Trace

With `PSVR2_TIMING_TRACE=1`, the mode adds:

```text
monado_psvr2_<PID>_latest_drawable.csv
```

Columns:

```text
event,event_ns,drawable_ptr,acquire_begin_ns,acquire_end_ns,acquire_wait_ns,
initial_frame_id,selected_frame_id,superseded_frame_id,selected_timeline_value,
selected_image_index,selected_enqueue_ns,source_age_ns
```

Important events include:

- `acquire_begin` / `acquired` — drawable acquisition duration and identity;
- `supersede_active` — acquisition began for one frame but a newer pending frame was selected after drawable availability;
- `selected` — frame actually bound to the acquired drawable;
- `execute_return` / `execute_error` — result from Metal copy/present.

`present_worker.csv` also gains mode-specific events such as `latest_enqueued`, `latest_drawable_begin`, `latest_drawable_end`, `active_superseded_after_drawable`, and `superseded_pending`.

## Corrected headset rerun

Rebuild after pulling the branch:

```sh
cmake --build build-macos-psvr2-display --target comp_main monado-service --parallel 4
```

Use the same predictor, late-render and presentation settings as the current best baseline, but disable stale substitution and enable newest-frame selection:

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
XRT_MACOS_PRESENT_STALE_SUBSTITUTE=0 \
XRT_MACOS_PRESENT_LATEST_FRAME=1 \
XRT_MACOS_EARLY_DRAWABLE=0 \
XRT_MACOS_DRAWABLE_SLOT=0 \
XRT_MACOS_MAX_DRAWABLES=3 \
XRT_MACOS_SKIP_BLOCKING_GPU_TIMESTAMPS=0 \
XRT_MACOS_DEFER_GPU_TIMESTAMPS=1 \
./build-macos-psvr2-display/src/xrt/targets/service/monado-service
```

Use the same `hello_xr` invocation and broadly comparable head movement as the baseline tests.

## What constitutes success now

The current stale-substitution baseline is already strong: ~119.48 fps, ~0.34% physical intervals >12 ms, renderer ~4.239 ms, and almost complete elimination of the >30 ms backlog state. The acquire-first mode therefore needs to offer a real benefit rather than merely become functional.

A useful corrected run should show:

1. renderer median remains near ~4.2–4.3 ms with residual CPU renderer time near ~0.1 ms, confirming the timestamp confound is absent;
2. physical cadence stays near 120 Hz and >12 ms intervals do not materially exceed the stale-substitution baseline;
3. `latest_drawable.csv` shows sensible `selected_frame_id >= initial_frame_id` behavior and active supersession when newer frames arrive during acquisition;
4. source-frame age and desired-to-physical latency are at least as good as the stale-substitution baseline;
5. source-image reuse waits remain negligible and all three compositor images are not exhausted;
6. `presentedTime` remains valid;
7. subjectively, motion is at least as smooth as the stale-substitution baseline.

If it only matches the current baseline while adding substantially more machinery, the conservative stale-substitution worker remains the preferred design.

## Failure modes to watch

Preserve the trace if any of these occur:

- service deadlock or compositor image acquisition stalls;
- repeated `latest_drawable_error` or `execute_error` events;
- source-image reuse waits approach a refresh;
- `selected_frame_id` stops advancing while newer jobs are enqueued;
- `presentedTime` becomes consistently zero;
- visual output freezes while tracking/service logging continues.

This remains a diagnostic architecture. A genuinely superior result would justify folding the logic cleanly into `comp_window_macos.m`; otherwise the successful conservative legacy-worker stale substitution is the stronger current production direction.

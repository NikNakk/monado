# macOS PS VR2 judder evidence ledger

This is the short-form decision record for the visible head-motion judder / apparent backwards snap seen in the native macOS PS VR2 OpenXR path. It complements `macos-psvr2-timing-diagnostics.md`, `psvr2-position-prediction.md`, and `psvr2-continuity-prediction.md`.

The aim is to stop later work from repeatedly reopening hypotheses that have already been tested. **Unlikely** means the evidence substantially lowers a hypothesis, not that it is mathematically impossible. **Ruled in** means a real mechanism has been demonstrated at a magnitude capable of contributing to visible judder, not necessarily that it is the only cause.

## Current synthesis — 2026-09-11

The evidence no longer supports a primary macOS-specific PSVR2 tracking/SLAM defect. The strongest current evidence is downstream of pose selection, in compositor/presentation cadence and synchronization.

A crucial distinction is now established:

1. `nextDrawable` blocking **on the compositor thread** is harmful. In the slot-off synchronous path, a normal ~6.5–7 ms drawable wait plus ~4.2 ms of compositor work exceeds the 8.34 ms refresh budget and deterministically causes the next frame to skip a refresh.
2. `nextDrawable` blocking **on the separate legacy presentation worker** can be beneficial. With deferred GPU timestamp readback enabled, the compositor remains independent and the legacy worker has produced near-120 Hz physical presentation while it waits ~6.7–8.4 ms for a drawable off-thread.
3. A rare ~15–16.8 ms worker drawable stall can leave the presentation worker one compositor frame behind for many subsequent 120 Hz presentations. This is a latency/backlog problem rather than an ordinary-cadence problem.

Several recent ~70–73 fps / ~8 ms renderer runs were confounded by omission of `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1`. Without deferred readback, current-frame Vulkan GPU timestamp collection adds roughly 3.7 ms of blocking inside `comp_renderer_draw()`. Restoring deferred timestamps returned the renderer to ~4.245 ms total, of which ~4.163 ms was the intentional late-render wait, leaving only ~0.072 ms residual CPU-side renderer time. The same corrected run physically presented at ~119.2 fps and was subjectively one of the best runs so far.

Therefore the earlier acquire-first/newest-frame worker regression cannot currently be attributed confidently to Metal/Vulkan contention. That run showed the same ~3.7 ms renderer residual and must be repeated with deferred GPU timestamps before the architecture is accepted or rejected.

The one-refresh `predicted_display_time_ns - target_output_ns` concern remains substantially resolved. In slot-off measurements, `CAMetalDrawable.presentedTime` showed physical presentation itself normally occurs approximately one refresh after `target_output_ns`, while `predicted_display_time_ns` aligns closely with measured presentation.

Residual motion-dependent instability may remain after cadence is stable, but predictor work should continue to be judged only against a known-good ~120 Hz presentation baseline.

## Evidence ledger

| Hypothesis / intervention | Evidence | Current status |
| --- | --- | --- |
| PSVR2 is not being driven as a direct display, so ordinary desktop presentation is the missing architectural step | The macOS target already drives the directly connected PS VR2 display and the persistent judder remains. Direct display does not remove `CAMetalLayer` / CoreAnimation from the current presentation path. | **Direct-display omission ruled out**; macOS presentation path still in scope |
| The visible cadence is simply a gross 60 Hz compositor lock | A one-time pacer phase-sync defect was fixed and good runs subsequently reached approximately 120 Hz, but the head-motion judder remained. | **Gross 60 Hz lock ruled out as the continuing root cause** |
| Reducing the drawable pool from 3 to 2 will improve the experience by reducing buffering latency | Two drawables shortened measured CPU-to-display/presentation latency, but made the headset visually worse / more juddery. Three drawables added some buffering/latency but were visually more stable. | **Real latency-versus-stability trade-off**; changing pool depth alone does not solve the underlying presentation problem |
| Concurrent / earlier drawable acquisition will hide the problem | Concurrent acquisition was subjectively worse rather than better. Later diagnostics therefore separated early prefetch, worker, and slot modes rather than assuming more concurrency was beneficial. | **Disfavoured as an assumption; acquire-first worker needs a clean deferred-timestamp rerun** |
| Clamping prediction horizon to 10 or 20 ms will remove the snap | 10/20 ms prediction-cap A/Bs produced little subjective change. | **Disfavoured as main cause/fix** |
| ATW/distortion reprojection is itself the dominant cause | Disabling ATW did not produce the dramatic improvement expected if ATW were the primary source. | **Weakened, not absolutely excluded** |
| Every new 60 Hz SLAM publication causes the visible backwards jump | Measured new-SLAM correction steps were only modestly larger (roughly 5–10%) than ordinary frame-to-frame changes, not the large discontinuity implied by the visual symptom. | **Unlikely as primary explanation** |
| macOS receives materially older PSVR2 SLAM than Linux | 1000 Hz diagnostic: median SLAM interval 16.683 ms on both macOS and Ubuntu ARM64/Fusion; median first-seen latency ~22.95 ms macOS vs ~23.97–24.00 ms Linux/Fusion; p95 ~27.9 vs ~28.3 ms. | **Unlikely** |
| Generic PSVR2 `xrt_device_get_tracked_pose()` prediction behaves differently or reverses on macOS | 200 Hz 0/+5/+10/+15/+20 ms sweep was quantitatively very similar on macOS and Linux/Fusion after accounting for movement speed. Backwards movement across increasing horizons was negligible. | **Unlikely** |
| The Linux/Fusion comparison proves bare-metal Linux latency is identical | Linux was Ubuntu ARM64 in VMware Fusion on the same Mac with USB passthrough. It is an implementation reference, not a bare-metal latency benchmark. | **Not established**; retain this caveat |
| Copying the GAV player pose predictor will solve the judder | A GAV-style prediction implementation was tried on the headset. Subjectively it was no better; objective retrospective prediction was slightly worse than the Monado predictor work being tested. | **Disfavoured**; do not revisit without new evidence |
| Legacy positional dead reckoning uses the correct full SLAM-to-target horizon | Replay/code inspection found a real mismatch: gyro-only dead reckoning advances the relation timestamp through IMU samples without advancing position, then extrapolates position only over the shortened residual horizon. Full-horizon raw prediction dramatically improved retrospective position error versus the recorded shortened-horizon EMA path. | **Real tracking defect ruled in and fixed experimentally**, but **not sufficient to explain/solve visual judder** |
| EMA-filtered linear velocity is preferable | In long-horizon replay, full-horizon EMA was worse than full-horizon raw. Stronger smoothing generally traded prediction accuracy for lag. | **Disfavoured as the main fix** |
| Bounded translational acceleration solves the visible snap-back | Offline replay improved full-horizon raw modestly, especially p95/p99. Headset traces retain measurable positional error and visible judder despite acceleration. | **Useful predictor refinement, not root-cause fix** |
| Host-time continuity transition at SLAM updates solves visual snap-back | Offline 4 ms / 5 mm transition greatly reduced instantaneous model jumps. Headset traces show a modest accuracy trade-off versus acceleration alone and visible judder persists. | **May smooth one component; not sufficient and adds lag** |
| Orientation prediction is grossly wrong | Retrospective late-pose angular errors remain sub-degree at the median and around 1–1.6° at p95 depending on motion speed. Faster runs show larger error, but not catastrophic predictor failure. | **Secondary / unresolved, not leading explanation** |
| CoreVideo host-time and Monado monotonic clocks can be used directly | Early capture exposed a clock-domain defect. The branch now bridges Mach/CoreVideo host time into `CLOCK_MONOTONIC`. | **Real defect fixed** |
| `CVDisplayLink inOutputTime` should be fed directly as the last vblank | It is a future output timestamp; the target now projects it back to the most recent refresh boundary before pacing feedback. | **Real defect fixed** |
| Queue-wide Vulkan idle is necessary before Metal | Replaced by exact timeline-semaphore waits, then exported `MTLSharedEvent` GPU handoff when available. This removes avoidable CPU/GPU serialization. Judder persisted. | **Old synchronization path not root cause; async handoff retained** |
| Synchronous Metal `waitUntilCompleted` is required | Async-present experiments remove it and protect IOSurface reuse with in-flight tracking. Removing it alone did not eliminate judder. | **Not required; not root cause by itself** |
| Current-frame Vulkan GPU timestamp readback is harmless during timing experiments | Runs without `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1` showed ~3.7 ms extra renderer-internal blocking, ~8 ms total renderer duration and ~70–73 fps mixed cadence. Restoring deferred timestamp readback gave ~4.245 ms renderer duration, ~4.163 ms intentional late wait, ~0.072 ms residual and ~119.2 fps presentation. | **Ruled in as a major experimental confound**; keep deferred readback enabled for cadence A/Bs |
| Rendering generally exceeds the 120 Hz budget | Corrected deferred-timestamp runs render in ~4.2–4.3 ms total, almost all of which is the deliberate late-render wait; intrinsic CPU-side renderer overhead is tiny. | **Sustained render overload unlikely** |
| PSVR2 refresh / CVDisplayLink cadence is grossly unstable | Stable runs show ~8.3417 ms refresh intervals with very few genuine display-link outliers. | **Unlikely as primary cause** |
| Moving final pose/render work later is irrelevant | Desired-present-relative late-render tests changed behaviour; around +3000 us was previously favourable but became cadence-limited. | **Phase sensitivity exists; simple late wait is not a final solution** |
| Requesting Metal output one whole refresh early (N-1) is the right calibration | Earlier captures showed requests for N often landing N+1, motivating N-1. Later instrumentation showed the fixed one-refresh request was already in the past at the call site; it was replaced with a tunable pre-latch offset. | **Superseded diagnostic** |
| `XRT_MACOS_PRESENT_PRELATCH_US=2000` materially changes normal physical presentation timing | A clean slot-off A/B compared 2000 us (PID 29302) with 0 us (PID 30829). Median physical presentation remained ~one refresh after `target_output` in both: ~8.3425 vs ~8.3417 ms. Median presentation relative to desired remained ~16.685 vs ~16.684 ms. | **No evidence of a meaningful steady-state presentation-time effect**; not a root-cause fix |
| `PRELATCH_US=0` is objectively more jumpy than 2000 | The 0-us run was subjectively more jumpy, but objectively had fewer blocked drawable waits and fewer missed presentation intervals. Head rotation was substantially faster in that run, making each missed refresh much more visible. | **Subjective comparison confounded by motion speed**; repeat only if choosing a final default matters |
| `CAMetalDrawable.presentedTime` is unusable on the direct-display path | It was zero for every submitted frame in the nonblocking drawable-slot run, but became valid for 2599/2601 and 2719/2724 frames in the two slot-off runs. | **Usable in slot-off/worker path; slot mode specifically interferes with or changes presented-time reporting** |
| `CAMetalLayer nextDrawable` blocking on the compositor thread is harmless | Slot-off runs directly link blocking to cadence loss: every `nextDrawable` wait >1 ms was followed by a skipped desired-present interval. Run 1 had 89 such waits and 94 skipped intervals; run 2 had 64 waits and 71 skipped intervals. Most waits were ~6.5–7 ms; three ~15 ms waits occurred in run 1. | **Ruled in as a direct cadence-loss mechanism when it blocks the compositor** |
| `nextDrawable` blocking is always harmful, regardless of thread | The legacy present worker can block ~6.7–8.4 ms off-thread while the compositor independently sustains ~120 Hz. A corrected baseline physically presented at ~119.2 fps and was subjectively one of the best runs. | **Ruled out as a blanket statement**; thread placement and pipeline phase matter |
| The one-drawable nonblocking slot solves the blocking problem | Slot mode prevents compositor blocking, but when the prefetched drawable is unavailable it explicitly drops the current frame: 110 drops in the previous ~43 s run. | **Diagnostic only; trades blocking-induced skips for explicit drops** |
| The legacy worker has no downside once cadence is smooth | A rare ~15–16.8 ms drawable wait can make the worker remain one compositor frame behind for many subsequent 120 Hz presentations, raising desired-to-physical latency from ~25 ms to ~33 ms until a frame is superseded. | **Real rare-stall backlog mechanism ruled in** |
| The acquire-first/newest-frame worker intrinsically causes ~71 fps by creating Metal/Vulkan contention | The first newest-frame run showed ~8 ms renderer duration and ~71.5 fps, but later runs reproduced the same ~3.7 ms renderer residual simply by omitting deferred GPU timestamp readback. | **Previous conclusion superseded / unproven**; rerun with `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1` |
| `predicted_display_time_ns` is one refresh too late | In the slot-off runs, physical `presentedTime` is itself normally ~8.34 ms after `target_output`. Median `presentedTime - predicted_display_time` was about -0.058 ms (run 1) and -0.107 ms (run 2), with ~72–73% within 1 ms. | **Substantially ruled out**; predicted display is aligned with measured presentation, while `target_output` is one pipeline stage earlier |

## 2026-09-09 nonblocking drawable-slot experiment

This run used full-horizon translation, bounded acceleration, the 4 ms / 5 mm continuity transition, desired-relative late rendering at +2 ms, async Metal/shared-event handoff, the nonblocking drawable slot, and a three-drawable pool.

Key measurements:

- 5,193 compositor frames generated;
- 5,083 submitted to Metal;
- 110 explicit `drawable_slot_drop` frames;
- 2.12% overall drop rate, about 2.54/s after startup;
- ordinary asynchronous `nextDrawable` acquisition ~6.7 ms;
- 108 long acquisitions, mostly ~15 ms and two ~23 ms;
- all 110 dropped frames coincided with those long acquisitions;
- vblank median ~8.34175 ms and generally stable;
- renderer median ~4.24 ms, p99 ~5.06 ms;
- all 5,083 `presentedTime` samples were zero.

Interpretation: asynchronous prefetch prevents `nextDrawable` from blocking the compositor, but the single-slot design simply exposes drawable starvation as explicit frame drops. It is not a production solution.

The earlier 2-versus-3 drawable A/B is consistent with this result: a two-drawable pool reduced latency but also reduced buffering headroom and was visually worse. Three drawables remain the safer baseline while the underlying acquisition/scheduling mechanism is fixed.

## 2026-09-11 slot-off / pre-latch A/B

Two successive runs from `slot-experiment.zip` provide a cleaner comparison because the drawable slot was disabled in both. `drawable_prefetch.csv` contained only its header in both runs.

| Metric | Run 1 — PID 29302, pre-latch 2000 us | Run 2 — PID 30829, pre-latch 0 us |
| --- | ---: | ---: |
| Frames | 2,601 | 2,724 |
| Active capture time excluding long pause | ~22.48 s | ~23.32 s |
| `nextDrawable` waits >1 ms | 89 | 64 |
| Skipped desired-present intervals (>12 ms) | 94 | 71 |
| Actual presented intervals >12 ms | 90 | 67 |
| Approx active delivered cadence | 115.65 fps | 116.75 fps |
| Valid `presentedTime` | 2,599 / 2,601 | 2,719 / 2,724 |
| Median presented minus target | 8.3425 ms | 8.3417 ms |
| Median presented minus desired | 16.6847 ms | 16.6835 ms |
| Median presented minus predicted display | -0.0578 ms | -0.1071 ms |
| Median late-pose angular speed | ~0.568 rad/s | ~0.951 rad/s |
| Mean late-pose angular speed | ~0.773 rad/s | ~1.073 rad/s |
| Median retrospective late orientation error | ~0.295° | ~0.417° |
| p95 retrospective late orientation error | ~1.322° | ~1.590° |

### Direct blocking evidence

The strongest result is deterministic: **every drawable wait over 1 ms was followed by a skipped desired-present interval in both runs**. Waits over 5 ms accounted for 88/94 skips in run 1 and 64/71 in run 2.

The frame immediately before a skip had a median apparent renderer duration of ~11.05 ms in both runs, because the blocking drawable acquisition is on that compositor path. This exceeds the 8.34 ms refresh budget. The same compositor with off-thread presentation and deferred timestamp readback is normally ~4.2 ms. Therefore even the common ~6.5–7 ms `nextDrawable` stall is enough to force the next frame to miss one refresh when acquisition blocks the compositor.

Run 1 also contained three ~15 ms drawable waits; run 2's maximum was ~7.7 ms. Run 2 was therefore objectively **cleaner**, not worse, in frame cadence.

### Why run 2 nevertheless looked more jumpy

The headset was rotated substantially faster in run 2. Median angular speed was ~67% higher (0.951 vs 0.568 rad/s), and mean angular speed ~39% higher. A single 8.34 ms held frame therefore corresponds to roughly 0.45° of head rotation at run-2 median speed versus ~0.27° in run 1.

More importantly, the actual missed-refresh events happened during much faster motion in run 2: median angular speed at those events was ~1.06 rad/s versus ~0.30 rad/s in run 1. A simple angular-travel estimate across the held presentation interval gives a median of ~1.01° in run 2 versus ~0.29° in run 1. Thus fewer misses can plausibly look much more jumpy when they occur during faster turns.

The positional and orientation retrospective errors were also somewhat larger in run 2, consistent with the harder motion rather than a clear pre-latch regression.

### Presentation phase result

These runs resolve the previous one-refresh ambiguity. In both runs, `presentedTime` normally lands one refresh after `target_output_ns`, while `predicted_display_time_ns` is already approximately one refresh after `target_output_ns`. The two latter timestamps are closely aligned.

Therefore the previous observation that `predicted_display - target_output ≈ 8.34 ms` should **not** be treated as a one-frame pose overprediction. `target_output` is an earlier CoreVideo/Metal scheduling point and the measured CoreAnimation presentation pipeline adds approximately one refresh before the image appears.

Changing pre-latch from 2000 us to 0 us did not remove that pipeline delay and did not materially shift normal physical presentation time.

## 2026-09-11 present-worker and GPU-timestamp correction

The legacy present-worker control established that drawable blocking off the compositor thread can coexist with excellent cadence:

- physical presentation ~119.72 fps;
- median physical interval ~8.3417 ms;
- only ~0.13% of intervals >12 ms;
- renderer median ~4.237 ms;
- normal worker `nextDrawable` wait ~6.698 ms;
- rare ~15–16.6 ms waits could create a one-frame latency backlog while cadence stayed smooth.

The initial acquire-first/newest-frame worker then appeared to regress to ~71.5 fps with ~8.0 ms renderer duration. Subsequent stale-off/on and shared-event A/Bs reproduced a similar ~70–73 fps state. These runs all omitted the deferred GPU timestamp setting.

With `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1` restored, the corrected known-good baseline showed:

- renderer median ~4.245 ms;
- intentional late-render wait median ~4.163 ms;
- renderer residual after subtracting that wait ~0.072 ms;
- physical cadence ~119.2 fps;
- 24 / 4152 physical intervals >12 ms (~0.58%);
- Metal GPU duration ~1.153 ms;
- Metal commit-to-complete ~1.378 ms;
- subjectively one of the best runs so far.

This strongly identifies synchronous current-frame GPU timestamp readback as the source of the ~3.7 ms extra renderer cost in the bad runs. The prior explanation that immediate drawable acquisition intrinsically caused Vulkan/Metal contention is therefore weakened substantially and should not be used as a design conclusion without a corrected rerun.

The corrected baseline also changes the stale-substitution threshold choice. Many ordinary drawable waits were ~8.35–8.4 ms, just above one nominal refresh, whereas genuinely abnormal waits were separated around ~15–16.8 ms. The stale-substitution experiment therefore now uses **1.25 refreshes** (~10.4 ms at 119.88 Hz) rather than one refresh, so normal worker pacing is left untouched.

## What not to spend the next iteration on

Unless new evidence appears, do not make another wholesale pose-predictor replacement the next experiment. In particular, do not return to the GAV predictor, generic velocity-EMA tuning, prediction caps, changing drawable count alone, or the premise that macOS simply receives much older SLAM than Linux.

Do not conflate the three drawable strategies:

- synchronous acquisition on the compositor thread directly causes missed subsequent refreshes;
- the single nonblocking prefetch slot avoids blocking but explicitly drops frames when it is empty;
- the legacy off-thread worker can sustain near-120 Hz cadence, but rare very long drawable stalls can add a persistent one-frame latency backlog.

Do not diagnose a ~70 fps / ~8 ms renderer run unless `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1` is explicitly present (or blocking GPU timestamp collection is otherwise disabled). Those runs are not valid architecture comparisons.

Likewise, the approximately one-refresh `predicted_display - target_output` difference no longer needs to be investigated as a suspected pose-phase bug unless new evidence contradicts the measured `presentedTime` alignment.

## Highest-value next work

1. **Test conservative stale-frame substitution on the corrected known-good baseline.** Keep legacy worker timing unchanged and substitute only after a drawable wait of at least 1.25 measured refreshes when a newer pending compositor frame exists. Confirm ordinary ~8.35–8.4 ms waits are untouched and ~15–16.8 ms stalls are selected.
2. Determine whether substitution prevents the long-lived ~25 ms → ~33 ms desired-to-physical latency backlog without degrading ~120 Hz cadence, renderer timing, source-image reuse, or subjective smoothness.
3. **Rerun the acquire-first/newest-frame worker with `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1`.** Its previous ~71.5 fps result is confounded and no longer sufficient to reject that architecture.
4. Investigate why `presentedTime` is consistently valid with slot-off/worker acquisition but zero in the nonblocking slot experiment. That difference may reveal an important drawable-lifetime or CoreAnimation scheduling detail.
5. Keep three drawables as the current baseline. Do not return to two merely to reduce latency until cadence/back-pressure is solved.
6. Once presentation cadence is reliably 120 Hz without long-lived backlog, repeat controlled slow/fast yaw and translation. Only then decide whether residual orientation prediction, scanout phase, or per-scanline/rolling-shutter correction is perceptually important.

## Related detailed documents

- `doc/macos-psvr2-timing-diagnostics.md` — trace formats, compositor timing experiments, Linux/Fusion comparison, and runtime controls.
- `doc/macos-psvr2-stale-substitution.md` — conservative legacy-worker stale-frame substitution experiment and corrected test command.
- `doc/psvr2-position-prediction.md` — full-horizon translation defect, replay methodology, and bounded acceleration.
- `doc/psvr2-continuity-prediction.md` — host-time continuity transition and replay trade-offs.

Update this ledger whenever a headset A/B materially changes the status of a hypothesis above.

# macOS PS VR2 judder evidence ledger

This is the short-form decision record for the visible head-motion judder / apparent backwards snap seen in the native macOS PS VR2 OpenXR path. It complements `macos-psvr2-timing-diagnostics.md`, `macos-psvr2-stale-substitution.md`, `macos-psvr2-latest-frame-worker.md`, `psvr2-position-prediction.md`, and `psvr2-continuity-prediction.md`.

The aim is to stop later work from reopening hypotheses that have already been tested. **Ruled in** means a real mechanism has been demonstrated at a magnitude capable of contributing to visible judder, not necessarily that it is the only cause. Distinguish measured fact from inference, and keep motion-speed confounding in mind for subjective A/Bs.

## Current synthesis — 2026-09-11

The evidence no longer supports a primary macOS-specific PSVR2 tracking/SLAM defect. The strongest current explanation for the large visible discontinuities is presentation cadence / latency state, with smaller residual tracking-prediction error still possible once presentation is stable.

The best current presentation baseline is the **legacy off-thread present worker with conservative stale-frame substitution**, using deferred GPU timestamp readback. In the latest `new-stale.zip` run it physically presented at ~119.48 fps, with only 12/3560 physical intervals >12 ms (~0.34%), renderer median ~4.239 ms, intentional late-render wait ~4.163 ms, and no measurable steady-state regression versus the preceding known-good baseline.

The stale substitution fired exactly 9 times. All 9 were genuinely abnormal `nextDrawable` waits around ~14.9–15.1 ms, against the 1.25-refresh threshold of ~10.43 ms. Ordinary ~6–8.4 ms worker waits were untouched. Each substitution replaced frame N with N+1 after drawable acquisition.

This almost eliminated the legacy worker's long-lived one-frame latency backlog: `drawable_end` with a newer pending frame fell from 1,679 instances in the preceding baseline to 9, and desired-to-physical latency >30 ms fell from 1,708 frames (~41.1%) to 19 frames (~0.53%). The remaining stall episodes recovered to the normal ~25 ms desired-to-physical latency within about two presented frames rather than remaining near ~33 ms for hundreds of frames. Subjectively this was reported as probably better again despite somewhat faster head motion.

A major experimental confound is also established: cadence comparisons must keep `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1`. Omitting it causes current-frame Vulkan GPU timestamp readback to block inside `comp_renderer_draw()` for roughly 3.7 ms, inflating renderer duration from ~4.2 ms to ~8 ms and producing ~70–73 fps mixed cadence. This confounded the first acquire-first/newest-frame-worker run, so that architecture has **not** yet had a valid A/B against the corrected baseline.

A crucial thread-placement distinction is established:

1. `nextDrawable` blocking **on the compositor thread** is harmful: ordinary ~6.5–7 ms waits plus compositor work exceed the 8.34 ms budget and deterministically force missed refreshes.
2. `nextDrawable` blocking **on the separate presentation worker** can coexist with near-120 Hz cadence and provides useful natural pacing.
3. Rare ~15 ms worker waits can create a stale-frame latency backlog; conservative post-acquisition substitution fixes that failure mode without disturbing ordinary pacing.

The one-refresh `predicted_display_time_ns - target_output_ns` concern is substantially resolved. Physical `CAMetalDrawable.presentedTime` normally occurs approximately one refresh after `target_output_ns`, and `predicted_display_time_ns` aligns closely with measured physical presentation. Treat `target_output_ns` as an earlier presentation-pipeline scheduling point, not the physical scanout time.

## Evidence ledger

| Hypothesis / intervention | Evidence | Current status |
| --- | --- | --- |
| PSVR2 is not being driven as a direct display | The current macOS target already drives the directly connected headset; judder remained. | **Direct-display omission ruled out** |
| Gross 60 Hz compositor lock is the continuing cause | One-time pacer phase sync was fixed; good runs subsequently reach ~120 Hz. | **Ruled out as continuing root cause** |
| macOS receives materially older SLAM than Linux/Fusion | 1000 Hz runs: SLAM interval ~16.683 ms both; first-seen median ~22.95 ms macOS vs ~23.97–24.00 ms Linux/Fusion, p95 ~27.9 vs ~28.3 ms. | **Unlikely** |
| Linux/Fusion proves bare-metal Linux latency | Linux comparison was Ubuntu ARM64 in VMware Fusion with USB passthrough. | **Not established; retain caveat** |
| Generic PSVR2 pose prediction behaves differently/reverses on macOS | 200 Hz horizon sweep closely matched Linux/Fusion; backward movement across increasing horizons negligible. | **Unlikely** |
| Every 60 Hz SLAM publication causes the visible snap | New-SLAM correction steps only ~5–10% larger than ordinary frame-to-frame steps. | **Unlikely as primary cause** |
| Legacy translational prediction used the correct horizon | Position was extrapolated only over the shortened residual after IMU orientation integration. Full-horizon prediction markedly improved retrospective error. | **Real tracking defect ruled in and experimentally fixed; not sufficient to solve judder** |
| EMA velocity smoothing is preferable | Full-horizon EMA generally worsened retrospective prediction compared with raw velocity. | **Disfavoured** |
| Bounded translational acceleration solves the judder | Modest retrospective improvement, especially tails; visible judder persisted. | **Useful refinement, not root cause** |
| SLAM-update continuity transition solves the judder | 4 ms / 5 mm transition reduces model discontinuity but adds a small lag/error trade-off; visual judder persisted. | **Useful smoothing option, not sufficient** |
| Orientation prediction is grossly wrong | Retrospective errors are sub-degree median and roughly 1–2° in upper tails depending on motion. | **Secondary / unresolved, not leading explanation** |
| GAV-style predictor is the answer | Tried on headset; subjectively no better and retrospective prediction slightly worse. | **Disfavoured** |
| 10/20 ms prediction caps solve the snap | Little subjective change. | **Disfavoured** |
| ATW/distortion reprojection itself dominates | Disabling ATW did not give the expected dramatic improvement. | **Weakened** |
| CoreVideo/Mach timestamps can be used directly as Monado monotonic | Early trace exposed a clock-domain error; branch now bridges host time to monotonic. | **Real defect fixed** |
| `CVDisplayLink inOutputTime` is the previous vblank | It is a future output time; code now projects to the most recent refresh boundary before pacing feedback. | **Real defect fixed** |
| Queue-wide Vulkan idle is required before Metal | Replaced by exact timeline/shared-event handoff. | **Old synchronization path unnecessary; not root cause** |
| Synchronous Metal `waitUntilCompleted` is required | Async present works with in-flight/source-image protection. | **Not required** |
| Blocking current-frame GPU timestamp readback is harmless | Omitting deferred readback adds ~3.7 ms renderer blocking and drives mixed ~70–73 fps cadence. | **Major experimental confound ruled in; keep `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1` for A/Bs** |
| Rendering intrinsically exceeds 120 Hz budget | Corrected runs show ~4.2–4.3 ms total `comp_renderer_draw()`, almost all intentional late-render wait; residual CPU renderer time ~0.07 ms. | **Sustained renderer overload unlikely** |
| PSVR2/CVDisplayLink refresh is unstable | Stable runs show ~8.3417 ms refresh with few genuine outliers. | **Unlikely as primary cause** |
| Two drawables are preferable | Lower latency but subjectively more juddery / less stable. | **Latency/stability trade-off; keep 3 as baseline** |
| `PRELATCH_US=2000` materially shifts normal physical presentation | 2000 vs 0 us slot-off A/B showed essentially unchanged physical presentation phase. | **Not a root-cause fix** |
| `PRELATCH_US=0` is intrinsically more jumpy | Subjective worse run had substantially faster head movement and objectively fewer cadence misses. | **Subjective comparison motion-confounded** |
| `presentedTime` is unusable on this direct-display path | Valid for almost every slot-off/worker frame; zero in the one-drawable-slot run. | **Usable in worker/slot-off path; slot behavior remains interesting** |
| `nextDrawable` blocking on compositor thread is harmless | Every >1 ms wait in clean slot-off A/B was followed by skipped desired cadence; ~6.5–7 ms common waits were enough. | **Direct compositor-thread cadence-loss mechanism ruled in** |
| `nextDrawable` blocking is always harmful | Legacy worker blocks off-thread for ~6.7–8.4 ms while compositor can sustain ~120 Hz. | **Ruled out as blanket statement; placement/phase matters** |
| One-drawable nonblocking slot solves acquisition | Avoids compositor block but turned starvation into 110 explicit frame drops (~2.1%). | **Diagnostic only** |
| Legacy worker has no downside once cadence is smooth | Rare ~15–16.8 ms waits can leave it one compositor frame behind for hundreds of frames, increasing latency ~25 → ~33 ms. | **Rare-stall backlog ruled in** |
| Conservative stale substitution fixes the legacy backlog | 1.25-refresh threshold; latest run: 9/9 triggers only on ~14.9–15.1 ms stalls, cadence ~119.48 fps, >12 ms intervals ~0.34%, >30 ms latency 41.1% → 0.53%, `newer_pending` 1679 → 9. | **Strongly supported; best current presentation baseline** |
| Acquire-first/newest-frame worker intrinsically causes ~71 fps via Metal/Vulkan contention | First run had ~8 ms renderer / ~71.5 fps, but it omitted deferred GPU timestamps and exhibited the same ~3.7 ms residual subsequently identified as timestamp blocking. | **Previous rejection superseded / unproven; clean rerun required** |
| `predicted_display_time_ns` is one refresh too late | Physical `presentedTime` itself normally sits ~one refresh after `target_output_ns`; median physical minus predicted was close to zero in clean slot-off runs. | **Substantially ruled out** |

## Key presentation experiments

### Nonblocking drawable slot — 2026-09-09

With a one-drawable asynchronous slot, 5,193 compositor frames produced 5,083 Metal submissions and 110 explicit `drawable_slot_drop` events (~2.12%, ~2.54/s). Ordinary acquisition was ~6.7 ms; 108 long acquisitions were mostly ~15 ms with two ~23 ms. All 110 drops coincided with these long acquisitions. Renderer remained ~4.24 ms and vblank ~8.3417 ms. This proved that moving acquisition off the compositor avoids blocking but a one-slot design merely converts starvation into current-frame drops.

### Slot-off synchronous acquisition / pre-latch A/B — 2026-09-11

With acquisition back on the compositor path, every `nextDrawable` wait >1 ms was followed by a skipped desired-present interval. Waits >5 ms accounted for 88/94 skips in the 2000-us pre-latch run and 64/71 in the 0-us run. Apparent renderer time before skips was ~11.05 ms because ~6.5–7 ms drawable blocking was added to ~4.2 ms compositor work. `presentedTime` showed physical presentation approximately one refresh after `target_output_ns` and closely aligned to `predicted_display_time_ns`. The 0-us run looked worse subjectively but involved much faster head motion.

### Legacy present worker and timestamp correction — 2026-09-11

A clean legacy worker control physically presented at ~119.72 fps despite worker `nextDrawable` median ~6.698 ms. It exposed the rare-stall backlog: ~15–16.6 ms waits could add one refresh of latency while cadence remained smooth, and the worker would not necessarily catch up until a later supersession.

Several subsequent ~70–73 fps runs, including the first acquire-first/newest-frame worker experiment, omitted `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1`. Restoring deferred timestamp readback gave renderer median ~4.245 ms, intentional late wait ~4.163 ms, residual ~0.072 ms, physical cadence ~119.2 fps, 24/4152 intervals >12 ms (~0.58%), Metal GPU duration ~1.153 ms, and Metal commit-to-complete ~1.378 ms. This identifies synchronous current-frame timestamp readback as the ~3.7 ms extra renderer cost and invalidates the prior contention-based rejection of acquire-first.

### Conservative stale substitution — 2026-09-11

The threshold was raised from 1.0 to **1.25 measured refreshes** because the corrected baseline had many normal ~8.35–8.4 ms waits but genuinely abnormal waits clustered around ~15–16.8 ms.

In `new-stale.zip` (PID 4385):

- physical cadence ~119.48 fps;
- 12/3560 physical intervals >12 ms (~0.34%);
- renderer median ~4.239 ms;
- intentional late-render wait ~4.163 ms;
- renderer residual ~0.073 ms;
- `nextDrawable` median ~6.699 ms;
- Metal commit-to-complete ~1.366 ms;
- exactly 9 stale substitutions;
- all 9 drawable waits ~14.9–15.1 ms, above the ~10.43 ms threshold;
- ordinary waits did not trigger;
- each substitution advanced N → N+1;
- `drawable_end` with `newer_pending` fell from 1,679 in the preceding baseline to 9;
- desired-to-physical latency >30 ms fell from 1,708 frames (~41.1%) to 19 (~0.53%);
- long-lived ~33 ms backlog episodes were replaced by ~two-frame recovery episodes back to ~25 ms;
- head movement was somewhat faster than the preceding baseline, so the subjective improvement is not explained by gentler motion.

Interpretation: conservative stale substitution preserves the useful legacy worker pacing while fixing its demonstrated rare-stall backlog. Treat this as the current best presentation baseline unless a cleaner architecture beats it.

## What not to spend the next iteration on

Do not return to wholesale pose-predictor replacement, GAV, velocity EMA, prediction caps, two drawables, or the premise that macOS simply gets older SLAM without new evidence.

Do not conflate drawable strategies: compositor-thread blocking causes misses; the one-slot prefetch converts starvation to drops; the legacy worker supports smooth cadence; conservative stale substitution fixes the worker's rare long-stall backlog.

Do not interpret a ~70 fps / ~8 ms renderer run unless deferred GPU timestamp readback is explicitly enabled or blocking timestamp collection is otherwise disabled.

Do not reopen the `predicted_display - target_output` one-refresh gap as a pose-timing bug unless new physical-presentation evidence contradicts `presentedTime`.

## Highest-value next work

1. **Rerun the acquire-first/newest-frame worker with `XRT_MACOS_DEFER_GPU_TIMESTAMPS=1` and stale substitution disabled.** Its previous failure is confounded and no longer sufficient to reject it.
2. Compare it directly against the current stale-substitution baseline: physical cadence, >12 ms intervals, renderer residual, drawable acquisition distribution, supersession count, source-frame age, desired-to-physical latency distribution, source-image reuse, and subjective smoothness.
3. Require acquire-first to offer a real benefit over the current baseline; merely reaching 120 Hz is no longer enough if it increases jitter or latency.
4. Investigate why `presentedTime` is valid in worker/slot-off acquisition but zero in the one-drawable-slot experiment if that distinction remains relevant after the architecture A/B.
5. Once presentation architecture is settled, repeat matched slow/fast yaw and translation before deciding whether residual orientation prediction, scanout phase, or rolling-shutter/per-scanline correction is perceptually important.

## Related detailed documents

- `doc/macos-psvr2-timing-diagnostics.md` — trace formats, compositor timing experiments, Linux/Fusion comparison, and runtime controls.
- `doc/macos-psvr2-stale-substitution.md` — current conservative stale-frame substitution implementation and test procedure.
- `doc/macos-psvr2-latest-frame-worker.md` — acquire-first/newest-frame experiment; must use deferred GPU timestamp readback for the corrected rerun.
- `doc/psvr2-position-prediction.md` — full-horizon translation defect, replay methodology, and bounded acceleration.
- `doc/psvr2-continuity-prediction.md` — host-time continuity transition and replay trade-offs.

Update this ledger whenever a headset A/B materially changes the status of a hypothesis above.

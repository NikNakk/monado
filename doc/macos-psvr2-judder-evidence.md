# macOS PS VR2 judder evidence ledger

This is the short-form decision record for the visible head-motion judder / apparent backwards snap seen in the native macOS PS VR2 OpenXR path. It complements `macos-psvr2-timing-diagnostics.md`, `psvr2-position-prediction.md`, and `psvr2-continuity-prediction.md`.

The aim is to stop later work from repeatedly reopening hypotheses that have already been tested. **Unlikely** means the evidence substantially lowers a hypothesis, not that it is mathematically impossible. **Ruled in** means a real mechanism has been demonstrated at a magnitude capable of contributing to visible judder, not necessarily that it is the only cause.

## Current synthesis — 2026-09-09

The evidence no longer supports a primary macOS-specific PSVR2 tracking/SLAM defect. The strongest current evidence is downstream of pose selection, in the macOS compositor/presentation path.

The present working model is at least two superimposed effects:

1. a possible approximately one-refresh pose/presentation phase error during continuous head motion; and
2. discrete `CAMetalLayer` drawable-starvation episodes that hold an old frame for one or more refreshes and then catch up.

The second mechanism is now directly demonstrated by the drawable-slot experiment. The first remains a high-priority hypothesis rather than a proven cause.

## Evidence ledger

| Hypothesis / intervention | Evidence | Current status |
| --- | --- | --- |
| PSVR2 is not being driven as a direct display, so ordinary desktop presentation is the missing architectural step | The macOS target already drives the directly connected PS VR2 display and the persistent judder remains. Direct display does not remove `CAMetalLayer` / CoreAnimation from the current presentation path. | **Direct-display omission ruled out**; macOS presentation path still in scope |
| The visible cadence is simply a gross 60 Hz compositor lock | A one-time pacer phase-sync defect was fixed and good runs subsequently reached approximately 120 Hz, but the head-motion judder remained. | **Gross 60 Hz lock ruled out as the continuing root cause** |
| Reducing the drawable pool from 3 to 2 will improve the experience by reducing buffering latency | Two drawables shortened measured CPU-to-display/presentation latency, but made the headset visually worse / more juddery. Three drawables added some buffering/latency but were visually more stable. | **Real latency-versus-stability trade-off**; changing pool depth alone does not solve the underlying presentation problem |
| Concurrent / earlier drawable acquisition will hide the problem | Concurrent acquisition was subjectively worse rather than better. Later diagnostics therefore separated early prefetch, worker, and slot modes rather than assuming more concurrency was beneficial. | **Disfavoured** |
| Clamping prediction horizon to 10 or 20 ms will remove the snap | 10/20 ms prediction-cap A/Bs produced little subjective change. | **Disfavoured as main cause/fix** |
| ATW/distortion reprojection is itself the dominant cause | Disabling ATW did not produce the dramatic improvement expected if ATW were the primary source. | **Weakened, not absolutely excluded** |
| Every new 60 Hz SLAM publication causes the visible backwards jump | Measured new-SLAM correction steps were only modestly larger (roughly 5–10%) than ordinary frame-to-frame changes, not the large discontinuity implied by the visual symptom. | **Unlikely as primary explanation** |
| macOS receives materially older PSVR2 SLAM than Linux | 1000 Hz diagnostic: median SLAM interval 16.683 ms on both macOS and Ubuntu ARM64/Fusion; median first-seen latency ~22.95 ms macOS vs ~23.97–24.00 ms Linux/Fusion; p95 ~27.9 vs ~28.3 ms. | **Unlikely** |
| Generic PSVR2 `xrt_device_get_tracked_pose()` prediction behaves differently or reverses on macOS | 200 Hz 0/+5/+10/+15/+20 ms sweep was quantitatively very similar on macOS and Linux/Fusion after accounting for movement speed. Backwards movement across increasing horizons was negligible. | **Unlikely** |
| The Linux/Fusion comparison proves bare-metal Linux latency is identical | Linux was Ubuntu ARM64 in VMware Fusion on the same Mac with USB passthrough. It is an implementation reference, not a bare-metal latency benchmark. | **Not established**; retain this caveat |
| Copying the GAV player pose predictor will solve the judder | A GAV-style prediction implementation was tried on the headset. Subjectively it was no better; objective retrospective prediction was slightly worse than the Monado predictor work being tested. | **Disfavoured**; do not revisit without new evidence |
| Legacy positional dead reckoning uses the correct full SLAM-to-target horizon | Replay/code inspection found a real mismatch: gyro-only dead reckoning advances the relation timestamp through IMU samples without advancing position, then extrapolates position only over the shortened residual horizon. Full-horizon raw prediction dramatically improved retrospective position error versus the recorded shortened-horizon EMA path. | **Real tracking defect ruled in and fixed experimentally**, but **not sufficient to explain/solve visual judder** |
| EMA-filtered linear velocity is preferable | In long-horizon replay, full-horizon EMA was worse than full-horizon raw. Stronger smoothing generally traded prediction accuracy for lag. | **Disfavoured as the main fix** |
| Bounded translational acceleration solves the visible snap-back | Offline replay improved full-horizon raw modestly, especially p95/p99. Latest headset trace: mean/median positional error ~1.47/1.08 mm versus ~1.68/1.29 mm for raw. Visual judder remained. | **Useful predictor refinement, not root-cause fix** |
| Host-time continuity transition at SLAM updates solves visual snap-back | Offline 4 ms / 5 mm transition greatly reduced instantaneous model jumps. Latest headset trace slightly worsened retrospective positional accuracy versus acceleration alone (~1.58/1.16 mm mean/median vs ~1.47/1.08 mm), and visual judder persisted. | **May smooth one component; not sufficient and adds lag** |
| Orientation prediction is grossly wrong | Latest retrospectively scored late-render queries: median angular error ~0.35°, mean ~0.45°, p95 ~1.15°. There is room for improvement in fast turns, but this is not evidence of catastrophic predictor failure. | **Secondary / unresolved, not leading explanation** |
| CoreVideo host-time and Monado monotonic clocks can be used directly | Early capture exposed a clock-domain defect. The branch now bridges Mach/CoreVideo host time into `CLOCK_MONOTONIC`. | **Real defect fixed** |
| `CVDisplayLink inOutputTime` should be fed directly as the last vblank | It is a future output timestamp; the target now projects it back to the most recent refresh boundary before pacing feedback. | **Real defect fixed** |
| Queue-wide Vulkan idle is necessary before Metal | Replaced by exact timeline-semaphore waits, then exported `MTLSharedEvent` GPU handoff when available. This removes avoidable CPU/GPU serialization. Judder persisted. | **Old synchronization path not root cause; async handoff retained** |
| Synchronous Metal `waitUntilCompleted` is required | Async-present experiments remove it and protect IOSurface reuse with in-flight tracking. Removing it alone did not eliminate judder. | **Not required; not root cause by itself** |
| Rendering generally exceeds the 120 Hz budget | Latest drawable-slot run: renderer median ~4.24 ms, p99 ~5.06 ms; only 10 frames exceeded ~8.34 ms. There are occasional large spikes, but not enough to explain persistent judder. | **Sustained render overload unlikely**; rare spikes still relevant |
| PSVR2 refresh / CVDisplayLink cadence is grossly unstable | Latest run median interval ~8.34175 ms; only a handful of >12 ms intervals in >5,200 samples. | **Unlikely as primary cause** |
| Moving final pose/render work later is irrelevant | Desired-present-relative late-render tests changed behaviour; around +3000 us was previously the favourable region but became cadence-limited. This shows render/presentation phase matters, while a simple late wait consumes remaining frame budget. | **Phase sensitivity ruled in; simple late wait not a final solution** |
| Requesting Metal output one whole refresh early (N-1) is the right calibration | Earlier captures showed requests for N often landing N+1, motivating N-1. Later instrumentation showed the fixed one-refresh request was already in the past at the call site; it was replaced with a tunable pre-latch offset. | **Superseded diagnostic**, not a final scheduling model |
| `XRT_MACOS_PRESENT_PRELATCH_US=2000` has correct Metal semantics | Current code passes `target_output - prelatch` to `presentDrawable:atTime:`. That time is a requested presentation time, not a latch deadline. Latest trace also shows GPU completion often around/after that early request while usually before the actual target. | **Questionable; test `PRELATCH_US=0` as a clean A/B** |
| `CAMetalDrawable.presentedTime` gives usable physical presentation feedback here | In the latest run all 5,083 `presented` callbacks reported `presentedTime == 0`, so measured-present-offset feedback received no usable samples. | **Current calibration mechanism ineffective on this path**; actual scanout time remains unmeasured |
| `CAMetalLayer nextDrawable` blocking is harmless / unrelated to judder | Drawable-slot run: 5,193 frames generated, 5,083 submitted, exactly 110 `drawable_slot_drop` frames (~2.12%; ~2.54/s steady state). Normal `nextDrawable` waits ~6.7 ms; 108 pathological waits were ~15 ms, with two ~23 ms. Every dropped frame occurred during one of those long waits. | **Ruled in as a real discrete frame-loss mechanism** |
| The one-drawable nonblocking slot itself is the fix | The slot deliberately converts a would-have-blocked `nextDrawable` into an explicit frame drop. It exposes starvation rather than curing it. | **Diagnostic only** |
| `predicted_display_time_ns` is aligned with the Metal/CoreVideo target | In the latest settled trace, `predicted_display_ns` is ~8.3–8.6 ms later than `target_output_ns`, approximately one 120 Hz refresh. This could be intentional semantics (for example different scanout reference points) or a true one-frame overprediction. | **High-priority unresolved hypothesis** |

## Latest drawable-slot experiment

The 2026-09-09 run used full-horizon translation, bounded acceleration, the 4 ms / 5 mm continuity transition, desired-relative late rendering at +2 ms, async Metal/shared-event handoff, no present worker, the nonblocking drawable slot, and a three-drawable pool.

Key measurements:

- 5,193 compositor frames generated;
- 5,083 submitted to Metal;
- 110 explicit `drawable_slot_drop` frames;
- 2.12% overall drop rate, about 2.54/s after startup;
- ordinary `nextDrawable` acquisition ~6.7 ms;
- 108 long acquisitions, mostly ~15 ms and two ~23 ms;
- all 110 dropped frames coincide with those long acquisitions;
- vblank median ~8.34175 ms and generally stable;
- renderer median ~4.24 ms, p99 ~5.06 ms;
- acceleration positional prediction mean/median ~1.47/1.08 mm;
- continuity output mean/median ~1.58/1.16 mm;
- late-pose orientation retrospective median ~0.35°, p95 ~1.15°;
- all 5,083 `presentedTime` samples were zero;
- settled `predicted_display_ns - target_output_ns` is approximately one refresh.

Interpretation: this directly demonstrates a presentation-side starvation mechanism capable of producing whole-frame judder independently of small pose-prediction errors. It does **not** yet prove that eliminating drawable starvation will remove continuous motion instability, because the approximately one-refresh pose/output phase discrepancy remains unresolved.

The earlier 2-versus-3 drawable A/B is consistent with this result: a two-drawable pool reduced latency but also reduced buffering headroom and was visually worse. Three drawables are therefore the safer current baseline while the underlying starvation/scheduling mechanism is investigated.

## What not to spend the next iteration on

Unless new evidence appears, do not make another wholesale pose-predictor replacement the next experiment. In particular, do not return to the GAV predictor, generic velocity-EMA tuning, prediction caps, changing drawable count alone, or the premise that macOS simply receives much older SLAM than Linux.

Likewise, do not treat the nonblocking drawable slot as a production solution: dropping rather than blocking was deliberately chosen to expose hidden back-pressure.

## Highest-value next A/Bs

1. Run the same configuration with `XRT_MACOS_DRAWABLE_SLOT=0`. The expected diagnostic contrast is explicit slot drops versus ~15/23 ms blocking `nextDrawable` stalls.
2. With other variables fixed, test `XRT_MACOS_PRESENT_PRELATCH_US=0` against 2000 because `presentDrawable:atTime:` specifies presentation time rather than a pre-latch deadline.
3. Instrument the semantic relationship among `desired_present_time_ns`, `predicted_display_time_ns`, `target_output_ns`, CoreVideo output time, and the headset DP frame/line counters. Determine whether the ~one-refresh difference is intentional scanout/reference-point semantics or true overprediction.
4. Establish an actual presentation/scanout timing reference that does not depend on `presentedTime`, since it is zero on the current direct-display path.
5. Only after presentation phase is understood, revisit orientation prediction or per-scanline/rolling-shutter correction if residual motion-dependent instability remains.

## Related detailed documents

- `doc/macos-psvr2-timing-diagnostics.md` — trace formats, compositor timing experiments, Linux/Fusion comparison, and runtime controls.
- `doc/psvr2-position-prediction.md` — full-horizon translation defect, replay methodology, and bounded acceleration.
- `doc/psvr2-continuity-prediction.md` — host-time continuity transition and replay trade-offs.

Update this ledger whenever a headset A/B materially changes the status of a hypothesis above.

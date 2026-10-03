<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# macOS PS VR2 judder evidence ledger

> **Image-reuse fix confirmed, 2026-10-03.** The user confirms the local Metal
> GPU reuse guard fixes partial black frames/black noise. Retain it alongside
> fresh scanout target poses, which the previous moving runs confirm remove
> regular judder. Occasional stalls remain unresolved. No new quantitative
> capture accompanies this confirmation. See the
> [visual confirmation](macos-psvr2-timing-diagnostics.md#user-confirms-image-reuse-correction).

> **User confirmation, 2026-10-03.** The fixed moving-head UE pair is "so much
> better": regular judder is gone, occasional stalls remain. Actual renderer
> poses match fresh queries throughout the matched subset. Both shared-event
> handoffs are active; reported partial black frames/noise prompts a separate
> correction enabling compositor-GPU completion protection before local Metal
> source-image reuse. Build/tests and actual timeline-wait checks pass, but its
> visual effect remains unvalidated. See the
> [confirmation and reuse evidence](macos-psvr2-timing-diagnostics.md#moving-head-confirmation-and-local-metal-image-reuse--2026-10-03).

> **Target-pose fix, 2026-10-03.** macOS compute rendering now retains fresh
> scanout poses instead of replacing them with recent submitted projection poses.
> A low-cost UE preflight verifies 609/609 actual renderer/query orientations
> agree within 0.001 degrees; all macOS build/tests and Python diagnostics pass.
> Moving-head subjective validation remains pending, and presentation stalls
> still occur. No predictor replacement or depth change was made; non-macOS
> behaviour is retained. See the
> [fix and next continuous-Game-Mode pair](macos-psvr2-timing-diagnostics.md#fresh-scanout-target-fix--2026-10-03).

> **UE workload A/B, 2026-10-03.** The low-cost preset increases application FPS
> from roughly 10–12 to 56–63 in matched observed Game Mode portions. The user
> reports smaller movement jumps, but they persist; B's voice cues arrived late,
> so its Game Mode remained active throughout. Presentation and tracking stalls
> worsen under that workload. Crucially, an existing compute-renderer branch
> overwrites fresh target poses with submitted projection poses for sufficiently
> recent layers. Source and effective target orientations are identical on 96.8%
> of B's matched frames despite `do_timewarp=1`. Earlier query-pose agreement does
> not establish actual renderer pose agreement. Correct this selection on macOS
> before assigning all regular judder to translation or replacing the predictor.
> See the [A/B and effective timewarp evidence](macos-psvr2-timing-diagnostics.md#ue-workload-ab-results-and-effective-timewarp--2026-10-03).

> **Recovered moving capture, 2026-10-03.** UE 70746/service 70890 presents at
> 117.59 Hz while UE supplies roughly 10–12 new frames/s. Shared pose queries
> remain fast; 11 physical gaps reach >=20 ms and completed frames wait up to
> 107 ms after GPU end, including one large hitch without a sensor gap and with
> CA callbacks on cadence. The user still reports very jumpy head movement,
> perhaps improved. Rotation-only warp also leaves source-frame view-centre
> translations uncorrected (new-source steps median/p95 6.65/25.31 mm), a
> plausible contributor to regular source-refresh judder, not proof of its
> visual cause. The next distinction is presentation stalls versus source
> refresh/parallax, rather than another wholesale predictor change. See the
> [capture evidence](macos-psvr2-timing-diagnostics.md#recovered-second-moving-head-capture--2026-10-03).

This is the short-form decision record for the visible head-motion judder / apparent backwards snap seen in the native macOS PS VR2 OpenXR path. It complements `macos-psvr2-timing-diagnostics.md`, `macos-psvr2-stale-substitution.md`, `macos-psvr2-latest-frame-worker.md`, `psvr2-position-prediction.md`, and `psvr2-continuity-prediction.md`.

The aim is to stop later work from reopening hypotheses that have already been tested. **Ruled in** means a real mechanism has been demonstrated at a magnitude capable of contributing to visible judder, not necessarily that it is the only cause. Distinguish measured fact from inference, and keep motion-speed confounding in mind for subjective A/Bs.

> **Update, 2026-09-30.** Presentation has moved on since the synthesis
> below. The drawable-slot newest-frame worker (the acquire-first design in
> "Highest-value next work") replaced stale substitution as the default, with 0
> slot drops at about 119.88 Hz, and stale substitution and the
> CAMetalDisplayLink modes have been removed. Current defaults are at the top of
> [macos-psvr2-timing-diagnostics.md](macos-psvr2-timing-diagnostics.md).
> Judder under Game Mode has a separate cause (the service is throttled); see
> [macos-client-compositor-design.md](macos-client-compositor-design.md).

> **Motion feedback, 2026-10-03.** All user-run UE configurations still felt
> unsmooth; CA with client compositing was probably best. The user made mostly
> rotational movements, whereas agent captures had a static headset. Service
> Retrospective IMU/SLAM/pose correlation now confirms a service presentation
> delay under Game Mode: median 40–52 ms beyond the pose target, with physical
> orientation disagreement p95 4.8–5.8 degrees against later SLAM, versus about
> 0.28–0.31 degrees during priority-97 menu/recovery frames. Hosted requested-time
> pose disagreement is much smaller (CA/CV p95 0.30/0.43 degrees), but their
> physical presenter files are empty. A fully buffered moving-head hosted capture
> is needed to distinguish remaining display delay from prediction/content
> effects. See the [motion correlation](macos-psvr2-timing-diagnostics.md#retrospective-correlation-of-user-rotational-ue-runs--2026-10-03).
> Static cadence results do not establish visual rotational smoothness.

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
| CADisplayLink is inherently worse than CVDisplayLink | Initial 2026-10-03 A/B favoured CV, but CA projected its target using 8.333333 ms instead of its actual 8.341708 ms interval, deriving a vblank one refresh too old. Corrected cv/ca/ca/cv: CV 119.73–119.76 Hz, 0.10–0.12% physical intervals >12 ms; CA 119.62–119.76 Hz, 0.10–0.22%. Hosted CV/CA: 118.74/118.63 Hz, 0.58/0.71%. All six clients exit cleanly after correcting runtime descriptor ownership. See the [corrected timing record](macos-psvr2-timing-diagnostics.md#corrected-ca-phaseperiod-and-repeated-headset-ab--2026-10-03). | **Original rejection superseded; CA closely matches CV in native service/hosted captures. Hosted UE follow-up favours CA in completion logs (112.76 vs 109.26 Hz), but both have only ~7.5 new app frames/s and event-wait timeouts; physical presenter CSVs are empty. Game Mode is user-confirmed (suspended while the middle-third Cmd+Esc menu is open). A fifth actual service-CA run reproduces ~17.7 Hz under Game Mode and 119.03 Hz during the menu interval. The user confirms intentionally heavy UE rendering. CA is now default, with CV retained as fallback; Fully buffered hosted UE repeats physically favour CA (105.18 vs 100.08 Hz; 13.86 vs 19.49% intervals >12 ms) at matched 6.61 app frames/s, with priority 97 retained. Subjective/90 Hz and direct Game Mode flag coverage remain pending.** |
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
| Blocking current-frame GPU timestamp readback is harmless | Omitting deferred readback adds ~3.7 ms renderer blocking and drives mixed ~70–73 fps cadence. | **Major experimental confound ruled in; deferred readback is now unconditional on macOS (`XRT_MACOS_DEFER_GPU_TIMESTAMPS` removed)** |
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


## CA callback thread experiment — 2026-10-03

**Final status: failed and removed.** The initial assessment below is historical
and was superseded by the repeated callback-body and deferred comparisons.
See the [integration record](macos-cadisplaylink-owned-compositor-experiment.md).

`codex/cadisplaylink-compositor` tests frame execution on CADisplayLink's own
thread with the existing drawable/presentation worker. An immediate variant
changed render phase by about 5.33 ms and performed worse; it was replaced by a
phase-matched variant retaining the ordinary pacer wait. This variant achieved
117.61 Hz, 1.83% physical intervals >12 ms and query-to-display p50/p95
19.59/19.63 ms. Ordinary CA controls varied from 119.53 Hz / 0.15% misses / 13.06
new app frames/s to 102.15 Hz / 17.23% misses / 6.96 new app frames/s. Callback
mode delivered 12.33 new app frames/s. This is an unresolved trade-off, not a
settled rejection or superiority claim. Retain it only as an opt-in branch
experiment pending matched Game Mode/window state and buffered moving-head runs.
The new `ca_callback.csv` records every callback; eight callback-frame overruns
versus 86 long physical intervals in the phase-matched run reinforce that link
and downstream presentation misses must be separated. See the
[full experiment record](macos-psvr2-timing-diagnostics.md#initial-headset-results-and-phase-correction).


### Five-pair follow-up — 2026-10-03

Five usable static-headset captures per mode, alternating ordinary CA pacing
and phase-matched CA callback compositing, favour ordinary pacing overall:
mean physical cadence **108.96 vs 102.03 Hz**, mean >12 ms interval percentages
**10.47% vs 17.47%**. Callback compositing is clearly worse in three adjacent
pairs, essentially tied in one, better in one. Its median-of-run query-to-display
p99 is shorter (27.97 vs 36.40 ms), so a latency-tail trade-off remains.
Callback work p99 reaches 11.84 ms (median across runs), with 520–969 callbacks
per run overrunning CA's next target. Ordinary timestamp-only callbacks remain
prompt even when the separate compositor/presentation pipeline falls behind.

Keep ordinary CA pacing as default; these results do not justify promoting
callback compositing. This is separate from CA versus CV. All executing threads
retain policy 2 / priority 97, but Game Mode was unconfirmed and app throughput
varied. Four empty-buffer attempts were repeated; post-window debugger flushing
preserved the remaining captures. Fixed AB order and some early concurrent
analysis limit causal conclusions. Static traces do not settle motion smoothness.
See the [complete method and results](macos-psvr2-timing-diagnostics.md#five-alternating-pairs-with-fully-buffered-traces--2026-10-03).


### Deferred CA rendering and retirement — 2026-10-03

Moving GPU readiness and pose retrieval off CA, then rendering immediately on
its run loop, makes that render critical section short (median-of-run p99
0.328 ms). Five alternating pairs still show no display benefit: mean physical
cadence 119.14 vs 119.36 Hz, mean >12 ms intervals 0.542% vs 0.369%, and typical
run query-to-display p99 24.19 vs 19.64 ms. The revision also renders about
125.3 times/s for roughly 119 Hz physical output. Retire the tested dispatch
variants and their environment switch; retain CA pacing, client-hosted Monado
and the stage diagnostics. This does not reject all conceivable CA-thread
scheduling designs, and cross-series workload variation prevents treating the
higher rates than the earlier series as a causal improvement.

Shared-event readiness waits are active in all captures. A separate policy
check confirms the service is externally backgrounded and all sampled service
threads remain priority 4 while UE's compositor threads retain 97. Hosted pose
retrieval still depends on synchronous IPC into that service. The next useful
architecture work is timestamped shared tracking state with local prediction,
including publication-age checks; keep client-hosted rendering for Game Mode.
See the [full decision and evidence](macos-psvr2-timing-diagnostics.md#deferred-run-loop-results-and-retirement).

### Shared tracking follow-up — 2026-10-03

`codex/macos-shared-tracking` now implements the proposed ingestion-published
PS VR2 snapshot and equivalent client-local future prediction, opt-in with
`XRT_MACOS_SHARED_TRACKING=1` in a client-hosted compositor. Five alternating
static UE captures per path cut median run pose-query p99 from 0.185 to 0.016 ms
and worst measured query from 31.197 to 0.132 ms. All 51,002 measured local
queries avoided RPC fallback. This establishes a successful transport change.
It does **not** establish a smoothness fix: mean physical Hz favours local
115.44 vs IPC 113.84, while median favours IPC 119.65 vs local 117.81, and
median query→physical p99 is worse locally (27.94 vs 19.63 ms). Large GPU-fence
waits and occasional source-age gaps remain. Mapped IMU-age p99 is typically
1.53 ms but reached 75.93 ms. Service threads still sample at priority 4.
Keep the path opt-in pending moving-head tests. General space-overseer IPC
remains; do not claim that all application tracking calls are now local.
The final importer additionally preserves equal-timestamp gyro samples;
its separate verification follows the original ten-run comparison. See the
[full run table and method](macos-psvr2-timing-diagnostics.md#shared-tracking-and-client-local-prediction--2026-10-03).

Moving-head shared-tracking validation is prepared for the user to run later.
The version-2 trace adds actual sensor receipt/device-clock evidence and returned
poses. A static preflight verified buffering and exact physical pose-target joins;
movement and confirmed Game Mode transitions remain untested. Earlier fully
buffered claims require qualification because some writers still flushed
periodically; these are now corrected. See the
[capture protocol and preflight evidence](macos-psvr2-timing-diagnostics.md#prepared-moving-head-freshness-capture--2026-10-03).

The first buffered moving-head shared-tracking capture is now analysed. Local
pose queries remain fast (p99 0.011 ms) during substantial rotation and observed
service background/menu policy transitions. Internal SLAM disagreement p95 is
0.209 degrees at the pose target and 0.226 degrees at physical presentation,
but rare receipt gaps reach 63 ms and physical gaps 142 ms. The largest physical
hitch occurs after Metal completion while CA callbacks remain on cadence,
with matching drawable backpressure. The user confirms Game Mode in the first
and last thirds and reports improvement, but persisting judder of a different
quality. Keep the path opt-in. Next isolate delayed presentation of completed frames,
rather than treating this result as proof that service acquisition must move.
See the [moving-head evidence](macos-psvr2-timing-diagnostics.md#first-buffered-moving-head-shared-tracking-run--2026-10-03).

Completed-frame presentation diagnostics now join GPU completion to physical
output, with passive scheduled-callback and service AppKit-pump traces. The
moving-head run has 94 frames displayed >20 ms after GPU end, while the largest
pause clears after two old frames rather than producing a persistent latency
ratchet. Capture now acknowledges flushes before UE teardown and checks source
health before measurement. Static lifecycle checks pass; the next moving-head
capture is prepared. See the
[investigation and capture command](macos-psvr2-timing-diagnostics.md#completed-frame-presentation-investigation--2026-10-03).

### Wine Underture moving-head feedback and existing traces — 2026-10-03

The user confirms the experimental in-process Wine/OpenComposite path displays
Underture, with high subjective FPS but high-frequency motion judder described
as every other frame stepping backwards. Existing `/tmp/monado_psvr2_77379_*`
traces identify `OpenComposite_Underture` and provide 59.876 seconds of focused
measurement after startup/end trimming. Application deliveries average 52.77/s;
physical output averages 62.57/s, with physical intervals median/p95
16.683/33.366 ms despite near-119.88 Hz display-link callbacks. Compositor GPU
time median/p95 is 0.909/2.079 ms, while GPU completion to physical output is
38.421/57.535 ms. Physical output trails its compositor pose target by
9.891/33.799 ms median/p95. No compositor or application source frame IDs
reverse in physical presentation order. These measurements establish uneven
presentation and pose-target delay, not a predictor defect or a proven cause
of the alternating perceived step. Image/pose correspondence remains unverified.

The first response overlooked these existing traces and cited console worker
completion counts from a different agent run; use the physical timestamps above
for the user's run. The complete client traces, analyser and JSON summary are
archived in the bridge checkout at
`.build/in-process-openxr-study/docs/results/underture-in-process-2026-10-03/user-run/`.
No Monado source or pacing changes were made in this investigation.

#### Underture lifecycle diagnosis — 2026-10-03

Joining the existing PID 77379 client traces isolates long drawable availability
waits (median/p95 16.099/30.453 ms) and completion-to-physical delay
(38.354/57.426 ms), with regular CA callbacks (8.342/8.377 ms). These do not
establish which presentation stage causes the backlog. There is also a concrete
thread-lifecycle defect: 33 bootstrap compositor samples use policy 2 / priority
97; after OpenComposite recreates its session for the application device,
7,674 samples use policy 1 / priority 31. The Mach policy update caches its period
process-wide, skipping application on a new compositor thread at the same
refresh period. Realtime diagnostic thread identity is also cached globally, so
its repeated thread ID is not evidence of continuity. Source frame IDs and both
source/display target times remain monotonic in physical order; this is not proof
of an alternating image/pose mismatch.

An unapplied thread-local state correction is prepared at
`.build/in-process-openxr-study/docs/proposals/monado-compositor-thread-policy.patch`;
source snapshots, OpenComposite recreation log and lifecycle analysis are in
the bridge's `docs/results/underture-in-process-2026-10-03/user-run/`. No Monado
source/pacing change has been applied; user approval is required by the explicit
Monado-change constraint. Restore correct thread initialization before evaluating
its measured or subjective impact.

#### Thread-policy patch applied and compiled — 2026-10-03

The user approved the correction. Commit `1b400a8e7` makes scheduling and
realtime diagnostic caches thread local; see
[the build and verification note](macos-compositor-thread-policy.md). ARM64
service/client and x86-64 client compile, with matching revision tags and IPC
headers. The extracted-source lifecycle probe verifies real non-default Mach
policy on each of three replacement threads on both architectures; the old
global-cache negative control fails on its second thread. This supersedes the
"unapplied" status above. Service installation/restart and Underture performance
revalidation remain pending.

## Black corruption recurrence — 2026-10-03

After the inherited diagnostic cleanup, the user reports black corruption
again. The reuse fix remains in source and the rebuilt runtime; its actual
waits in the latest launch are not yet verified. A comparison with pixel
readbacks restored and reuse logging enabled is pending. This reopens the
visual corruption question for the cleanup build without invalidating the
earlier successful run. See the
[timing record](macos-psvr2-timing-diagnostics.md#black-corruption-recurrence-after-cleanup--2026-10-03).

The readback-on comparison removed black noise subjectively and confirmed
453 reuse waits/completions. Source samples were absent; target samples were
active. A stronger explicit macOS output-image barrier now replaces the
ordinary TOP_OF_PIPE destination without restoring readbacks. Its headset
validation is pending; see the
[barrier correction](macos-psvr2-timing-diagnostics.md#readback-on-result-and-explicit-output-barrier--2026-10-03).

The user subsequently confirmed “Yes that fixed it” for the explicit output
barrier with readbacks off. Black noise is therefore visually resolved for
this follow-up. Keep image-reuse protection and the output barrier; do not
restore unconditional diagnostic readbacks. Rapid-rotation edge borders
remain a separate reported phenomenon.

> **Review follow-up:** the native-wrapper `XRT_MACOS_WSI` prototype and credit
> limit are now removed. Only `XRT_MACOS_WSI_GENERIC` remains executable.
> Commands and flags below describe historical captures; use
> [the current migration and validation note](macos-wsi-migration.md) for new runs.

> **Historical evidence:** captures and performance conclusions in this report use
> the older `08619006b` native base. They do not compare WSI with the latest
> `macos-upstream-clean`. See [the migration record](macos-wsi-migration.md)
> for the current source, preserved native behavior and validation status.

<!-- SPDX-License-Identifier: BSL-1.0 -->
# Interleaved native / calibrated WSI comparison

Date: 15 October 2026. Branch `experiment/macos-wsi`, base `08619006b`.
Native remains the default; no installed runtime change or upstream publication.

## Why repeat the comparison

The previous native-versus-CV table compared separate capture batches. This
follow-up interleaves native and calibrated WSI using the same binaries, hardware,
display state, runner and analysis. It measures whether higher presentation
cadence is accompanied by fresh client frames and better latency tails.

Two targets, 90/120 Hz, baseline / sixteen scene passes per eye / injected CPU
stalls, three rotated-order repeats: 36 captures of 12 seconds after session
readiness, excluding two seconds after the first valid projection. The CPU
control delays application `xrEndFrame` by 20 ms every 90 application frames;
the compositor remains free to reproject an older frame. Four separate CPU-load
Metal-validation captures cover both targets at both rates. No runtime source
changes, builds or regression-test runs overlap the performance captures.

The native policy retains its existing asynchronous presentation and 8 ms
minimum-duration setting. WSI uses the standard generic FIFO swapchain, CV phase
updates and separate offset calibration. This compares complete target policies,
not the isolated GPU cost of a final blit. No claim about that cost follows.

`compare.py` recomputes strict summaries and pairs captures by refresh/load/repeat.
It rejects missing/duplicate captures, differing workload environment variables,
unequal analysis warm-up and failed display/service restoration. Differences are
reported as WSI minus native; three pairs are not enough for confidence intervals.

## Application-frame metrics

The existing `application_source_age_at_present_ms` field subtracts the layer's
OpenXR display/pose timestamp from reported actual presentation. It is useful
for reprojection timing, but is **not** elapsed time since physical rendering or
GPU completion. The new paired analysis keeps that distinction explicit.

Additional joins connect each presented system frame to its source client frame,
then to service-observed semaphore readiness and layer delivery:

- **Ready to present:** actual presentation minus the service's observed ready
  semaphore timestamp. This is a CPU observation of readiness, not an exact GPU
  hardware completion timestamp.
- **Delivery to present:** actual presentation minus the service's recorded client
  layer-delivery event; includes remaining GPU wait and presentation scheduling.
- **Unique client frames/s:** number of distinct presented client frame IDs minus
  the initial boundary frame, divided by the observed presentation span.
- **Presented reuse:** consecutive reported presentations using the same client
  frame ID, recomputed after joining feedback. It differs from reuse counted at
  every compositor latch, since native can supersede unpresented frames.

These joins explicitly require one active client slot and reject readiness after
presentation. They measure frame availability and reuse, not perceived world
stability, pixel equality or motion-to-photon latency.

## Integration boundary audit

| Area | Current WSI coverage | Work before full replacement |
| --- | --- | --- |
| Ordinary service window | Shares `comp_macos_frontend`, layer configuration and PS VR2 selection | User visual comparison, then lid-closed control |
| Timing | Standard fallback pacer, opt-in CVDisplayLink phase, opt-in calibration | Extract shared platform source; keep the fixed GOOGLE clock bridge separate |
| Hosted client compositor | Shared frontend has hosted support, but hosted factory still constructs native target | Route a WSI factory with the client's hosted handle and acknowledge the correct presentation ticket from observed presentation feedback |
| Hosted first-presentation handoff | Native frontend currently takes an `MTLDrawable` and installs its presented handler | Add a presentation acknowledgement abstraction with safe ticket/client lifetimes; WSI does not expose its drawable directly |
| Passthrough | Camera-backed compositing remains in native Metal presentation; WSI intentionally omits it | Share or redesign the passthrough render pass before making WSI eligible for that workload |
| Refresh changes | Reads existing physical mode; runner changes/restores it externally | Share physical refresh enumeration/request and reset pacer, CV source, filter and clock state on recreation |
| Display loss / sleep | No exercised generic-target recovery | Define stop/recreate behavior and test lifecycle transitions |
| Rendering API scope | Tested with Metal OpenXR client through existing IPC | Vulkan-client workload and synchronization tests |

The ordinary shared window frontend can already host layers from other clients.
That is distinct from using WSI **inside** the hosted client compositor; the latter
still selects the native target. No assertion of missing all hosted behavior is
intended.

A focused cleanup should extract the CV platform timing source and timestamp
projection into a shared macOS component, while retaining a separate adapter for
the existing CADisplayLink experiment. Both presenters should consume the same
small timing snapshot. Keep presentation mechanisms, optional calibration and
passive diagnostics separate. Add synthetic tests for invalid timestamps and
multi-period phase projection before changing native timing internals.

## Results and evidence

All 36 interleaved performance captures and four separate CPU-load Metal API
validation captures passed. Runtime sources and binary hashes match the saved
snapshots. All four validation logs confirm Metal API validation enabled.

The new comparison qualifies the earlier separate-batch result: WSI's higher
presentation rate is reproducible under GPU load, but at 90 Hz it presents fewer
**distinct client frames**, with worse client-frame readiness tails.

### Cadence and client frames

| Hz / load | Target | Presents/s | Distinct client frames/s | Presented reuse (%) |
| --- | --- | --- | --- | --- |
| 90 / baseline | Native | 88.22–89.01 | 67.53–70.30 | 20.29–23.68 |
| 90 / baseline | WSI + CV + calibration | 89.01–89.51 | 68.63–70.40 | 21.33–22.87 |
| 90 / cpu | Native | 88.72–88.91 | 67.41–68.23 | 23.07–24.08 |
| 90 / cpu | WSI + CV + calibration | 88.52–89.61 | 66.76–68.03 | 23.89–25.17 |
| 90 / gpu16 | Native | 80.23–82.24 | 71.45–73.55 | 10.55–12.23 |
| 90 / gpu16 | WSI + CV + calibration | 87.82–88.92 | 56.82–65.46 | 26.18–35.26 |
| 120 / baseline | Native | 115.19–117.19 | 85.13–86.57 | 24.83–27.33 |
| 120 / baseline | WSI + CV + calibration | 115.07–117.98 | 88.51–89.79 | 22.85–24.47 |
| 120 / cpu | Native | 117.18–117.78 | 82.69–87.68 | 25.34–29.41 |
| 120 / cpu | WSI + CV + calibration | 115.66–116.37 | 87.28–90.01 | 22.53–24.98 |
| 120 / gpu16 | Native | 102.10–103.55 | 47.79–48.39 | 52.76–53.79 |
| 120 / gpu16 | WSI + CV + calibration | 113.45–115.87 | 50.28–52.51 | 53.67–56.56 |

### Timing tails

| Hz / load | Target | Sample age p99 (ms) | Client ready → present p99 (ms) | Client ready → sample p99 (ms) | Absolute prediction error p99 (ms) |
| --- | --- | --- | --- | --- | --- |
| 90 / baseline | Native | 36.22–36.24 | 45.26–47.32 | 13.90–15.15 | 11.11–11.12 |
| 90 / baseline | WSI + CV + calibration | 36.11–36.23 | 45.91–49.42 | 18.56–20.71 | 7.28–11.10 |
| 90 / cpu | Native | 36.20–36.25 | 49.16–56.79 | 20.97–31.63 | 11.12–11.12 |
| 90 / cpu | WSI + CV + calibration | 25.26–36.27 | 47.71–58.27 | 21.61–27.47 | 5.55–11.12 |
| 90 / gpu16 | Native | 36.30–37.04 | 48.68–50.07 | 13.49–20.20 | 11.04–11.30 |
| 90 / gpu16 | WSI + CV + calibration | 36.34–36.36 | 58.87–61.23 | 33.12–33.84 | 11.12–11.12 |
| 120 / baseline | Native | 27.93–27.95 | 37.27–38.39 | 10.87–11.04 | 6.92–8.34 |
| 120 / baseline | WSI + CV + calibration | 27.96–32.23 | 39.57–52.73 | 15.15–30.08 | 7.84–8.93 |
| 120 / cpu | Native | 27.93–27.94 | 44.34–52.41 | 24.59–32.80 | 6.51–7.34 |
| 120 / cpu | WSI + CV + calibration | 27.96–36.21 | 45.63–52.69 | 25.27–27.27 | 8.33–10.24 |
| 120 / gpu16 | Native | 36.25–36.27 | 51.09–54.81 | 23.03–23.21 | 12.55–13.01 |
| 120 / gpu16 | WSI + CV + calibration | 27.98–28.03 | 49.79–53.55 | 23.06–30.61 | 8.34–8.34 |

### Matched differences (WSI minus native)

| Hz / load | Presents/s difference | Presentation-rate change (%) | Sample age p99 difference (ms) |
| --- | --- | --- | --- |
| 90 / baseline | -0.00–1.29 | -0.00–1.46 | -0.13–0.01 |
| 90 / cpu | -0.20–0.70 | -0.22–0.79 | -10.95–0.03 |
| 90 / gpu16 | 5.58–8.69 | 6.79–10.83 | -0.69–0.06 |
| 120 / baseline | -0.79–1.31 | -0.67–1.12 | 0.02–4.28 |
| 120 / cpu | -1.52–-1.27 | -1.30–-1.08 | 0.03–8.28 |
| 120 / gpu16 | 10.61–13.78 | 10.24–13.49 | -8.27–-8.24 |

Ranges are minima/maxima across the three captures, not confidence intervals.
A negative sample-age difference favors WSI; a positive rate difference favors
WSI. Quantiles of separate pipeline components must not be added together.

### What the matched data establishes

- **90 Hz, GPU load:** WSI presents 6.79–10.83% more often, but presents 7.97–15.35
  fewer distinct client frames/s in every matched pair. Its ready-to-present p99
  is 9.59–12.42 ms worse. Most of the client-frame tail is already present before
  compositor sampling: ready-to-sample p99 is 33.12–33.84 ms, versus native's
  13.49–20.20 ms. More output refreshes do not establish fresher application images.
- **120 Hz, GPU load:** WSI presents 10.24–13.49% more often and shows 2.42–4.52
  more distinct client frames/s in every pair. Compositor sample-to-present p99
  improves by 8.24–8.27 ms, about one refresh. Ready-to-present p99 remains mixed
  (WSI 49.79–53.55 ms, native 51.09–54.81 ms); do not extend the compositor timing
  win to a universal application-latency claim.
- **Baseline:** rates mostly overlap. WSI shows more distinct client frames at
  120 Hz, but one repeat has a worse sample and client-readiness tail.
- **CPU stalls:** each formal CPU capture observes 11–15 injected pauses and all
  exits are clean. There is no consistent output-rate or client-readiness-tail
  improvement. WSI's 120 Hz presentation rate is 1.08–1.30% lower in each pair.
- Every reported client-frame readiness join has 100% coverage of the analyzed
  presentations. The largest consecutive source reuse observed is twelve WSI
  presentations (one 120 Hz GPU capture); native also has a ten-presentation
  burst in a CPU-load capture. Both must remain visible in the assessment.

### Inspection of the long reuse bursts

For the twelve-presentation WSI burst, compositor rendering spans 91.46 ms using
client frame 386. Frame 387 enters layer delivery 77.89 ms after the first sample
and becomes ready at +92.82 ms, after the last sample. No newer client frame is
observed ready during the span. The native ten-presentation CPU burst spans
75.05 ms, with the next client frame ready at +80.79 ms. A separate nine-frame WSI
baseline burst has the same pattern. Thus these worst bursts do not demonstrate
that either compositor ignored an available newer frame. Reprojection continues
while client delivery/readiness pauses. The traces do not isolate the source of
those pauses to an application, driver, pacing policy or OS scheduling cause.

Service app-pacing logs also show 90 Hz GPU-load client GPU medians around
9.37–9.66 ms native versus 10.30–10.40 ms WSI, near the 11.12 ms refresh period.
That makes GPU sharing and client pacing thresholds useful next hypotheses; it
does not prove either, or establish an isolated compositor GPU-cost difference.

## Recommendation

Keep native as the integration default and keep calibrated WSI opt-in for the
user's ordinary-window visual comparison. At 120 Hz under GPU load it is a useful
candidate; at 90 Hz the client-frame freshness regression needs investigation
before choosing it as a replacement. The next diagnostic should inspect client
pacing, delivery and GPU overlap at 90 Hz, then repeat the matched controls with
one policy change at a time. Do not reintroduce presentation-credit throttling
without separately measuring its known throughput tradeoff.

Shared timing-source extraction remains worthwhile after that behavior is
understood. Hosted handoff, passthrough and lifecycle work remain explicit
integration tasks, not implied by passing ordinary-window timing captures.

## Verification and reproducibility

- No runtime C/C++/Objective-C or CMake edits in this follow-up; all captures use
  the same binary previously covered by 36/36 CTest tests and formatting checks.
- 22 analysis tests pass: fourteen existing strict-analysis tests plus eight
  paired-control/client-readiness tests. Negative cases include workload changes,
  missing/duplicate pairs, wrong ordering/warm-up, failed restoration, readiness
  after presentation, incomplete joins and ambiguous multi-client frame IDs.
- Source/binary hashes and exact tracked runtime diff match both manifests.
  The final analysis-tool bytes are copied and hashed separately because paired
  analysis was developed while the runtime captures were running.
- Original display mode and launchd registration are restored. The installed
  runtime is unchanged. New-file whitespace and `git diff --check` pass.

Evidence in the main checkout's `build/review/macos-wsi-captures/`:
`native-cv-interleaved/` (36 captures, strict `summary.json`, `paired.json`),
`native-cv-cpu-validation/` (4), `native-cv-tooling/`, `native-cv-evidence.json`,
`native-cv-results.md`, and `native-cv-reuse-inspection.json`. The small inspection
script is `build/review/inspect-native-cv-reuse.py`.

To rerun, use a fresh output directory:

```sh
python3 /Users/nickkennedy/.codex/worktrees/macos-wsi-experiment/monado-upstreaming/scripts/macos/wsi-experiment/run.py \
  --build build/review/macos-wsi-build \
  --display-helper build/review/wsi-tools/display-mode-generic \
  --output build/review/macos-wsi-captures/native-cv-new \
  --conditions native wsi-generic-calibrated --generic-fake-pacing \
  --hz 90 120 --loads baseline gpu16 cpu --repeats 3 --seconds 12
python3 /Users/nickkennedy/.codex/worktrees/macos-wsi-experiment/monado-upstreaming/scripts/macos/wsi-experiment/compare.py \
  build/review/macos-wsi-captures/native-cv-new/manifest.json
```

Visible output, pixel equality, lid-closed behavior, hosted Game Mode, passthrough,
Vulkan-client rendering and sleep/resume remain unverified by these captures.

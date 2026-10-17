<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->
# Matched PS VR2 presentation comparison — 2026-10-11

Current native milestone status is recorded in the [M1–M2 completion report](macos-gpu-sync-completion.md). The results below remain evidence for their recorded source versions.

The synchronization fixes show **no clear slowdown in typical presentation** in
this native service workload. Rare presentation gaps were slightly worse in the
modified samples: 39 long intervals versus 33, with a 33.367 ms maximum versus
25.028 ms. The variation between runs and background activity prevent assigning
that difference to the code or claiming strict tail non-regression. This result
supersedes the earlier absence of a matched physical comparison, not the
[remaining synchronization exit criteria](macos-gpu-sync-service-tests.md#current-exit-criteria).

## Scope and reproducibility

Evidence: [macos-gpu-headset-comparison-20261011](results/macos-gpu-headset-comparison-20261011/).
Tested HEAD **`98b4284ad`**, with the recorded integration working tree and small
measurement-only changes enabling a longer, still bounded client runtime.
There are no additional production synchronization or presentation fixes.

A is the synchronization code at `805316bebe976256a16c591ee135a5422ffe73a3`,
retaining the initial checkout's pre-existing multi-compositor edits through the
same source overrides as the earlier offscreen comparison. B is the current
synchronization implementation (`1c7b515d6`, followed by `ba46ccd91`). Seven
synchronization source/header files differ; their complete A/B contents and
SHA-256 hashes are retained. Presentation code, runtime test client, offscreen
plumbing, configuration and build settings are identical in both arms. This
isolates these synchronization changes, rather than comparing two unrelated
integration builds. Other pre-existing integration changes remain present.

Four runs use **A–B–B–A**, each submitting **7,200 frames**, approximately one
minute at 120 Hz. Each run starts a fresh isolated service. The normal client
writes a 64×64 Metal texture and submits a quad into the real Vulkan/MoltenVK
compositor and the physical **4000×2040, 120 Hz PS VR2** display. Cameras are off;
`XRT_MACOS_CLIENT_COMPOSITOR=0` tests service-side compositing. Game Mode is not
requested. This is a low-load native client with full-resolution compositor
output, not a moving-head game, client-hosted compositor or Wine/Chromium test.

**Pixel readbacks and per-image reuse logging are disabled.** Timing traces remain
enabled and fully buffered, with a minimum 16 MiB buffer per stream. This is
matched instrumentation in both arms, not a zero-instrumentation capture. All
individual trace files are below that buffer size. `caffeinate -di` prevents
system/display sleep. All client begin/end thermal states were nominal (0),
and Low Power Mode was false. No continuous GPU power/frequency sampling was
performed. Per-run power, settings and background CPU snapshots are retained.

The test client accepts an optional timeout argument after mode and frame count
(default still 20; allowed 1–300). The runner accepts `--client-timeout` (default
still 30; allowed 1–300). These keep longer manual captures bounded without
changing routine CTests. One arm's measurement command is:

```sh
caffeinate -di python3 scripts/macos/run-gpu-sync-service.py \
  --headset --benchmark --client-timeout 95 --output <new-directory> \
  --client build/arm64/tests/tests_macos_service_gpu_sync normal 7200 90
```

`run-comparison.py` in the evidence directory reproduces the full source/build
substitution, unique service registration and installed-service restoration.
Run it from the repository root with `--output <empty-directory>`; it requires
host launchd/Metal access and exclusive headset USB ownership. It only submits
normal completed frames through the historical baseline, not fault injection.
The installed LaunchAgent is temporarily stopped and restored in a finally
block. The seven current sources are restored exactly and all targets rebuilt;
source hashes and launchctl's restored running state are retained. The original
installed plist and executable selection are unchanged.

Analysis is reproducible with:

```sh
python3 scripts/macos/summarize-gpu-sync-headset.py \
  doc/results/macos-gpu-headset-comparison-20261011 --warmup 120
```

The first 120 client frames are excluded. The analyzer joins compositor frame
IDs from `client_frame_map.csv` to **nonzero actual CAMetalDrawable presentation
timestamps**. It includes both fresh and reused mappings for cadence and records
each distinct client frame's first confirmed presentation for frame latency.
An initial latch may be superseded, so a later reused latch can be that client
frame's first physical appearance. Zero-timestamp callbacks are excluded.
Latency is measured from entry to `xrEndFrame`, not from tracking acquisition
or the start of application rendering; it is not motion-to-photon latency.

## Results

All four clients completed 7,200 frames successfully. Every presentation command
buffer completed with status 4 and retained the shared-event dependency; no
presentation GPU error or no-event fallback was observed.

| Physical presentation metric | A0 | B0 | B1 | A1 |
|---|---:|---:|---:|---:|
| Throughput, Hz | 119.661 | 119.678 | 119.374 | 119.495 |
| Measured presentation callbacks | 7,068 | 7,070 | 7,056 | 7,062 |
| Cadence p50, ms | 8.342 | 8.342 | 8.342 | 8.342 |
| Cadence p95, ms | 8.343 | 8.343 | 8.343 | 8.346 |
| Cadence p99, ms | 8.343 | 8.344 | 8.344 | 8.347 |
| Maximum cadence gap, ms | 16.684 | 33.367 | 33.367 | 25.028 |
| Gaps >12.5 ms | 13 | 11 | 28 | 20 |
| Gaps ≥20 ms | 0 | 1 | 1 | 2 |
| Estimated skipped refreshes | 13 | 13 | 30 | 22 |
| Distinct client frames confirmed presented | 5,349 | 5,290 | 5,238 | 5,289 |
| xrEndFrame entry to presentation p50, ms | 27.391 | 26.791 | 27.172 | 27.308 |
| Entry to presentation p95, ms | 28.587 | 28.553 | 28.530 | 28.591 |
| Entry to presentation p99, ms | 28.757 | 28.761 | 28.991 | 28.816 |
| Entry to presentation maximum, ms | 38.515 | 45.386 | 53.654 | 45.559 |

A long cadence interval is defined as >12.5 ms (1.5 nominal refresh periods).
Skipped-refresh estimates sum `max(round(gap / (1000/120)) - 1, 0)`; they are not
protocol deadline failures. Distinct-frame counts are based on trace ownership
joins, not frame-varying pixel validation. The client supplies 7,080 post-warmup
frames; some are superseded before confirmed presentation while completed
frames are reprojected. Therefore physical presentation throughput is not the
same as the rate of unique application content reaching the display.

Pooling the two runs per arm:

- Baseline: **33 / 14,128** long intervals (**0.234%**), 35 estimated skipped
  refreshes, two gaps ≥20 ms.
- Modified: **39 / 14,124** long intervals (**0.276%**), 43 estimated skipped
  refreshes, two gaps ≥20 ms.
- Mean run throughput: **119.578 → 119.526 Hz**, a decrease of 0.052 Hz (~0.04%).
- Mean per-run median entry-to-display latency: **27.350 → 26.982 ms**,
  about 0.368 ms lower with the fixes.
- Mean per-run p99 entry-to-display latency: **28.786 → 28.876 ms**,
  about 0.090 ms higher with the fixes.

Typical cadence and latency did not worsen materially in these samples. The
modified arm's maximum cadence gap is one refresh longer, and its maximum
frame latency is 8.095 ms higher. Those differences must not be hidden by the
unchanged medians. Only two runs per arm were made; correlated scheduling
stalls are not independent statistical trials. This does not establish a
performance improvement or a strict guarantee against rarer tail regressions.

## Submission cost and long-gap evidence

| CPU timing p50 / p95 / p99 / max, µs | A0 | B0 | B1 | A1 |
|---|---|---|---|---|
| xrEndFrame call | 156 / 279 / 372 / 7653 | 152 / 274 / 366 / 1751 | 150 / 272 / 364 / 8988 | 150 / 276 / 372 / 7091 |
| Readiness worker wait | 375 / 669 / 746 / 1925 | 440 / 691 / 768 / 2121 | 359 / 674 / 761 / 6956 | 377 / 681 / 773 / 2961 |
| Vulkan queue submission | 196 / 250 / 278 / 353 | 186 / 234 / 258 / 374 | 185 / 233 / 255 / 408 | 185 / 234 / 257 / 311 |

Median Vulkan CPU submission cost shows no regression here: mean per-run medians
are 190.5 µs for A and 185.5 µs for B. This differs from the earlier small
positive cost observed offscreen; neither sample isolates the semaphore cost
from CPU scheduling and load. Readiness wait includes producer GPU completion
and CPU observation/wakeup, and is not a pure GPU duration.

B0's 33.367 ms gap is between compositor frames 6441 and 6442; B1's is between
5352 and 5353. Around these gaps, maximum Vulkan queue submission time is
0.166 / 0.188 ms, and previous consumer fence wait is 0.002 / 0.003 ms. The
renderer dispatch CPU times remain below 0.34 ms. B1's presentation worker trace
records a 20.261 ms queue delay and 25.395 ms drawable wait for frame 5353.
B0 does not show similarly long worker/drawable waits around its gap.
Complete associated rows are retained in `long-gap-details.json`.

These rows do not show a long synchronization submission/fence wait causing
either physical gap. The B1 worker/drawable delay is evidence of a separate
presentation delay; it does not identify the operating-system or application
cause. The traces cannot prove that the synchronization changes have no indirect
scheduling effect.

Background load varied. Startup snapshots include substantial Spotlight and
media-analysis CPU usage during later runs, especially B0; A0's snapshot is
lighter. They are snapshots, not continuous attribution of each hitch. No
system processes, scheduling settings or presentation defaults were changed.
The A–B–B–A order reduces simple ordering effects but does not eliminate this
confounding. No headset-worn visual judgement accompanies these captures.

## Validation and conclusion

The restored current build succeeds, its source hashes match the saved modified
arm, and both default headset-free service suites pass **2/2** after extending
the optional test deadline. Existing unrelated working-tree changes remain
preserved. Raw traces, exact source variants, binary hashes, build logs and
service restoration evidence are retained.

The supported conclusion is **no clear regression in ordinary presentation
cadence or median latency for this workload, with a small observed worsening of
rare gaps that remains causally unresolved**. M2.10 remains NOT VERIFIED as a
strict end-to-end/thermal non-regression criterion. Keep the correctness fixes;
this comparison does not justify removing their ownership protections. Residual
presentation stalls, heavier application loads, client-hosted Game Mode,
moving-head visual checks and longer thermal stress remain separate validation.

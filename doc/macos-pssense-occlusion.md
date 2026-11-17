<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Sense tracking loss in native Alyx, 25 October 2026

The user reports intermittent complete right-controller loss and difficulty
putting objects over either shoulder. The implementation below improves diagnosis
and prepares a bounded inertial fallback; hardware improvement is **not yet
validated**. No service was started, installed or switched during this work.

## Recorded evidence

Source: `~/Code/alyx-native/runs/`, specifically the `service.log` and
`identity.json` in these directories. These are native Alyx runs, separate from
the earlier Wine clock-slew investigation. Their identities select the isolated
`alyx-native/build/monado-claude-gaze-fix` build. The 20:27 identity records service
SHA256 `b2ce2204959dcaee0296467c1a33e04cb8ab31ef431d5ab9f239c7b14aed642a`;
it does not supply a source commit, so do not substitute this checkout's HEAD.

| Run (24 October, UTC name) | Device 0 (left) recovered gaps / total / longest | Device 1 (right) recovered gaps / total / longest |
| --- | --- | --- |
| `headset-native-msaa-wsi-direct-20261024T202748Z-monado` | 39 / 46.81 s / 6.14 s | 93 / 197.10 s / 76.61 s |
| `headset-native-msaa-wsi-direct-20261024T213852Z-monado` | 160 / 171.24 s / 59.39 s | 169 / 169.15 s / 68.50 s |

These are `JOINT_LOSS` summaries, not application-pose availability: they exclude
gaps shorter than 250 ms and losses still open at shutdown, include tentative
reacquisition, and cannot distinguish deliberate hiding/put-down from accidental
loss. Hand assignment in these logs is supported by the interleaved per-hand
`CONSTELLATION_FUSED_ACCEPT` records at recovery. New `POSE_STATE` records include
both side and tracker device ID to make this explicit.

The 20:27 run has **51 failed right HID output writes**, no failed left writes,
and two right disconnect/reconnect cycles. Lines 41914–44299 show IOKit error
`0xe00002bc`, 50 consecutive output failures and a disconnect. Lines 46547 and
46550 show reconnect and output recovery. A second failure/disconnect at
62275–62289 recovers at 64808–64811. The right optical filter starts afresh at
80096, immediately before the 76.61 s recovered-loss report at 80098. The
largest observed input-timestamp gap at output writes is 10.51 s right versus
0.32 s left. This is transport/reconnect evidence, not just optical occlusion;
it does not identify whether a disconnect was spontaneous or user initiated.
An IMU fallback cannot bridge periods without IMU input.

In contrast, all 141,553 output writes in the 21:38 run succeed. Its largest
observed input gaps are 111/120 ms (left/right), despite very long optical losses.
That run needs a separate optical/visibility explanation; successful writes do
not establish that the programmed LED pulses intersect camera exposure.

Most long-loss exposures are `dark_unpredicted` (20:27: 1,729 left / 8,536
right; 21:38: 6,342 / 6,347). This means insufficient free LED-shaped blobs above
the learned background and an expired position prediction. It **does not prove
out-of-view, physically dark LEDs, or a clock fault**. The 20:27 right also has
967 `lit_single`, 821 `lit_multi`, and 1,100 `acquiring` exposures: some recovery
opportunities remain with light visible. These categories are evidence for
investigating visibility, correspondence and confirmation separately; they do
not justify loosening optical acceptance gates indiscriminately.

The [earlier host-clock diagnosis](macos-pssense-6dof-integration.md#the-shared-clock-shift-is-the-host-clock-macos-slews-clock_monotonic-2026-10-14)
remains relevant history, not an established cause of these native sessions.

Reproduce the summary without hardware:

```sh
python3 scripts/macos/summarize-sense-losses.py \
  ~/Code/alyx-native/runs/headset-native-msaa-wsi-direct-20261024T202748Z-monado/service.log \
  ~/Code/alyx-native/runs/headset-native-msaa-wsi-direct-20261024T213852Z-monado/service.log
```

Local machine-readable results are saved in
`build/arm64/controller-evidence/20261025-alyx-occlusion/summary.json`.
Use one service lifetime per input log. Active-loss snapshots are cumulative and
are kept separate from recovered totals. Observed HID gaps are sampled at output
writes; pose-state log counts are not time fractions.

## Changes prepared in this checkout

- `JOINT_LOSS_ACTIVE` starts at 250 ms missing and repeats every five seconds,
  even if the controller never returns. It carries an exposure timestamp, loss
  classes, initial camera coverage/edge margin and current prediction visibility.
  The final `JOINT_LOSS` retains its existing meaning and gains a timestamp.
- `PSSENSE_POSE_DIAG=1` logs `POSE_STATE` on application relation-flag changes
  and once per second while queried, with host/query times, side/device ID,
  accepted-optical and IMU age, filter counts, last solved camera count and LED
  model position. This is the relation before grip/aim transforms and any explicit
  synthetic-position options, not a full trace of the final OpenXR space graph.
  Filter rejection samples (first and every 30th) and timestamped filter status
  make visible-optical versus accepted-filter disagreements diagnosable.
- The existing EKF integrates accelerometer/gyro data during optical loss. It
  already allows position for 300 ms, reporting tracked position for 250 ms.
  `PSSENSE_IMU_FALLBACK_MS=600` with `PSSENSE_FILTER=1` extends **application
  position validity only** to 600 ms since the last accepted optical exposure.
  Runtime input is clamped to 0–1000 ms; 0 retains the default. This never extends
  tracked status or the optical solver's prior. The same filter state is used,
  and reacquired optics use the existing filter update/reinitialisation policy.
- Both normal and fallback relations now stop claiming live tracking/velocity
  when IMU age exceeds the filter's 100 ms forward-prediction limit. A held
  orientation can remain valid. Previously stale IMU output retained tracked
  orientation indefinitely. Reconnect resets the filter and diagnostics state.

A 600 ms fallback is an experiment for short shoulder excursions. IMU integration
has no absolute position reference and can drift significantly; it cannot make
seconds of hidden movement accurate. Applications may also choose to hide a
valid but untracked position. Do not claim shoulder interaction is fixed before
a user-operated test. Optical solver thresholds, Linux camera-stream defaults
and normal position horizons remain unchanged.

## Origin jump after loss (25 October 2026)

The mode-16 controller survey shows each optical loss, including 28 gaps of
about 0.1 s, ending position validity 300 ms after the last accepted pose. The
driver then reports orientation only, and OpenXR receives the tracking origin,
about 6 m away, until optics return (up to 10 s behind the shoulder; 19–37 cm
from the last tracked position after long holds). `PSSENSE_HOLD_POSITION=1`
(opt-in) holds the last valid position instead, untracked, while orientation
continues. A headset check with `--generic-controller` is prepared in
`20261025-depth-hold-v4`.

## Offline coast and blob evidence (25 October 2026)

Dataset: `20261025-controller-survey-both-mode16-v3/constellation.ctd` (518 s,
both controllers, detector blobs, 66 Hz IMU). Ground truth is the offline joint
tracker (`constellation_replay --tracker --tracker-csv`, 93% of exposures, RMS
0.3–0.4 px). `--filter-eval` now takes tool-only `FILTER_GAP_MS`,
`FILTER_GAP_PERIOD_MS`, `FILTER_DAMP_MS` and `FILTER_STILL_GATE=1`, and writes gap
age, hold and damped/gated estimates per hidden exposure. Errors by time since
the last delivered optical pose, 3 s gaps every 6 s (`moving`: truth more than
5 cm from the hold position; p50 / p95 mm):

| Since optical | Hold | EKF coast | Still-gated coast |
| --- | --- | --- | --- |
| 0.15–0.3 s, all / moving | 2 / 21, 82 / 146 | 2 / 8, 5 / 15 | 2 / 10, 7 / 12 |
| 0.5–1 s | 5 / 46, 138 / 304 | 13 / 50, 22 / 66 | 6 / 32, 26 / 59 |
| 1–2 s | 9 / 168, 157 / 457 | 51 / 196, 56 / 140 | 9 / 81, 46 / 124 |
| 2–3 s | 13 / 245, 173 / 468 | 169 / 545, 165 / 395 | 14 / 164, 74 / 218 |

The EKF coast is far better than holding while the hand moves, even beyond 1 s,
but drifts while it rests. Integrating the filter velocity only while the IMU
shows motion (|gyro| > 0.3 rad/s or |accel| more than 0.5 m/s² from 1 g) keeps
both benefits. Exponential velocity damping (100–400 ms) was worse than either.
Runs of metres seen at first were real tracker losses coasted unboundedly, not
IMU faults; the live 300 ms validity limit already prevents those.

Blobs during real losses (the offline tracker's gaps over 250 ms, projected
from the nearer of the last/next solved pose; projection checked at 6–7 px on
tracked exposures): behind-shoulder and shoulder losses have essentially no
unowned blobs near the predicted position. In-view losses (low, side, palm-up,
cross-body) often have 4–9 unowned blobs there, frequently in both lower
cameras: the controller is visible but not solved. These could constrain
position with the IMU orientation (partial-blob updates), which needs tracker
changes and is not attempted here. The offline tracker loses some poses the live
run kept (trial 16 low: 92% live), so these counts are indicative.

### Driver coast (opt-in)

`PSSENSE_COAST_POSITION=1` implements the still-gated coast in the driver, per
IMU sample under the controller lock, from the moment the filter's normal
position validity (300 ms) expires; earlier gaps already use the filter's own
coast. It adds limits the offline test did not have: 3 s, 0.8 m from the coast
start and 4 m/s, then holds. Unit tests cover tracking, gating, the limits and
the application relation. It is enabled in `20261025-depth-hold-v4` with the
hold; the headset check is pending.

### Position-only solve from blobs and IMU orientation (offline prototype)

The joint tracker already uses the IMU while tracking: the filter's predicted
position seeds each refine and the IMU orientation, aligned to the optical
world, is a 3° prior. After 250 ms without a solve the device is re-acquired by
bootstrap: stereo (no prior) or, with `CONSTELLATION_TRACKER_ORIENTED_BOOTSTRAP`,
one camera at the IMU orientation (at least 4 free blobs, 5 inliers, coverage
0.5, RMS 0.8 px), then 3 confirming solves. Lost devices get no position prior,
and views with fewer than 5 matches are never used.

The offline tracker with the filter and oriented bootstrap (live settings)
solves 29,498 / 28,815 exposures against the live 29,397 / 29,062, so it stands
in for the live run. The live predicted orientation agrees with optical solves
to 0.12° p50 / 0.65° p95 and stays available during losses. The LED model
export is in OpenCV axes (projection 0.3 px p50 on own blobs).

A Python prototype (session scratchpad, not in the tree) fixes the orientation
from the live prediction and solves position only: each blob-LED pairing whose
LED faces the camera puts the device on that blob's ray, at the point nearest
the prior; the hypothesis explaining most blobs within 6 px is refined (Gauss-
Newton, 10 cm prior) and accepted at 2+ inliers and RMS ≤ 1 px.

- Synthetic partial views (tracked exposures, one camera, k own blobs plus all
  free blobs, prior 3–8 cm off): k = 2 accepted 58% at 5–9 mm p50 but 110–138 mm
  p95; k = 3, 66–70% at 3–5 / 72–120 mm; k = 4, 69–75% at 3–4 / 42–44 mm.
- Real losses of 100 ms or more (12, each exposure seeded from the previous
  estimate): 5% of loss exposures accepted, mostly as the ring returns, often
  with 11–26 inliers before the tracker re-acquires. Distance to the first
  optical pose after the loss: hold 101 mm p50 / 321 mm p90; blob + IMU 4 / 62 mm,
  better in all 12. Requiring 3 inliers was worse (p90 325 mm).

**In the tracker (`CONSTELLATION_TRACKER_PRIOR_REACQUIRE=1`, opt-in).**
`prior_reacquire` in `oriented_bootstrap.cpp` shares the oriented bootstrap's
views and scoring; the joint tracker tries it after stereo and oriented
bootstrap, seeded from the predicted position (or the last solve), and counts a
result within 100 ms and 5 cm of the previous solve as a continuation, so
sparse solves only reach the device once three consecutive exposures agree.
Synthetic unit tests: 3–4 LEDs in one camera are found from a 4 cm prior (where
oriented bootstrap finds none), 90% within 2 cm; a 50 cm prior is accepted at
most rarely. Replay of v3 with live settings, against the same replay without it:

| Minimum matches | Extra poses | Losses ≥ 100 ms | > 5 cm jumps | Recapture jump p50 / p90 |
| --- | --- | --- | --- | --- |
| none (hold) | — | 12 | 0 | 101 / 321 mm |
| 2 | +121 | 9 | 1 (a 2-match single-camera solve 4 cm off) | 13 / 265 mm |
| 3 (default) | +75 | 11 | 0 | 14 / 320 mm |

No existing pose changed. The real tracker gains less than the prototype,
which accepted single sparse solves without confirmation; the p90 losses are
behind-shoulder reaches with no blobs until the ring returns fully.

So the larger gain is earlier re-acquisition when the ring is plainly visible
but bootstrap and confirmation have not accepted it, rather than tracking
through sparse views. A tracker change would need temporal consistency checks
before 2-blob solves reach applications.

## Next user-operated capture

Build the service used by the Alyx launcher from these sources. Its recorded
identity currently points to another build directory; rebuilding `build/arm64`
alone does not update that isolated service. Pass these environment variables
to the **service**, not just Alyx:

```sh
PSSENSE_POSE_DIAG=1
PSSENSE_IMU_FALLBACK_MS=0  # baseline: current 300 ms horizon
# Second capture: change the value to 600.
# The integrated 6DoF setup must retain PSSENSE_FILTER=1.
```

The isolated native-test preparation helper also accepts `--pose-diag` and
`--imu-fallback-ms 600`. Preparing its plist does not start hardware. Existing
`PSSENSE_TIMING_DIAG=1` can add detailed HID/clock evidence but emits much larger
logs. Preserve the launcher identity and service log with each capture.

Start with both rings visible, then test left/right separately: slow shoulder
reach, quick shoulder reach, hold behind shoulder for 0.2/0.5/1 s, return to view.
Record which side and approximate elapsed time. Follow with sustained normal
play to separate a transport disconnect from optical occlusion. Compare time to
position expiry, visible-light failures, filter rejection/reinitialisation,
return-to-view recovery and subjective shoulder interaction. Longer fallback
must not delay optical reacquisition or leave large position jumps.

## Validation

Prepared against `0daaffc4a` plus the working-tree changes. Regression tests cover
66 Hz synthetic occlusion, normal-prior expiry while the application fallback
remains valid, fallback expiry/cap, optical recovery, and stale-IMU flags. The
summary tests cover cumulative active snapshots and independent HID failures.
Full `build/arm64` build passes. All 91 native CTests pass with host IPC/GPU
access, including the seven focused tracking/filter/clock suites; the two new
Python summary tests pass. The first sandboxed full run could not initialise
20 IPC/GPU fixtures (one optional test skipped); rerunning with host access
passes all 91. Logs are saved beside `summary.json` as `build-final.log` and
`ctest-host.log`. `git diff --check` passes. Hardware acceptance and Linux CI
remain pending.

## Headset check with hold, coast and prior re-acquisition (25 October 2026)

User-operated, working tree over `0daaffc4a`, `20261025-depth-hold-v4`:
`PSSENSE_HOLD_POSITION=1`, `PSSENSE_COAST_POSITION=1` and
`CONSTELLATION_TRACKER_PRIOR_REACQUIRE=1` with the 6DoF defaults, mode 16, four
short `--generic-controller --passthrough` sessions (7 service starts). The
user reports controller tracking felt better. `PSSENSE_POSE_DIAG` shows no
orientation-only state after a controller's first lock (0, against every loss in
the v3 survey) and 139 held or coasted (valid, untracked) states. Orientation-only
states occur only before a controller first locks, when there is no position to
hold. `JOINT_STATUS` was not logged at this level, so the prior re-acquisition
counts are not available from this run; `constellation.ctd` is kept for replay.

Following this run, the three settings are 6DoF defaults (`sense_tracking_defaults`),
each still overridable; a unit test checks they are applied and that an explicit
value wins.

## Recent in-view loss log review (17 November 2026)

Read-only review of existing user runs, both reporting service version
`v25.1.0-2500-ga185d42d5` (not this checkout's current HEAD):
`~/Code/alyx-native/runs/play-20261115T194853Z/monado-service.log` and
`~/Code/alyx-native/runs/sourcevr-20261116T232419Z/monado-service.log`.
The latter's `run.json` identifies Half-Life 2 / SourceVR, 23:24–23:45 UTC.
Both controllers attach in left/right order, giving tracker IDs 0/1.

| Run | Left recovered gaps / total | Right recovered gaps / total | Right entirely `acquiring` |
| --- | --- | --- | --- |
| 15 November play | 45 / 59.43 s | 123 / 83.10 s | 84 / 25.81 s |
| 16 November SourceVR | 45 / 31.85 s | 85 / 41.74 s | 62 / 19.07 s |

These are optical confirmation gaps of at least 250 ms, not application tracking
outage durations; short and unfinished losses are excluded. Both right medians
are 317 ms. `acquiring` means an optical candidate was solved but remained
tentative, below the three-solve confirmation gate. Thus most logged right gaps
are not simply a lack of visible blobs. Repeated bootstrap/failed continuation
is a stronger investigation target than indiscriminately relaxing acceptance.
The summaries alone cannot establish why confirmation repeatedly fails.

In SourceVR, 81/85 right gaps start with the last confirmed position at least
50 px inside two or more camera views. Line 35511 records a 1.886 s gap starting
207 px inside three views: 17 `dark_in_view`, then 93 `dark_unpredicted`
exposures and two acquiring exposures. Line 621 has a similar 1.718 s gap,
193 px inside four views. These support investigating in-view LED detection or
illumination as a separate failure mode. Geometry is predicted, not proof of
an unobstructed ring throughout the gap; `dark_unpredicted` means the position
prediction expired, not that the controller moved out of view.

Neither log reports a controller disconnect, HID error or output-write failure
after initial connection. Detailed HID/IMU timing and application `POSE_STATE`
diagnostics are absent, so transport health and application-visible loss duration
cannot be fully verified. Both logs also report right LED timing-lock loss for
dim detections and failed hinted rescans; in SourceVR these occur near the end
(lines 37732/37869), not at every short loss. Successful LED writes would not
by themselves prove pulse/exposure overlap.

Next useful capture: blob/IMU CTD plus per-exposure bootstrap/refine/confirmation
diagnostics and `PSSENSE_POSE_DIAG=1`, with a wearer time marker for an unexpected
in-view right loss. This distinguishes tentative-solve churn, missing LED
detections, and filter/application rejection. No runtime settings were changed
and no hardware was started during this review.

### Existing recordings contain matching candidates (17 November)

The October 25 `20261025-controller-survey-both-mode16-v3` recording has the
needed CTD blobs, camera geometry, IMU, live `CONSTELLATION_FUSED_ACCEPT` and
`POSE_STATE` records, and guided application pose/phase CSVs. Its live log has
17 right-hand recovered gaps starting at least 50 px inside two or more views;
11 are entirely `acquiring`. One is before survey start; the remainder include
ordinary ready/recover phases, not just deliberate shoulder occlusions.

Examples, seconds from the first recorded exposure (`149001109987900` ns):

| Start, s | Right-hand phase | Gap | Acquiring exposures | Start views / margin | Live service.log line |
| --- | --- | --- | --- | --- | --- |
| 303.117 | Low, ready | 317.0 ms | 18/18 | 4 / 179 px | 35645 |
| 312.109 | Low, hold | 650.7 ms | 2/38 | 3 / 124 px | 36766 |
| 315.446 | Low, recover | 317.0 ms | 17/18 | 4 / 241 px | 37121 |
| 351.448 | Side, ready | 317.0 ms | 18/18 | 4 / 178 px | 41119 |
| 368.682 | Cross-body, ready | 400.4 ms | 23/23 | 3 / 226 px | 43189 |

For the first example, the driver's optical age reaches 242.6 ms (`inertial`),
then 297.1 ms (`orientation_only`), with fresh 11.5 ms IMU input. It returns
`tracked` immediately after optical acceptance. Filter rejections remain at
five through the gap, while accepted updates remain at 17,564 until recovery:
this instance loses confirmed frontend output, rather than repeatedly rejecting
delivered optical poses in the filter. The 650.7 ms low/hold gap is a contrasting
case: 17 dark-in-view and 19 dark-unpredicted exposures, with few detections.

The existing measured-model `--tracker-filter` replay accepts 20 poses through
the first 317 ms interval, instead of the live confirmation drought. Repeating
it with prior reacquisition enabled also accepts 20, as well as 19, 20 and 25
poses through the low/recover, side/ready and cross-body/ready intervals.
Thus toggling prior reacquisition alone does not reproduce these failures.
Replay rebuilds its own EKF/prior history and runs deterministically; different
live prediction, delivery timing, settings or source revisions remain possible.
This is a useful recorded live-versus-replay discrepancy, not yet a root cause
or evidence that changing a runtime setting fixes it.

The offline run uses the same executable and measured-model offsets as the
November LED-model comparison, adding only
`CONSTELLATION_TRACKER_PRIOR_REACQUIRE=1`. Commands, replay output and candidate
timestamps are in ignored
`build/arm64/controller-evidence/20261117-existing-loss-review/`. No new wearer
recording is needed to begin investigating these cases.

### Recorded-prior reproduction and confirmation retry (17 November)

**A drifting live position prior can prevent a visible tentative track from
confirming.** The offline tool now accepts `--tracker-recorded-prior`, returning
the exact recorded per-exposure device relation instead of generating a new
prior. It reports missing exact timestamps and fails if any are missing; it
cannot be combined with `--tracker-filter`. This is an open-loop diagnostic:
replayed optical acceptance does not alter the supplied recorded prior.

On the survey, using the measured model and oriented/prior reacquisition enabled,
this reproduces the live loss at `149304544050716` ns exactly: 317.0 ms,
18/18 acquiring, four views, 179 px margin. The recorded position differs from
the independent closed-loop optical replay by about 22 mm on the first missing
exposure, increasing to 83 mm before validity expires. This comparison has no
physical ground truth, but the reproduction establishes that the recorded prior
is sufficient to trigger the live confirmation failure.

Mechanism: a bootstrap resets the tentative track to one confirmation. Its pose
is withheld from the device filter until three solves agree. The next refine
uses the filter's position, which has not received that tentative optical
correction; if that refine fails but bootstrap succeeds again, confirmation
restarts at one. The process can repeat until the filter position expires at
300 ms. Repeated 317 ms gaps are consistent with that expiry plus the remaining
confirmation exposure, rather than a 317 ms transport outage.

Prepared in the working tree over `b6758b7d6`: only after a normal refine fails,
a tentative track retries from its last optical position. It retains the same
IMU orientation prior, optical acceptance limits and three-solve confirmation.
Confirmed-track prediction, Linux stream defaults and the legacy Rift path are
unchanged. An initial always-use-optical seed was inferior in moving-hand
recordings and was replaced by this narrower retry; no experiment toggle remains.

With the exact recorded prior, right survey gaps of at least 250 ms fall from
19 to 7; accepted right poses increase from 28,634 to 28,695, with no baseline
right poses removed. Left gains 76 with none removed. The open-loop input still
forces stale predictions after replay accepts a pose, so this is not a prediction
of live recovery time.

Across the same eight measured-model **closed-loop** recordings as the LED model
comparison (oriented bootstrap on, prior reacquisition off), accepted poses change
from 139,545 to 139,638: 148 added timestamps and 55 removed. Gaps above 100 ms
fall from 124 to 119, and gaps at least 250 ms from 95 to 85. In OpenBrush the
right-hand latter count is 13 → 9; in the survey it is 8 → 7. A hard-case run
(`20261004-000405-hard-cases`) delays initial right acquisition by about 0.7 s
(42 removed right samples), so this is not a blanket non-regression claim.
There is no independent ground truth or new hardware validation.

The synthetic regression starts a tentative ring with a 20 cm erroneous device
position prior. It checks confirmation without premature publication, plus
withholding when the confirming exposure has no blobs. It fails three assertions
against the original joint source, linked separately into the same test binary,
and passes with the retry. All five focused CTest suites pass: joint solver,
filter, pose metrics (including legacy isolation), dataset and Sense clock.
The replay and test targets build, and `git diff --check` passes. Linux CI and
wearer acceptance remain pending. Nothing was installed or started on hardware.

Evidence stays in `build/arm64/controller-evidence/20261117-existing-loss-review/`:
`recorded-prior-receipt.json` gives the reproduction command/environment;
`recorded-prior.log` is the original-source reproduction;
`recorded-prior-fallback.log`/`.csv` are the retry results;
`fallback-scores.csv` compares all eight sessions. For a closed-loop comparison,
use the LED-model comparison's `local-receipt.json` commands with only a new
output path. Prefer investigating the remaining recorded dark/detection losses
before requesting another wearer run solely to diagnose this confirmation loop.

### What remains in the 85 replay gaps (17 November)

These are both hands across eight recordings, not 85 unexpected right-hand
failures: 52 left and 33 right, including intentional occlusion. Classifying each
gap by its largest exposure category gives 43 `dark_unpredicted`, 20
`dark_in_view`, 14 `lit_single`, four `acquiring`, three `dark_out_of_view` and
one `lit_multi`. No remaining gap is entirely acquiring. These are descriptive
categories, not mutually proven physical causes. About 79% of missing exposures
have insufficient excess LED-shaped detections; expired predictions dominate
that figure and do not establish out-of-view motion.

The guided recording associates its 8.425 s left and 6.440 s right gaps with
intentional behind-shoulder movement. It also retains unexpected-looking
in-view cases: right low/hold (650.7 ms) and side/hold (667.3 ms). Predicted
visibility alone cannot exclude ring occlusion or pulse/exposure misalignment.

The best correspondence/reacquisition investigation target is
`20261004-000405-hard-cases`, left, 8.926 s after the first exposure: a 3.387 s
gap contains 89 multi-camera-lit exposures (about 1.5 s) but only two tentative
solves, starting three views in and 189 px from an edge. This is more informative
than treating a dark-unpredicted long gap as proven in-view failure. Four gaps
are still mostly acquiring, supporting further investigation of candidate
consistency/confirmation, without relaxing the mirror-ring safety gates.
Per-gap fields, timestamps and source log lines are retained as
`20261117-existing-loss-review/remaining-losses.json` in the evidence directory.

### Lit-gap diagnosis: stale alignment after a filter reset (17 November)

The multi-camera-lit interval above reveals a second mechanism. Raw saved camera
frames near 9.04, 10.54, 11.95 and 12.45 s show two rings lit, then one ring's
LEDs absent, then both lit again. Thus the 3.387 s interval mixes missing
illumination/detections with failed reacquisition after the LEDs return; it is
not continuous proof of an unobstructed lit ring.

In an isolated diagnostic build, orientation-guided failed refinements reach
roughly 0.95–1.74 px RMS, above the unchanged 0.8 px acquisition limit. They are
not rejected by the rival-hand ambiguity check. Increasing the search budget
tenfold and refining more candidates does not resolve them. As a diagnostic
using future information, seeding from the first recovered optical pose fits
earlier lit exposures with 21–22 matches and roughly 0.55 px RMS. Good fits
therefore exist under the original acceptance gates; these are missed by the
reacquisition path, rather than requiring a higher RMS limit.

The cause is the tracker/source orientation-frame handoff. Before delivering a
confirmed optical pose, the joint tracker computes `align = optical *
inverse(predicted)`. Accepting that pose can reinitialise the source's EKF and
change its orientation at the same exposure. The tracker retains the pre-update
alignment and applies it again on the next exposure. In a controlled trial that
preserves all earlier processing and changes only the target period, the source
orientation shifts **68.45°** at `2757346052299500` ns upon acceptance. The stale
aligned orientation then differs from an independently seeded optical fit by
about 69° during the later lit period. A subsequent stereo bootstrap clears it.

The working-tree fix refreshes the orientation alignment from the source after
an accepted optical delivery. Sources with stable orientation frames retain the
same mapping; fused sources that correct/reset their orientation no longer get
the old correction applied twice. Acquisition thresholds, mirror-hand gates,
confirmation count and defaults are unchanged. This only affects the opt-in
joint tracker; no service was installed or started.

Compared with the position-retry candidate, the target interval now contains
90 accepted left poses instead of one. Its remaining long gap is 1.718 s
(`2757347053331200`–`2757348771730100` ns), consistent with the sparse/dark middle
period, rather than 3.387 s spanning the return of visible LEDs. Across the same
eight closed-loop recordings, total accepted poses increase from 139,638 to
139,926, gaps ≥250 ms change 85 → 83, and gaps >100 ms change 119 → 115. One
distance-run gap splits into two as intervening poses become available, showing
why gap counts alone are not an accuracy or time-with-tracking metric. There is
still no independent pose ground truth or wearer acceptance.

The synthetic test now also changes the source's orientation frame on its first
optical acceptance and verifies uninterrupted confirmation/output on subsequent
exposures, while preserving the missing-confirmation case. It fails four
assertions against the position-retry implementation without alignment refresh;
the final implementation passes all 20 assertions. All five focused CTest suites
pass and both targets build. Linux CI and hardware validation remain pending.

Diagnostic derivative sources/binaries, sampled raw frames, `frames.png`, search
probe logs, before/after poses, `alignment-scores.csv` and the negative-control
test are under `20261117-existing-loss-review/lit-gap/` in the evidence directory.
Search-limit experiments remain confined to those offline derivative builds;
they do not change runtime source or add environment switches.

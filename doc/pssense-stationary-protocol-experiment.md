# Stationary Sense camera/LED protocol experiment (4 October 2026)

This is experimental instrumentation, not a tracker change. Preserve the current
checkout and its unfinished shared-tracker integration. The remote Sense branch
was checked with `git ls-remote`: `macos-pssense-6dof` is at
`c1ca162dec37f1a24d2d846743eaeb08b8cc11ce`. The local checkout is
`claude/pssense-mr2940-evaluation`, HEAD `e54478a16`, 24 commits ahead of that
Sense branch. `build-sense-rel` is RelWithDebInfo and points at this repository;
its CLI predates the unfinished integration. Rebuild only the HID probe for this
experiment; the Python survey bypasses the tracker.

## Prior branch findings that govern this experiment

This run extends the existing LED work. The following are established prior
observations, not discoveries from today's controls:

- [Blink waveform note](pssense-led-blink-waveform.md): all 32 bits were tested
  with a new latch sequence per transition and a constant sequence during
  keepalives. Single held bits produced dark and multi-point lit frames. Do not
  repeat a bitmap hypothesis or a full mask scan just to reconfirm this result.
- [LED poke](../scripts/pssense_led_poke.py): its 25 September notes already
  record unreliable Python forced-on, reliable C-probe illumination, and the
  need to read calibration feature 0x05 after connecting. Keep those reads.
- [Calibration controls](psvr2-camera-calibration.md#static-led-phase-sweep):
  force-IR illumination was intermittent in sampled images even in successful
  captures. Judge frame sequences and ring visibility, not median brightness or
  pose solve counts.
- [Optical tracking history](pssense-optical-tracking.md#the-always-lit-fault-pattern-detection-and-fewer-triggers-25-sep):
  a genuinely stuck ring ignored OFF, INIT, other phases and calibration reads
  while vibration still worked. Only power cycling cleared it. Healthy locked
  sessions tolerated many sequence changes; content changes during scans were
  associated with the fault. Empty waveform bytes are not a reliable OFF.
- [4 October update](macos-pssense-mr2940-frontend-evaluation.md): the fault
  recurred after a period-42 full scan when hinted scans failed out of view.
  Commit `b4c54372b` reduced fallback scans and set session wide pulses to 32.
  Avoid additional exploratory period-42 sweeps. Today's captured BROAD vectors
  do contain period 42; that wire observation qualifies the older toolkit
  calibration's period-32 limit, without establishing period 42 as fault-free.

The existing phase bootstrap, hinted scans, phase sweep, input diagnostics,
stuck-lit survey, mask analyser and CTD replay remain the tools for their
documented questions. The new instrumentation supplies complete wire recording,
raw IF8, independently decoded mode 0x10 and captured-settings translation; it
does not replace those tools or restart LED semantics research.

## Evidence and clock inventory

| Tool | Evidence retained |
| --- | --- |
| Camera survey | Complete selected IF6 VI packets, decoded PGMs, every framed-packet timestamp/sequence/VTS in CSV, complete camera-mode control payloads in `actions.jsonl` |
| Survey `--raw-if8` | IF8/0x89 reads concatenated in `if8.bin`, offsets/lengths and receipt times in `if8.csv`; no private decoder or asserted packet boundaries |
| LED poke | Complete TX/RX HID reports in `reports.jsonl`, feature reads, IOKit results, automatic command/capture markers, and per-step images/counts |
| C probe with `PSSENSE_RAW_REPORTS=1` | Complete callback RX and post-override/CRC TX bytes in stderr, side, device identity, report type/ID, IOKit status and timestamps |
| Existing phase/mask diagnostics | Latched sequence transitions, all 32 temporal bits, schedule CSV, frame-level mask analysis; existing phase sweep/clock logs and CTD replay remain available |

Python `time.monotonic_ns()` and C `os_monotonic_get_ns()` have different epochs
on macOS. Use realtime to associate existing camera/mask recordings; the new
Python HID/probe markers also record `clock_monotonic_ns` (CLOCK_MONOTONIC) for
the C bridge. Receipt times are observations, not physical exposure times.
Camera VTS remains authoritative device evidence. Poke uses continuously received
controller-clock samples, rejects a timed command if the latest sample is over
100 ms old, extrapolates at 3 ticks/us and schedules 50 ms ahead modulo 2^32.
That mapping is a latency-biased receipt estimate, not a calibrated clock fit.
Keepalive reports hold the LED sequence constant; each action changes it once.
The Bluetooth sequence nibble, packet counter and CRC advance per transmitted
report. Cleanup latches OFF for 500 ms.
When changing sender processes, pass `--initial-led-sequence` with the last actual
transmitted LED sequence. The coordinator extracts it from the C backend's raw TX
log. Reusing sequence 1 at that handoff can leave an OFF command unlatched.

IF8's current 36,944-byte read request comes from the existing public-facing USB
driver. The reported candidate `64 + 4*(4 + 256*36)` also equals 36,944; this is
only an observable size hypothesis. Retain all bytes and length distributions.
Background detections and empty/covered records must not be equated to semantic
controller matches. The Monado auxiliary IF8 callback previously only emitted
hex at trace level; it did not save raw binary evidence.

Mode 0x10 no longer falls through to a guessed L8 interpretation. An explicit
`--bc4-layout stacked` decodes the observed mode-0x10 raster as two vertically
stacked 1024x1016 views. The transport raster is 1024x2032 and its payload is
1,040,384 bytes. This packing agrees with the independent decoder supplied in
the handoff and coherent Mac scene features. Endpoint/index bytes are not
camera lanes. The independent decoder follows the public
[Khronos RGTC format](https://registry.khronos.org/DataFormat/specs/1.4/dataformat.1.4.html),
with nearest-integer L8 output. PGMs retain the 0–255 scale: no amplification,
histogram stretch, or difference-image brightness claims. Four mode-4 sources
remain set4/plane0, set4/plane1, set5/plane0, set5/plane1. A matching mode-0x10
source is not assigned a physical identity merely because its index agrees.

## Run sequence and gates

Keep DisplayPort active and run each command under `caffeinate -dims`. Close GAV,
Monado and other headset owners. Use one freshly power-cycled controller, fixed
supports, roughly 40–60 cm separation, a broad ring arc, and no background build.
Use a fresh persistent directory for every run. The tools reject reused output
directories and `/tmp` (including resolved symlinks).

1. **Mode-4 command control.** Run three `off,all_on,off` repetitions with four
   seconds settling and two seconds capture each. Successful writes alone do not
   prove the controller applied the setting. Require compact ring points to
   appear on command and disappear on every OFF; ceiling lights are background.
2. **Known C-probe control if needed.** The existing force-IR recipe sometimes
   succeeds where Python forced-on does not. The coordinator runs that unmodified
   C probe, captures after four seconds, closes it, then uses poke to latch OFF
   and capture after another four seconds. The senders never run concurrently.
   This recipe uses period 42/2 ms cycle and is a historical positive control,
   not a proposed BROAD setting. Run only three trials; if OFF fails, stop and
   power-cycle rather than scanning through the stuck-lit fault.
3. **Repeated mode visits.** Only after the control passes, hold that same
   verified LED setting unchanged while surveying `4,0x10,4` three times. Use
   three-second samples, explicit verified BC4 packing, and raw IF8 recording.
   Return to mode 4 for a same-placement OFF check. Establish view identities
   from scene features and recorded cover/landmark interventions, returning to
   uncovered stationary views between interventions. Do not move either device
   for mode changes. Save and show unamplified representative images.
4. **BROAD preparation.** The supplied handoff ZIP and PCAP are now validated
   locally. `pssense_sony_wire_prepare.py` verifies complete wire CRCs and matches
   all 10,759 selected right-controller CSV rows. Corrected handles are R=20,
   L=21 for this connection. Eight complete captured BROAD settings, captured
   OFF and period-32 PRESCAN are selected; source hashes and the actual seeded
   plan are saved in `20261004-sony-wire-preparation`. Run a three-step smoke
   trial before the full 90-step plan.
5. **Balanced trials.** After report rebasing is verified offline, shuffle complete
   BROAD setting bundles with OFF/PRESCAN controls in repeated balanced blocks,
   save the seed and actual plan, then repeat at a second fixed orientation and
   on the other hand. Repeat the OFF gate between blocks. Orientation/hand changes
   are user-coordinated; command transitions are automatically marked.

Commands (replace ROOT with a fresh directory under `~/Code/psvr2-datasets/experiments`):

```sh
cmake --build build-sense-rel --target pssense_hid_probe -j 4
caffeinate -dims .venv/bin/python scripts/pssense_led_poke.py ROOT/poke-controls \
  --side R --steps off,all_on,off --step-repeats 3 --hold 4 --sample 2
caffeinate -dims .venv/bin/python scripts/pssense_probe_camera_control.py ROOT/probe-controls \
  --hand right --cycles 3
# After the mode-4 control passes AND actual BC4 packing is verified:
caffeinate -dims .venv/bin/python scripts/pssense_probe_camera_control.py ROOT/mode-comparison \
  --hand right --cycles 1 --sequence 4,0x10,4 --repeat 3 --sample 3 \
  --bc4-layout stacked --raw-if8
```

The poke tool also supports `--step-repeats` and seeded `--shuffle-seed` for balanced
existing OFF/PRESCAN controls. Existing mask/phase tools should be run only after
the mode-4 OFF/on gate, if the oracle evidence leaves a specific waveform question.
The phase-sweep diagnostic is about 75 seconds; mask scanning retains the C
probe's 36 segment schedule and automatic manifest. Neither needs manual X marks
or a new CTD replay harness.

## BROAD rebasing contract

Work from complete observable reports and provenance hashes, not isolated `led0`
values. Inventory every changing flag, phase, pulse period, waveform byte,
base/cycle field, host timestamp and counter across adjacent transitions. Preserve
the relative timing and relationships of each captured settings bundle. Classify
absolute controller time versus phase-relative offsets from wire observations
and the existing independently understood protocol before rebasing; do not
treat every phase's cycle position as absolute. Preserve offsets, rebase absolute
schedule anchors to a fresh live clock with adequate lead, and handle wraps in
the proper units. Host timestamps must be fresh and any coupled timestamp fields
must retain their measured relationship. Transport counters and latching
sequence are freshly generated, and Bluetooth CRC is recomputed after all changes.

Before hardware: round-trip the captured packets, validate CRC, show a byte-level
diff of each rebased vector, test controller-tick wrap and clock staleness, and
verify that no captured absolute timestamp survives unchanged. Keep test vectors
and their wire-only explanation in the experiment dataset. Do not copy Sony
private structures, offsets, algorithms or internal LED geometry into Monado.
Sony semantic 17-LED labels are reference ground truth only; independent Mac
replay has no live Sony labels. BROAD's reported ~10 s and ordinary PRESCAN's
~3 s scheduling are context for trial duration, not an assumption that loss of
optical matches immediately invalidates pose or forces PRESCAN. The prepared plan uses a one-second settling interval and ten-second observation
window per tested bundle. Each trial is preceded by four seconds OFF settling
and two seconds recording, then one second PRESCAN settling and two seconds
recording. The three balanced shuffled blocks contain all eight BROAD bundles
and both controls, with seed 20261004. The experiment holds commanded settings;
it does not reproduce Sony's automatic optical-loss or BROAD/PRESCAN state machine.

## First hardware observations

`20261004-stationary-right-controls`: all nine mode-4 samples recorded 236–241
packets (approximately 120 per camera set), with successful HID writes in every step. None of
the three LED_ALL_ON stages showed the ring. Compact counts were approximately
`[0,0,0–1,1]` in both conditions; the upper views show ceiling lights. This is a
failed positive control, not evidence for or against the waveform hypothesis or
a stuck-lit fault. The next experiment uses the C-probe positive control at the
same stationary placement.

`20261004-stationary-right-probe-controls`: three C-probe controls did show the
ring, but their following Python OFF captures reused the C probe's LED sequence
1. The ring remained visible in those captures. This is **not** a valid stuck-lit
test: the handoff failed to guarantee a new latch sequence. Cleanup subsequently
used sequence 2. The corrected rerun (`...-latched`) was an empty failed start:
the controller had powered down. Wake it without changing the placement, then
use a new output directory and the corrected sequence handoff.


`20261004-stationary-right-probe-controls-latched-2`: the corrected C-to-Python
handoff used LED sequence 1 → 2, with cleanup at 3. All three ON stages contained
26/60 sampled camera-0 frames with ring pixels above 100 DN; every following OFF
stage contained 0/60. The positive control is intermittent, so median images
alone would conceal it. This passes the repeated command/latch control and does
not exhibit the stuck-lit fault at this placement.

`20261004-stationary-right-mode-comparison`: three repeated `4,0x10,4` visits
used the same C-probe LED setting. The mode-0x10 visits recorded 179, 179 and 173
set-11 packets. Most mode-4 visits recorded 360–361 packets; the last recorded
256, with unequal set counts (148/108). Keep that throughput reduction visible
in any comparison. Raw IF8 reads were 1,885 × 36,944 bytes plus six zero-length
reads. Both BC4 views decode coherently without intensity amplification.

`20261004-lower-wearer-left-covered`: two `4,0x10,4` repeats with only the lower
wearer-left camera covered identify set4/plane0 and BC4 view0 as that physical
camera. Set4/plane1 and BC4 view1 retain matching scene features, consistent with
the complementary lower wearer-right camera. This intervention does not
establish upper-camera identities. Original PGMs remain unamplified; contact
sheets resize spatially only.

The supplied native reports use a different transport wrapper from Monado's
existing PS5-style framing. The experiment preserves the complete independently
validated 38-byte settings block, refreshes host time and the LED latch sequence,
and generates the existing transport sequence/counter/CRC. Native opaque tails
are retained as wire evidence and are not interpreted or replayed. This is a
settings translation experiment, not byte-identical native-report replay.
PRESCAN's absolute controller-clock anchor is rebased with at least 50 ms lead,
preserving its captured modulo-cycle phase relative to an RX receipt estimate.
BROAD's observed zero relative offset is preserved. Exposure phase remains
uncalibrated; no live Sony semantic labels are available on the Mac.

Smoke command (fresh output directory; right awake and card removed):

```sh
caffeinate -dims .venv/bin/python scripts/pssense_led_poke.py ROOT/sony-smoke \
  --side R --settings-plan ~/Code/psvr2-datasets/experiments/20261004-sony-wire-preparation/settings-plan.json \
  --limit-steps 3 --initial-led-sequence 3 --raw-if8 --off-roi 260 120 420 280
```

Review all three stage image sequences, successful writes, fresh RX clocks and
OFF gate before removing `--limit-steps`. Reconfirm the ring ROI after changing
orientation or controller; reuse neither its location nor the right-side plan
blindly. Prepare the left-side plan from the corrected left capture.


## Captured-setting smoke and alignment control

`20261004-stationary-right-sony-smoke` recorded 1,566 TX and 1,544 RX
reports. All TX CRCs and preserved settings bytes checked correctly; all writes
succeeded. OFF had zero ring pixels above 100 DN in all 60 saved camera-0 ROI
frames. Captured PRESCAN likewise had zero in 60, and BROAD-0effffff zero in
300. Raw IF8 reads were all 36,944 bytes (154 / 153 / 633 by stage). This is
a dark trial, not a successful BROAD positive control. Receipt-relative source
phase alone does not establish the light's phase against Mac camera exposures.

The existing session phase diagnostic was then run at the unchanged placement:
`sessions/20261004-221158-stationary-right-phase-check`. Its hinted narrow scan
locked at 5.1 s, with a 1,450 us window and 1,000 us locked pulse. Each camera
showed the ring in 247/248 saved locked frames; no stuck-lit event occurred.
The CLI exited 2 because no controller pose was solved. That does not invalidate
the LED phase measurement; pose solving is outside this experiment's gate.
The built CLI was reused without rebuilding the unfinished integration.

`--phase-reference-log` is a separate, explicitly recorded alignment treatment.
It reads the last healthy locked `LED_SCHEDULE` from that existing diagnostic,
checks the live controller clock against it (rejects >50 ms discontinuity or
reference age >90 seconds), and projects the pulse centre by whole captured
cycles to a fresh anchor at least 100 ms ahead in the current recipe (50 ms in
the initial runs). It retains all captured flags,
waveform bytes and cycle length, and does not change BROAD's zero relative
offset. It changes the common PRESCAN anchor's phase relative to the Mac
exposures instead of asserting that a Windows receipt phase is a Mac exposure
phase. Original and applied phase, the reference and every actual report are
logged. The original source-preserving dark trial stays separate. This uses
observable Mac timing logs; no Sony private timing implementation is imported.


The first long aligned attempt, `20261004-stationary-right-sony-shuffled-orientation1`,
was interrupted after its positive PRESCAN controls progressively faded. OFF
remained dark. The interrupt executed the new-sequence OFF cleanup (sequence 59).
Do not treat those trial differences as isolated waveform effects. A captured
cycle can be appropriate within a short trial while being unsuitable for
indefinitely projecting a Mac phase reference between trials.

The supplied `~/Downloads/timing-handoff.md` reinforces this separation: clock
fits, receipt timestamps and emission/exposure phase are distinct. The first
30-second Mac phase check had 66.8 us reported clock creep and exposure residuals
p5/median/p95 -28/0/29 us. The first refreshed 10-second check settled its clock
within 100 us at 2.8 s and recorded 47/48 lit locked frames per camera. These are
session measurements, not new universal correction constants. Ordinary captured
Sony PRESCAN is usually period 40; the selected common anchor here is the
complete period-32 startup setting, and BROAD representatives retain period 42.

The coordinator now supports `--settings-plan --calibration --off-roi --raw-if8`.
It refreshes phase using the existing session diagnostic before every two trials,
closes that HID owner, then runs six captured-setting stages. No senders overlap.
The current recipe projects a recent healthy anchor using the captured cycle
length, without fitting oscillator rate from the short bootstrap log. Captured
cycle lengths within trials remain unchanged. References older than 90 seconds are rejected.
PRESCAN controls require ring pixels in at least half the saved ROI frames;
OFF controls require no more than a quarter. Either failed control stops the
batch through OFF cleanup. Explicit phase-refresh and batch markers preserve the
actual plan indices and commands. The older CLI's final timing sequence is used
with a +16 latch margin, since that binary lacks the new full-TX logger; the
following optical OFF gate verifies the handoff. The camera and poke stages do
retain complete HID TX/RX and raw IF8.

`--compact-examples` preserves the same raw camera bytes in `.bin.gz` and the
same unamplified 8-bit pixels in PNG instead of PGM. Selection remains every
second packet of each set. The manifest names the actual files, and lossless
pixel identity is tested. This reduces persistent disk use for repeated runs;
IF8 and HID recording are unchanged. Earlier examples remain in their original
formats.


The short-log Mac cycle-rate fit was removed after it included startup offset
corrections and caused a failed PRESCAN gate before BROAD began. The passing
early batches retain that fit in their logs; later batches use only a fresh
healthy anchor and the captured cycle length, bounded to 90 seconds. This is
explicit treatment history, not a claim that a short bootstrap log measures a
physical oscillator rate. Phase refreshes now run for 30 seconds so hinted
retries and startup clock settling have time to finish. Resumption is at a
six-stage boundary with the original seeded plan indices retained.

The initial readout validated 8,287 TX and 8,208 RX reports across the aligned
smoke and first two completed batches: no CRC, I/O, transport-sequence/counter
or captured-settings preservation errors. Receipt-biased first-send PRESCAN
leads were 38.8–60.2 ms. They were positive, but the new prepared-run default
requests 100 ms (`--minimum-lead-ms`) to leave margin for new input-clock
observations catching up between preparation and first write. This advances
whole cycles and preserves modulo phase; it is a scheduling margin, not a
measured physical latency. Both requested and observed lead remain recorded.

A headset stream interruption in the first refreshed attempt produced zero
IF6 packets in mode 4 and mode 0x10 despite the DP display being online at
4000x2040/120 Hz. The user restarted the headset without changing placement.
The resumed camera stream recovered. Preserve those empty stream captures
separately; they are not evidence of LED OFF.

## Final first-orientation acquisition and camera-mode geometry check

The resumed right-controller orientation-1 plan finished all 90 unique stages
(30 shuffled trials) across roots ending `orientation1-2` through `-5`. Failed
PRESCAN stages remain recorded alongside their successful retries. The summary
distinguishes acquisition completion from acceptance of the optical gate.
Earlier batches used the transient short-log rate fit described above; preserve
that treatment history when interpreting differences.

The final coordinator recipe uses 30-second phase refreshes with the existing
closed-loop bootstrap tracking enabled (`TRACK=1`, `TRACK_COVERAGE=0`). It
requires ring illumination in at least 80% of the last 20 calibration camera-0
frames before handing off to the settings sender. Actual captured-setting trials
retain fixed settings and the captured cycle. Their OFF and PRESCAN gates remain
25% maximum and 50% minimum populated frames, respectively.

The Charuco target is also used to compare mode 4 and mode 0x10. Reuse the camera
survey with `--sequence 4,0x10,4 --repeat 2 --settle 1 --sample 2 --examples 8
--save-every 8 --bc4-layout stacked --compact-examples --raw-if8`, saving each
fixed board position as a separate persistent root. Move only the board between
roots; vary position, depth and tilt. The offline
`scripts/psvr2_camera_mode_charuco_compare.py` reuses the existing public board
detector, matches corner IDs in native pixels and reports candidate affine
mappings, exact doubling and half-pixel-centre hypotheses, corner repeatability,
and held-position errors once three positions exist. It excludes documented
mode-4 padding columns 508–511. Detection-only contrast variants are labelled;
saved images retain their original decoded DN values. One planar position is
insufficient to validate camera identity or transfer a complete calibration.

The first position gave exact-coordinate-doubling RMS errors of 1.12 pixels for
mode-4 camera 0 to BC4 view 0 and 1.65 pixels for camera 1 to view 1, measured in
mode-0x10 pixels. This supports the lower-camera 2× mapping and complements the
physical cover test; it is an initial observation pending additional positions.

The first-orientation report summary contains 55,197 TX and 54,991 RX reports
with zero CRC, I/O, transport-nibble or transport-counter errors. All 90 unique
accepted stages preserve captured settings apart from the explicitly rebased
timestamp and latch fields. The 24 BROAD trial stages have ring pixels in every
saved camera-0 ROI frame; the three actual OFF trials have none, and the three
actual PRESCAN trials have ring pixels in every saved frame. Compact component
counts vary between roughly five and six in the BROAD trials. These are image
components, not physical LED identities or a decoded BROAD codebook.

Charuco position 2 failed lower-camera detection/coverage and must not count as
a successful mapping check. Position 3 did provide lower-camera matches. Across
positions 1 and 3, exact doubling gives RMS errors 1.44 and 1.59 mode-0x10 pixels;
the half-pixel-centre hypothesis gives 1.02 and 1.32 pixels. Different-camera
affine fits worsen to roughly 6–10 pixels as board position changes. Additional
usable positions are required for held-position validation. Both modes' observed
consecutive VI VTS intervals have median 16,683 us in position 1. The recording
has sequence gaps, especially during decoded example saving: received packet
counts alone must not be used to infer a different nominal camera frame rate.

Position 4 also failed lower-camera detection in individual frames. A separately
labelled `--mean-frames` treatment, consistent with the prior static native
calibration, averages all stationary saved frames per camera before detection.
It requires at least eight corners in a view. This recovers 10/8 lower-camera
corners at position 4; position 2 remains excluded from lower-camera fits. The
result is saved as `20261004-charuco-mode4-mode16-analysis4-mean/comparison.json`.
Across usable positions 1/3/4, lower-pair affine fit RMS errors are 0.77/0.82
mode-0x10 pixels. Held-position-out RMS ranges are 0.61–1.09 and 0.70–1.01 pixels.
Wrong-camera mappings have held-position errors in the tens of pixels. Exact
doubling RMS is 1.11/1.24 pixels. This supports a same-projection, approximately
2× lower-camera mapping without reflection over the sampled board coverage.
It does not validate full-field intrinsics, exact pixel-centre convention,
photometric equivalence or absolute rig-to-head alignment. No runtime calibration
has been changed. Raw images and the individual-frame failures remain preserved.

The second right-controller orientation was then confirmed by the user (roughly
45° rotation with headset fixed) and started with the current final coordinator
recipe in `20261004-stationary-right-sony-refreshed-orientation2`. The Charuco
board was removed before LED trials. Its first batch passed optical controls.

`scripts/pssense_stationary_spots.py` is offline image instrumentation. It reuses
the mask diagnostic's compact-component detector on native camera-0 ROI pixels,
chooses fixed image locations from the first accepted PRESCAN, matches components
one-to-one within four pixels, and retains VI sequence/VTS plus host receipt
timestamps. It records component presence, peak DN and a 7×7 patch's summed DN,
comparing each actual trial to its preceding PRESCAN control. Spot indices are
image locations only; they do not identify physical LEDs or Sony semantic IDs.
Its template does not cover additional spots absent from that first control.

The first-orientation patch readout has five reference spots and 7,303 saved ROI
frames across accepted stages. Every reference spot is present in every actual
BROAD/PRESCAN trial frame, and absent in the OFF trial frames. Most BROAD patch
sum ratios to the preceding PRESCAN are approximately 0.97–1.08. One early
`08ffffff` trial shows a common brightness increase of 1.8–2.5× across all five
spots; its subsequent repeats do not reproduce it. Peaks are usually saturated.
No reproducible spatial selection is established by these vectors. This is not
proof that waveform bytes lack temporal effects: exposure/selection aliasing,
phase treatment history, saturation and non-byte-identical HID translation limit
the inference. The timing and image measurements are preserved for reanalysis.

For the earlier constant-setting mode-comparison recording's `00-on/if8.bin`,
an independent wire-only check sampled every tenth 36,944-byte read within each
of its nine VI visit receipt windows. It tested the user-supplied candidate
partition `64 + 4*(4 + 256*36)`: the leading little-endian count in each group
matched the number of nonzero 36-byte records in every sampled read (161 reads).
All four groups can be populated during all three mode-0x10 visits and during
mode-4 visits. Mode-0x10 per-group maxima were [6,5,8,9], [6,5,8,10] and
[6,5,8,11]. No group-to-physical-camera assignment follows from this check.
This is observable packet structure, not an imported Sony private layout or
algorithm. It supports distinguishing IF6's two-view image output from the
internal detector's observable IF8 output; it does not establish which sensors
are active or the semantic meaning of the record bodies.

The second-orientation run stopped before steps 48–53 because the preceding
30-second calibration's final 20 camera-0 frames were only 70% lit. Its clock
settled late (26.6 seconds) and reported 751.5 us creep. The gate stayed at 80%.
Resumption in `orientation2-2` starts at step 48 with `--phase-seconds 45`;
that refresh reported clock settling at 34.9 seconds and 397/397 lit saved
locked frames in each camera, followed by a passing captured-settings batch.
No rejected calibration starts a BROAD trial.

Receipt manifests can have substantial camera sequence gaps. For example, the
first second-orientation BROAD capture received 338 set-4 and 134 set-5 packets
over the selected interval; relative to their respective first-to-last sequence
spans, 42.7% and 77.1% were absent. Median received VI VTS intervals were 33,365
and 50,050 us. Those receipt intervals must not be treated as nominal camera
periods. The summary now records per-set sequence increments, missing fractions
and received VTS intervals. The runs retain sufficient positive/negative image
controls for the stationary lighting test, but are not lossless temporal-waveform
recordings. No missing frame is imputed as a dark observation.

After 20 completed second-orientation trials, the next batch failed its baseline
OFF gate with ring light in 26/26 saved camera-0 ROI frames. The user observed
the status LED off and identified the documented stuck state. The sender logged
448/448 successful OFF writes at latch sequence 203, followed by cleanup OFF at
204 (548 total successful OFF writes including cleanup). No BROAD vector from
that batch was sent. Its preceding calibration began with a dark baseline, then
ran one narrow scan and closed-loop phase tracking (11 probes, two moves); it
reported six clock snaps and late settling at 38.5 seconds. This locates the
fault across the calibration/handoff interval, not to a demonstrated causal
BROAD vector. The older CLI's lack of full TX logging limits exact trigger
reconstruction. The user power-cycled the right controller without moving it;
a known probe ON/new-sequence OFF recovery control follows before resumption.

Recovery control `20261004-stationary-right-orientation2-fault-recovery` found
ring light in 23/57 probe ON frames (the known intermittent force-IR sampling)
and 0/56 newly latched OFF frames. Resumption at step 60 is in `orientation2-3`.
To close the fault-trigger logging gap without rebuilding unfinished integration
sources, the original Ninja CLI link command was applied to its cached object
and archive inputs, with only its output path changed to
`build-sense-rel/src/xrt/targets/cli/monado-cli-raw-reports`. The already rebuilt
`aux_os` archive contains the opt-in raw-report logger. The original executable's
SHA-256 was verified unchanged. Cached input hashes, linker arguments and both
binary hashes are recorded beside the new binary and copied to
`orientation2-3/phase-cli-provenance.json`. No source compilation or production
tracker change was needed for this separate diagnostic binary.

The session script's existing `MONADO_CLI` override selects that diagnostic.
The coordinator enables `PSSENSE_RAW_REPORTS=1` and now prefers the selected
controller side's actual final TX LED sequence, including exit OFF, as its seed;
the poke increments it for the first setting. The original binary retains its
documented timing-field +16 fallback. Complete calibration TX/RX/feature bytes
are retained in each subsequent session `run.log`, closing the earlier gap for
future events. This is an explicitly recorded instrumentation treatment change.

`scripts/psvr2_if8_record_counts.py` makes the candidate count check reproducible
without decoding any record bodies. Applied with stride 10 to the entire earlier
mode-comparison IF8 capture, it sampled 189 of 1,885 full-size reads and found
zero leading-count/nonzero-record disagreements. Results are in
`20261004-mode-comparison-if8-candidate-counts`. Six empty reads are retained in
the source manifest and excluded from candidate parsing. Camera-mode visit
association is explicitly receipt-window based, not a clock/exposure alignment.

After 28 second-orientation trials, a 45-second refresh's final ROI frames were
only 15% lit and the next batch did not start. Its logged clock creep was
-2,154.8 us, with late settling at 41.9 seconds. This is a dark phase-control
failure, distinct from the stuck-lit OFF failure. The final two trials are
retried from step 84 in `orientation2-4` with 75 seconds for the existing phase
diagnostic to settle and track; the illumination thresholds are unchanged.
These refresh-duration changes are acquisition treatments, not measured physical
latencies or new firmware timing constants.

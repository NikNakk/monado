# PS VR2 Sense native optical LED synchronization

Status: experimental reverse-engineering note, 2026-10-04.

## Provenance

Feature: Sense controller LED synchronisation
Source of knowledge: Sony Windows driver used as a behavioural oracle
Method: Bluetooth HCI/ETW capture of native A2/31 output, USB camera/timing observation, and controlled state-change
experiments; narrow in-process hooks were used only to timestamp behavioural events and were cross-checked against the
wire capture where applicable
Observed protocol/behaviour: A2/31 LED field layout, phases/periods, timing units, schedule-generation semantics,
phase/mask/base-time transitions, and controller tracking-state correlations
Implementation provenance: independently implemented in Monado using Monado's existing pssense architecture and pose
solver; no Sony source, decompiled implementation, proprietary payload, SDK material or internal Sony data structure is
copied into Monado

This follows the PSVR2Toolkit `AGENTS.md` clean-room boundary:
`observation -> protocol description/test vector -> independent implementation`.

This document records the native Sony Sense-controller optical LED schedule observed from the Windows
PlayStation VR2 SteamVR driver and explains how it maps onto Monado's existing `pssense` timing/bootstrap and
6DoF pose solver.

The immediate engineering goal is **not** to replace Monado's pose solver. The current macOS branch already
has a usable runtime-tested optical pose solver. The goal is to stop driving the controller as permanent
PRESCAN, reduce the controller LED lockout/stuck-on failure rate, and make loss/reacquisition use phase
semantics closer to Sony's driver.

## Evidence

Successful Windows oracle run:

- `vrserver(20261004-101906).txt`
  - SHA-256: `7880e0e4680c0887bb77511db0c9e4d638af914b11e7446e259aa2ef81b7b109`
- `steamvr-success.pcapng.gz`
  - SHA-256: `2d2ed18dc26006f1d6c6a588d1086a7917cf515bb385cc57ccdda86e3c783a22`

The run used the stock Sony LED state machine with passive Toolkit tracing, plus the experimental HMD/camera
workarounds needed to keep the Sony tracker alive on the test PC. Toolkit's custom LED-sync path was disabled.

The decisive camera fix was to send the Sony camera report `0x0b` with `wValue=0x0b`, subcommand 1 and
payload `{1, 0x10}`. After that, USB interface 6 / endpoint `0x87` delivered 1,040,640-byte tracking frames
at 60 Hz and Sony reported one VI / tracking11 packet for each frame.

The right Sense then achieved Sony tracking flag 9 (full optical tracking / 6DoF) repeatedly.

## Reproducible oracle test vector

A compact protocol-fact fixture derived directly from the successful PCAP is checked in as:

```
tests/data/pssense_native_led_oracle.csv
tests/data/pssense_native_led_oracle.md
```

It contains the 40 consecutive runs of identical LED schedule fields from all 5,949 CRC-valid A2/31 reports. The CSV
was regenerated and checked against the source PCAP; report counts sum to 5,949. It intentionally contains protocol
fields and timing summaries only, not proprietary Sony payload/code.

## Bluetooth A2/31 report

The native controller output is HIDP output `0xA2` followed by report ID `0x31`.

- HIDP + report: 79 bytes.
- Report: 78 bytes.
- Native Sony report byte 1 ("mode" in Toolkit terminology) was `0xA2` in this capture.
- CRC32 is valid when seeded/prefixed with `0xA2` and calculated over report bytes 0..73; report bytes
  74..77 contain the little-endian CRC.
- 5,949 valid A2/31 reports were recovered from the successful capture.
- No invalid CRC was found among those reports.

The LED schedule starts at report offset 21:

| Offset | Size | Meaning |
|---:|---:|---|
| 21 | 1 | phase |
| 22 | 1 | LED schedule sequence/generation |
| 23 | 1 | period ID |
| 24 | 4 | `cycle_position`, little-endian |
| 28 | 4 | `cycle_length`, little-endian |
| 32 | 4 | LED blink mask |

The exact preceding output-report fields are already represented by `pssense_output_settings`; this section
only fixes the LED-field wire semantics.

## Units: do not conflate these two fields

The LED structure deliberately uses two different scales.

### cycle_position

`cycle_position` is in the controller/device timestamp domain: **3 MHz ticks**, one tick = 1/3 microsecond.

Examples from the Sony log and Bluetooth capture:

- PRESCAN `baseTime=20,862,736` -> wire `cycle_position=62,588,208`.
- BROAD `ADJUST_BASE_TIME offset=-250` -> wire `cycle_position=-750`.
- BG entry `offset=+300` -> wire `cycle_position=+900`.

Thus Sony's libpad base-time/offset values are in microseconds and the wire value is x3.

### cycle_length

`cycle_length` is in **thirds of a nanosecond**, not 3 MHz ticks.

The successful run used a 59.94 Hz camera cycle:

- Sony libpad `frameCycle=16,683,350 ns`
- wire `cycle_length=50,050,050`
- exactly `frameCycle * 3`

This matches Monado's existing `average_exposure_interval_ns * 3` encoding.

### period ID

The pulse-width period ID uses 50 microseconds per unit:

- 40 -> 2.00 ms
- 42 -> 2.10 ms
- 30 -> 1.50 ms
- 20 -> 1.00 ms
- 9 -> 0.45 ms

## Observable HMD USB streams in the successful run

The successful Windows session also provides two useful external data-plane cross-checks:

- camera interface 6 / endpoint `0x87`: 60 successful reads/s, each 1,040,640 bytes;
- interface 8 / endpoint `0x89`: 60 successful reads/s, each 36,944 bytes.

The independently written Monado PSVR2 protocol definitions identify interface 8 / endpoint 9 as the LED-detector
(`PSVR2_LD`) stream and define `USB_LD_XFER_SIZE=36944`. Thus the successful Windows log's
`2,216,640 bytes/s` on IF8 is exactly `36944 * 60`.

This is externally observable USB traffic and is a better clean-room target for future analysis than Sony internal
optical-processing state. The prepared Toolkit oracle branch records the raw IF8 packets without assigning internal
structure to them.

The 1,040,640-byte camera packet should likewise be treated as a raw VI packet until its observable lane packing is
validated. Toolkit's older BC4 interpretation and Monado's current diagnostic 8-byte-lane interpretation disagree on
semantics despite the same packet size, so the capture records raw header metadata and preserves the bytes.

## Output cadence and report timestamp

The successful Bluetooth capture contains 5,949 CRC-valid native A2/31 reports over 78.46 seconds. Delivery is
asynchronous to the 59.94/60 Hz camera stream: the overall output rate is about 76 reports/s with substantial Windows/
Bluetooth scheduling jitter (median inter-report spacing about 14.94 ms).

Report bytes 17..20 form a little-endian microsecond-scale timestamp. Across the capture its delta tracks the packet
capture time essentially 1:1 (median device/host interval ratio ~1.0000006). This is a separate time domain from the LED
`cycle_position`, which uses 3 MHz ticks.

Monado currently writes Bluetooth output at the 32/3000 s PCM-haptics cadence (~93.75 Hz). That difference is worth
keeping visible, but it is not by itself evidence for the lockout: the controller has already tolerated ~93.75 Hz in
standalone LED tests. The more important native distinction is that the LED schedule generation/sequence is held across
many output packets.

## Native timing refinements

Two native operations can now be characterized from the successful log.

### BROAD -> BG centre preservation

Sony enters BG/30 from BROAD/42 with `offset=+300 us`. This exactly preserves the pulse centre:

```
(42 - 30) * 50 us / 2 = 300 us
```

This independently confirms that phase-width changes must compensate the pulse **start** by half the width difference.
It supports Monado's centre-preservation rule when changing output period.

### Combined frame-cycle/base-time correction

Sony also emits `ADJUST_TIME_AND_CYCLE(adjustmentFactor, offset)`. The next `frameCycle` shows that the factor is a
fractional multiplicative correction:

```
new_frame_cycle = trunc(old_frame_cycle * (1 + adjustmentFactor))
```

Observed examples:

- `16,683,350 * (1 - 4.5034726e-6) = 16,683,274.87` -> Sony stores `16,683,274`, with base offset `-18 us`.
- `16,683,274 * (1 - 6.465052e-7) = 16,683,263.21` -> Sony stores `16,683,263`, with base offset `-9 us`.

No standalone `ADJUST_FRAME_CYCLE` command was seen in this successful log; the combined command was used for the
fine long-term correction once BG was active.

## Native fine timing commands

The first successful Windows session also exposes the semantics of Sony's combined time/cycle adjustment:

- `ADJUST_TIME_AND_CYCLE factor=-4.5034726e-06 offset=-18` with current `frameCycle=16683350` is followed by
  `frameCycle=16683274`, matching `floor(16683350 * (1 - 4.5034726e-06))`;
- a later `factor=-6.465052e-07 offset=-9` changes `16683274 -> 16683263`, again matching a multiplicative
  fractional correction.

The offset component becomes the new relative `baseTime`. This strongly suggests the factor is a fractional
frame-period/frequency correction rather than an arbitrary tuning value. The successful Bluetooth PCAP covers the
second session, which did not contain these two combined adjustments, so the corresponding on-wire cycle-length
update has not yet been independently verified there.

## Native phases observed

The successful capture contained:

| Phase | Period | Reports | Meaning inferred from successful run |
|---|---:|---:|---|
| INIT (0) | 0 / 40 | 35 | startup / transition lead-in |
| LED_ALL_OFF (5) | 0 | 3 | controller parked before optical acquisition |
| PRESCAN (1) | 40 | 825 | absolute-time acquisition schedule |
| BROAD (2) | 42 | 4,085 | broad search / reacquisition |
| BG (3) | 30 | 1,001 | background/tracked operating phase |
| STABLE (4) | — | 0 | not required for 6DoF in this run |

The absence of STABLE is important: **do not gate Monado's valid 6DoF pose on reaching phase 4**. Sony acquired
and reacquired full 6DoF in PRESCAN, BROAD and BG.

## Native cycle_position semantics by phase

### PRESCAN

PRESCAN uses an absolute device-time anchor. The wire `cycle_position` advances with the controller clock and
is periodically re-anchored.

Sony did not relatch this schedule on every output report. The A2/31 stream continued at high rate while the LED
schedule sequence and base time remained unchanged for many reports.

### BROAD / BG

BROAD and BG use a signed **relative offset** rather than an absolute controller timestamp.

Typical successful values:

- BROAD: 0, then -750 wire ticks after a -250 us libpad adjustment.
- BG entry: +900 wire ticks for a +300 us offset, then usually 0.

Therefore a Monado schedule already calibrated as "pulse centre relative to camera exposure" should convert that
relative centre directly to signed 3 MHz ticks when emitting BROAD/BG. It should not put an absolute device
timestamp into `cycle_position` outside PRESCAN.

## Successful right-controller timeline

Relative times below are from the Bluetooth oracle capture. They align to the second successful Windows session
starting at approximately 11:16:44.856.

| t (s) | Event |
|---:|---|
| 0.000 | INIT / zero schedule |
| 0.055 | LED_ALL_OFF |
| 0.087 | INIT with period 40 |
| 0.481 | PRESCAN / 40, all-LED mask |
| 2.776 | Sony tracking flag 0 -> 6; optical candidate/boot state |
| 3.960 | flag 6 -> 9: **full 6DoF while still PRESCAN** |
| 5.394 | BROAD / 42, offset 0 |
| 6.384 | LED mask first byte -> `0x0a` |
| 7.074 | base offset -> -250 us / wire -750 |
| 23.896 | LED mask -> `0x07` |
| 24.061 | PRESCAN / 40 |
| 27.076 | BROAD / 42 |
| 28.080 | mask `0x07` |
| 28.155 | mask `0x01` |
| 28.763 | offset -250 us / wire -750 |
| 31.283 | mask `0x0c` |
| 32.009 | tracking 9 -> 6: optical loss |
| 34.215 | mask `0x0a` |
| 34.607 | tracking 6 -> 9: **reacquired in BROAD** |
| 35.198 | mask `0x01` |
| 51.697 | BG / 30, +300 us / wire +900 |
| 52.725 | BG offset returns to 0, mask `0x07` |
| 53.722 | mask `0x01` |
| 55.283 | tracking 9 -> 3: lost while BG |
| 56.536 | mask `0x0a` |
| 56.722 | mask `0x07` |
| 56.931 | tracking 3 -> 9: **reacquired in BG** |
| 60.719 | mask `0x01` |
| 61.732 | mask `0x07` |
| 62.722 | mask `0x06` |
| 63.734 | mask `0x03` |
| 64.717 | PRESCAN / 40 |
| 67.733 | BROAD / 42 |
| 68.729 | mask `0x03` |
| 76.020 | offset -250 us / wire -750 |
| 78.457 | shutdown / INIT-zero packet |

The native transition policy clearly contains more state than a simple tracked/not-tracked switch:

- the controller reaches tracking flag 9 while still in PRESCAN;
- Sony changes PRESCAN -> BROAD while tracking remains valid;
- Sony later changes BROAD -> BG while tracking remains valid;
- Sony changes BG -> PRESCAN -> BROAD with no intervening logged tracking loss;
- tracking can be lost and reacquired without a phase change in either BROAD or BG.

Therefore **tracking freshness must not be treated as the phase state machine**. Phase changes look more like optical
identification/timing-maintenance modes around an independently valid pose estimate. Exact policy remains to be reverse
engineered. Monado should use the observed phases conservatively rather than claiming an exact clone of Sony's internal
state machine.

## LED masks

Sony changes the first byte of the four-byte LED mask dynamically while BROAD/BG are active. Observed first bytes in
this run included:

`ff, 0a, 07, 01, 0c, 06, 03`

The remaining three bytes stayed `ff` in the recovered A2/31 reports.

The distribution is phase-dependent:

- PRESCAN: 825/825 reports used `ff ff ff ff`.
- BROAD: only 225/4,085 reports used all-`ff`; most used `01`, `0a`, `03`, `0c` or `07`.
- BG: **0/1,001 reports used all-`ff`**; masks were `07`, `01`, `06`, `03` or briefly `0a`.

These changes correlate with Sony's `SET_LEDS_IMMEDIATE` commands and clearly form part of optical identification/
maintenance rather than being cosmetic. A particularly strong example is BROAD reacquisition at 11:17:19: Sony changes
the mask to `0a ff ff ff` and the tracking flag changes 6 -> 9 about 2 ms later.

Until the bit-to-physical-LED mapping and Sony's selection policy are captured, Monado should **not** assume that
`BG/30 + ff ff ff ff` is a native-like steady state; that combination never occurs in this successful capture.

## LED-pattern changes around loss and reacquisition

The second successful native session gives a stronger clue about the four-byte `leds[]` field. It should not yet be
treated as a literal 32-bit physical-LED mask, but its changes are tightly coupled to Sony's optical search state.

During the BROAD loss/reacquisition interval:

- `0x0c ff ff ff` was selected about 0.34 s before tracking flag 9 -> 6 and remained on wire throughout the ~2.6 s
  flag-6 interval;
- Sony then issued `SET_LEDS_IMMEDIATE 0x0a ff ff ff` essentially at the 6 -> 9 reacquisition transition;
- roughly one second later it moved to `0x01 ff ff ff`.

During the later BG loss/reacquisition interval:

- `0x01 ff ff ff` remained active through tracking flag 9 -> 3;
- Sony again issued `0x0a ff ff ff` immediately around the 3 -> 9 reacquisition;
- it then moved quickly through `0x07`, later `0x01`, `0x07`, `0x06`, and `0x03`.

This is consistent with the field selecting an optical identification/search pattern or LED subset, but the current
logs do not establish its physical meaning. The prepared Windows optical-capture branch records raw tracking frames plus
Sony's 4-camera x 17-LED blob associations so that the mapping can be measured directly.

## Schedule sequence number

The LED schedule sequence is a **generation/latch counter**, not an output-packet counter.

In the successful capture, many A2/31 reports were sent with the same LED sequence. During a long BROAD interval the
sequence remained at 20 for roughly 16.5 seconds despite continuous output traffic and repeated Sony housekeeping
commands.

This differs from the historical Monado implementation, which incremented the LED sequence every camera exposure.
That repeated relatch is unnecessary and is a plausible contributor to the controller's irreversible "tracking LEDs
stuck on" lockout.

Native-style mode therefore:

- increments the schedule sequence when the bootstrap/refinement output generation changes;
- increments it on an explicit phase/mask/base-time schedule mutation;
- does **not** increment it merely because another camera exposure or A2/31 packet occurred.

The PCAP makes this concrete: BROAD sequence 8 persists for about 16.8 seconds while Sony emits command type 6 roughly
once per second; the sequence changes to 9 only when the LED mask changes. Thus command type 6 is not itself a generic
"increment LED generation" operation.

The independent output report / haptics packet counters continue normally.

## Relation to Monado's always-lit lockout

Before this oracle capture, every located Monado lockout onset had two features:

1. the controller was in PRESCAN; and
2. a period-42 (2.1 ms) pulse had been sent within the preceding 1.5 seconds.

Monado also remained in PRESCAN permanently and re-latched the LED schedule every camera exposure.

The successful Sony driver instead uses:

- PRESCAN: period 40;
- BROAD: period 42;
- BG: period 30;
- no per-frame schedule relatch.

This does not prove that PRESCAN+42 or relatch frequency alone causes the hardware/firmware lockout, but it provides a
much stronger and safer operating envelope than the previous Monado scan behaviour.

## First Monado implementation

Branch: `experiment/pssense-native-led-phases`.

The successful trace now rules out the first heuristic implementation (`fresh pose -> BG`, `stale pose -> BROAD`),
so the branch has been made deliberately conservative.

`PSSENSE_NATIVE_LED_PHASES=1` is enabled by default on macOS and applies the two evidence-backed safety changes while
leaving the pose solver untouched:

1. **PRESCAN safety envelope**
   - initial/bootstrap scans remain PRESCAN;
   - the normal wide scan is capped at Sony's observed period 40 rather than period 42;
   - narrower bootstrap pulses remain narrow;
   - `cycle_position` remains an absolute device-time anchor.

2. **Native-style schedule relatching**
   - the LED sequence is no longer incremented every camera exposure;
   - bootstrap/refinement output-generation changes still relatch the schedule;
   - explicit phase changes relatch it;
   - while locked in PRESCAN with no other schedule change, Monado re-anchors the absolute `cycle_position` after
     60 camera exposure-sequence steps;
   - the native PCAP shows successive anchors separated by 60 or 61 camera periods (examples: 1,001,141 us =
     60.008 x 16,683.35 us and 1,017,750 us = 61.004 x), so this is better described as camera-cadence maintenance
     than an unrelated wall-clock 1 Hz timer;
   - BROAD/BG receive no periodic relatch because their native schedule uses relative offsets and the successful
     trace holds the same sequence for long intervals there.

By default the controller therefore remains in PRESCAN after lock. This is intentional: it isolates the two strongest
lockout hypotheses without introducing an unsupported BG mask/phase combination.

### Optional first phase transition

Set:

```
PSSENSE_NATIVE_LED_ADVANCE=1
```

to test only the first transition for which the native trace gives reasonable support:

- wait for timing/bootstrap lock;
- wait for the first accepted optical pose;
- hold PRESCAN for `PSSENSE_NATIVE_LED_ACQUIRE_HOLD_MS` (default 2000 ms);
- then enter BROAD/42;
- remain in BROAD while tracking continues or is temporarily lost;
- return to PRESCAN only when the bootstrap itself must rescan.

This approximates Sony's observed "acquire in PRESCAN, then continue in BROAD" behaviour without pretending that pose
validity controls the phase. BG is deliberately not emitted yet because native BG always used selective LED masks in
this capture and we do not yet know their physical/policy mapping.

For BROAD, `cycle_position` uses the calibrated pulse centre folded into the nearest signed camera-cycle offset and
converted to 3 MHz ticks, matching the native relative-offset semantics.

The bootstrap stores a **pulse-start** offset. When the output width changes, the scheduler reconstructs the centre from
the calibrated/source pulse width first, so PRESCAN -> BROAD does not shift the illuminated centre simply because the
pulse became wider. `PSSENSE_LED_PERIOD_ID`, when explicitly set, remains a true diagnostic output-width override.

## Test plan

Use the existing macOS runtime/session setup, with the same calibration and solver settings that already produce usable
6DoF.

Record at least:

- one controller, static then ordinary movement;
- both controllers;
- deliberate occlusion/loss and reacquisition;
- at least several restart/power-cycle sessions to estimate lockout incidence.

Useful log events:

```
LED_NATIVE_PHASE side=R event=optical_acquired; holding PRESCAN before BROAD
LED_NATIVE_PHASE side=R old=1 new=2 source_period=20 output_period=42 ...
```

Acceptance for the first pass:

- no controller enters the irreversible always-lit/status-LED-off lockout;
- with no explicit diagnostic period override, the default wide scan no longer emits PRESCAN/42;
- in safety-only mode, valid optical poses still reach the runtime without irreversible lockout;
- with `PSSENSE_NATIVE_LED_ADVANCE=1`, acquisition remains in PRESCAN for the hold interval then changes once to
  BROAD/42 without losing the already-working pose solver;
- temporary optical loss/reacquisition can occur while remaining in BROAD;
- no regression in pose jitter/age compared with the same solver on `macos-pssense-6dof`.

Set `PSSENSE_NATIVE_LED_PHASES=0` to restore the historical scheduling for comparison. Keep
`PSSENSE_NATIVE_LED_ADVANCE=0` for the minimal lockout-safety test.

## Upstream provenance blocker: current controller clock estimator

The experimental `pssense_driver.c` clock-offset estimator also has legacy research provenance: comments in the
original branch explicitly said that it followed PSVR2Toolkit's Sense clock-tracking implementation.

That is acceptable for local experimentation, but it is not an upstream-clean implementation under the clean-room
rule. Before upstreaming, replace it with an independently specified estimator whose inputs are only observable facts:

- controller/device timestamp carried in incoming HID reports;
- host/QPC receive timestamp;
- wraparound modulus and timestamp units established from the wire;
- an explicit transport-delay/noise model.

A suitable independent design is a conventional lower-envelope/minimum-delay clock-offset estimator plus a slow drift
model and bounded smoothing. Its parameters should be justified by recorded timestamp-pair traces rather than copied
from Toolkit constants. The current implementation is intentionally left unchanged on this experimental branch so the
LED scheduling experiments do not simultaneously change the already-working tracking clock.

## Upstream provenance blocker: current LED geometry

The current experimental Monado file `src/xrt/drivers/pssense/pssense_led_model.h` explicitly states that its 17 LED
coordinates were extracted from Sony's PC driver. Under the PSVR2Toolkit `AGENTS.md` clean-room policy, that table is
not suitable provenance for an eventual upstream contribution.

It remains useful for local experimental validation of the optical pipeline, but upstream-oriented work should replace
it with geometry derived independently from observable behaviour or direct hardware measurement.

Prepared clean-room route:

1. capture raw VI camera packets directly from PSVR2 USB interface 6 / endpoint 0x87;
2. capture HMD and Sense poses Sony publishes through the standard OpenVR driver-host interface;
3. use independently obtained HMD camera calibration;
4. identify/associate visible IR emitters across stereo frames and multiple controller poses;
5. triangulate emitter positions into controller coordinates and fit/refine the constellation;
6. validate the independently derived model on held-out frames and with Monado's existing pose solver.

The Sony-derived coordinate table must not be used as a target, prior or optimisation constraint for that
reconstruction.

## Remaining reverse-engineering

- Exact opcode name for Sony command type 6. Timing strongly suggests a PRESCAN absolute-epoch maintenance operation:
  while in PRESCAN it occurs about once per second and is followed by `seq += 1` plus a median base-time advance of
  1,000,991 us; in BROAD/BG, the same periodic command usually leaves sequence and relative base offset unchanged.
  The next Toolkit capture logs its two payload bytes to identify it more precisely.
- Exact policy that causes BROAD -> BG, BG -> PRESCAN, and subsequent PRESCAN -> BROAD.
- Selective LED-pattern meaning and physical-emitter mapping. The prepared Toolkit capture saves raw VI packets
  before and after native phase/mask changes, periodic VI samples, the observable IF8/0x89 LED-detector USB stream,
  and poses Sony publishes through OpenVR. Semantics-free byte-lane differencing plus static-controller captures can
  therefore answer this without Sony internal tracker data.
- Whether STABLE is used in longer/cleaner sessions and what condition enters it.
- Whether the native fixed output mode byte `0xA2` matters for long-term LED reliability; Monado's current output
  formatting is already accepted by the controllers and is not changed in this experiment.
- Whether period 42 is intrinsically unsafe in PRESCAN or whether the old fault requires the combination of period 42
  and high-frequency sequence relatching.

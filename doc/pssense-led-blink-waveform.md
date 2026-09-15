# PS VR2 Sense `led_blink[4]` semantics

This note records the latch-aware macOS hardware experiment used to determine whether the four Sense `led_blink` bytes are a spatial per-LED mask or a temporal blink waveform.

## Result

The 32 bits behave as a shared temporal waveform, not as selectors for the 17 physical IR emitters.

The decisive experiment held each single bit unchanged for 500 ms while recording synchronized PS VR2 mode-4 camera images. The probe scanned all 32 bits and explicitly advanced the Sense LED settings sequence number once per mask transition, confirming that every setting was latched by the controller. Repeated 10 ms keepalive writes within each segment retained the same LED sequence number.

While an individual bit remained unchanged, successive camera frames alternated between dark frames and frames containing several of the same controller constellation points seen in the all-on reference. A static spatial LED-selection bit cannot change which emitters are enabled while the bit value is unchanged. Different camera pairs also observed the transitions at different phases, consistent with a temporal emission waveform interacting with camera exposure timing.

When a single bit produced a lit frame, it illuminated the same multi-point spatial constellation rather than isolating one emitter. High-frame-rate phone video of a Sense controller driven by a PS5 independently showed the visible IR emitters switching together.

The practical model is therefore:

```
led_blink[4]
    -> 32 temporal emission slots
    -> shared by the enabled tracking constellation
    -> visible Sense IR emitters pulse together
```

`{0xff, 0xff, 0xff, 0xff}` should consequently be understood as enabling every temporal slot rather than selecting every physical LED. In the macOS diagnostic FORCE-IR path this combines with the repeating PRESCAN timing to produce continuous-equivalent illumination across camera exposures.

## Reproducible diagnostic

Keep the headset and one Sense controller stationary. Record mode 4 for long enough to cover 36 500-ms segments:

```sh
.venv/bin/python scripts/psvr2_camera_mode_survey.py \
  /tmp/psvr2-mode4-mask-test \
  --sequence 4 \
  --settle 0.5 \
  --sample 22 \
  --examples 500 \
  --save-every 5 \
  --no-contact-sheet
```

In a second terminal run the latch-aware 32-bit scan:

```sh
PSSENSE_TIMING_DIAG=1 \
./build-sense/src/xrt/auxiliary/os/pssense_hid_probe \
  --hand left \
  --force-ir-mask-scan /tmp/psvr2-mode4-mask-test/led-mask-schedule.csv \
  --mask-segment-ms 500
```

A current build reports 36 segments and emits a line such as
`PSSENSE_FORCE_IR latched LED mask=00000001 led_seq=3` for each transition.

Then run:

```sh
.venv/bin/python scripts/psvr2_tracking_mask_analyze.py \
  /tmp/psvr2-mode4-mask-test \
  /tmp/psvr2-mode4-mask-test/led-mask-schedule.csv \
  --output /tmp/psvr2-mode4-mask-test/mask-analysis.json
```

The v3 analyser examines individual frames as well as median images. If a constant single-bit segment contains both dark frames and lit frames with multiple all-on-matching constellation points, it reports `temporal_waveform_supported` and records the supporting camera/bit/frame counts under `temporal_waveform_evidence`.

## Tracking consequence

Temporal coding does not provide the physical LED identity required by the constellation solver. LED identity must come from the rigid 17-point geometry, camera calibration, an IMU/optical pose prior, and temporal blob association. The live Sense path should therefore treat the synchronized four-camera epoch as the optical measurement and avoid allowing an isolated single-camera correspondence error to seed subsequent tracker state.

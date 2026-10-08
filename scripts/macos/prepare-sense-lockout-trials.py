#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Prepare three controlled LED-lockout trial profiles; never operate hardware or launchd."""
import argparse
from pathlib import Path
import plistlib

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--base-plist', type=Path, default=root / 'build/arm64/local-sense-default/org.freedesktop.monado.service.plist')
parser.add_argument('--output', type=Path, required=True, help='New evidence directory; existing directories are rejected')
args = parser.parse_args()
base = plistlib.loads(args.base_plist.read_bytes())
if base.get('EnvironmentVariables', {}).get('PSVR2_SENSE_6DOF') != '1':
    parser.error('base profile must enable runtime Sense tracking')
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=False)
for name, probes, stress in [('A-steady', '0', '0'), ('B-probes', '1', '0'), ('C-rescans', '0', '20')]:
    trial = out / name
    trial.mkdir()
    profile = plistlib.loads(plistlib.dumps(base))
    profile['EnvironmentVariables'].update(
        PSSENSE_TIMING_DIAG='1', PSSENSE_INPUT_DIAG='1',
        PSSENSE_LED_BOOTSTRAP_TRACK=probes,
        PSSENSE_LED_BOOTSTRAP_STRESS_RESCAN_S=stress,
        PSSENSE_LED_BOOTSTRAP_WIDE_PERIOD_ID='32',
        CONSTELLATION_TRACKER_DATA_RECORDER_OUTPUT=str(trial / 'constellation.ctd'),
        IPC_EXIT_WHEN_IDLE='1', IPC_EXIT_WHEN_IDLE_DELAY_MS='5000')
    profile['StandardOutPath'] = profile['StandardErrorPath'] = str(trial / 'service.log')
    (trial / 'service.plist').write_bytes(plistlib.dumps(profile))
(out / 'README.txt').write_text('''Controlled Sense LED-lockout trials

Use the same optimised service/runtime, calibration, camera settings and controller pair throughout.
Power-cycle BOTH controllers before EACH trial; a service restart does not clear an existing lockout.
Close clients before switching profiles. Unload both normal and isolated service registrations, then
bootstrap ONE trial service.plist (these deliberately reuse the normal service label and endpoint).
Launch build/arm64's psvr2-openxr-test with the normal native-client.env and --generic-controller.
Keep the client OPEN and both rings unobstructed 30-50 cm from the headset for 180 seconds.
A: probes disabled, no forced rescan. Steady lock is the control; log any spontaneous loss/rescan.
B: probes enabled, no forced rescan. Same visibility and duration as A.
C: probes disabled, forced FULL rescan every 20 seconds after lock. Rings stay visible.
Stop a trial on observed status-LED-off / IR-always-on fault; record hand and approximate elapsed time.
Record whether buttons/triggers still respond in the visual panels. Observe status LEDs externally;
this run does not rely on automatic stuck_lit detection, which missed the last occurrence.

Do not compare a clean run with a run starting from stuck controllers. Repeat suspicious comparisons;
three minutes without a fault is not proof of prevention. Preserve service.log, constellation.ctd,
service.plist and an observation note for every run; regenerate into a NEW directory for repeats.
After testing, restore the persistent normal service profile. Keep normal idle exit enabled.
The opt-in trace adds logging overhead equally to all three cases. Confirm any chosen mitigation
again with verbose tracing disabled before treating it as a default.

Interpretation:
A fails before any loss/rescan: investigate steady command stream, cadence and scheduling first.
Only B fails reproducibly: phase-adjustment probes are implicated.
Only C fails reproducibly: full-scan transitions are implicated.
A faults after a spontaneous rescan: it was not a pure steady-lock control; repeat.
''')
print('Prepared', out)
print('No service registration changed; no hardware started. Read README.txt before the user runs the trials.')

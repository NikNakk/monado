#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""How optical tracking success depends on the controller's orientation and rotation speed.

Every exposure has an IMU orientation, tracked or not. It is carried into the optical world with the alignment from
the nearest optical pose (optical = align * IMU, re-estimated per pose, so gyro drift is followed), and the position
comes from the nearest optical pose. From that it computes, per exposure:

- ring angle: between the plane of the LED ring's normal and the direction to the headset, folded to 0-90 deg
  (0 = ring face-on to the headset, 90 = edge-on). The Sense LEDs point all round the ring (their mean normal has
  length 0.16), so the ring has no "front" and a facing angle from the mean normal says little;
- LEDs facing a camera: LEDs whose normal is within 70 deg of some camera's direction;
- distance from the headset (mean camera position);
- rotation speed from successive IMU orientations;

and reports the fraction of exposures with an optical pose, per bin. Inputs come from constellation_replay:

    constellation_replay DATASET.ctd --calibration CAL.json --geometry PREFIX --tracking-csv TRK.csv \\
        --tracker-csv TR.csv          (with CONSTELLATION_TRACKER_JOINT=1)
    psvr2_sense_rotation_coverage.py PREFIX TRK.csv TR.csv
"""
from __future__ import annotations

import argparse
import csv
from collections import defaultdict

import numpy as np
from scipy.spatial.transform import Rotation

C = np.diag([1.0, -1.0, -1.0])  # OpenCV <-> OpenXR axes
MAX_GAP_NS = 10_000_000_000  # nearest optical pose must be this close for alignment and position
FACING_LIMIT_DEG = 70.0


def load(prefix: str, trk_path: str, tr_path: str):
    cams = defaultdict(dict)  # ts -> camera -> (pos, quat) XR
    for r in csv.DictReader(open(prefix + "-cameras.csv")):
        cams[int(r["timestamp_ns"])][int(r["camera"])] = (
            np.array([float(r[k]) for k in ("px", "py", "pz")]),
            np.array([float(r[k]) for k in ("qx", "qy", "qz", "qw")]))
    leds = defaultdict(list)
    led_pos = defaultdict(list)
    for r in csv.DictReader(open(prefix + "-leds.csv")):
        leds[int(r["device"])].append(np.array([float(r[k]) for k in ("nx", "ny", "nz")]))
        led_pos[int(r["device"])].append(np.array([float(r[k]) for k in ("px", "py", "pz")]))
    imu = defaultdict(dict)  # device -> ts -> quat (IMU world)
    for r in csv.DictReader(open(trk_path)):
        if int(r["camera"]) == 0 and int(r["flags"]) & 1:
            imu[int(r["device"])][int(r["timestamp_ns"])] = np.array([float(r[k]) for k in ("qx", "qy", "qz", "qw")])
    opt = defaultdict(dict)  # device -> ts -> (pos, quat) XR optical world
    for r in csv.DictReader(open(tr_path)):
        opt[int(r["device"])][int(r["timestamp_ns"])] = (
            np.array([float(r[k]) for k in ("px", "py", "pz")]),
            np.array([float(r[k]) for k in ("qx", "qy", "qz", "qw")]))
    return cams, leds, led_pos, imu, opt


def table(title: str, values: np.ndarray, tracked: np.ndarray, edges: list[float]) -> None:
    print(f"  {title}")
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (values >= lo) & (values < hi)
        if m.sum() == 0:
            continue
        print(f"    {lo:6.0f}-{hi:<6.0f} exposures {m.sum():5d}  tracked {tracked[m].mean() * 100:5.1f}%")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("geometry_prefix")
    parser.add_argument("tracking_csv")
    parser.add_argument("tracker_csv")
    args = parser.parse_args()
    cams, leds, led_pos, imu, opt = load(args.geometry_prefix, args.tracking_csv, args.tracker_csv)

    for device in sorted(imu):
        normals_cv = np.array(leds[device])
        positions_cv = np.array(led_pos[device])
        centred = positions_cv - positions_cv.mean(axis=0)
        plane_normal_cv = np.linalg.svd(centred)[2][2]
        opt_ts = np.array(sorted(opt[device]))
        if len(opt_ts) < 100:
            continue
        # Per optical pose, the IMU->optical alignment.
        aligns = {}
        for t in opt_ts:
            if t in imu[device]:
                aligns[t] = Rotation.from_quat(opt[device][t][1]) * Rotation.from_quat(imu[device][t]).inv()
        align_ts = np.array(sorted(aligns))

        facing, facing_leds, speed, tracked, when, distance = [], [], [], [], [], []
        prev = None
        t0 = min(imu[device])
        for t in sorted(imu[device]):
            q_imu = Rotation.from_quat(imu[device][t])
            if prev is not None and 0 < t - prev[0] < 40_000_000:
                w = (q_imu * prev[1].inv()).magnitude() / ((t - prev[0]) * 1e-9)
            else:
                w = np.nan
            prev = (t, q_imu)
            # Only after the first optical pose: before it the LEDs are still being brought into phase.
            if len(align_ts) == 0 or t not in cams or t < opt_ts[0]:
                continue
            i = np.searchsorted(align_ts, t)
            cand = [align_ts[j] for j in (i - 1, i) if 0 <= j < len(align_ts)]
            near = min(cand, key=lambda x: abs(x - t))
            if abs(near - t) > MAX_GAP_NS:
                continue
            j = np.searchsorted(opt_ts, t)
            pcand = [opt_ts[k] for k in (j - 1, j) if 0 <= k < len(opt_ts)]
            pnear = min(pcand, key=lambda x: abs(x - t))
            pos_xr = opt[device][pnear][0]
            R_xr = (aligns[near] * q_imu).as_matrix()
            R_cv = C @ R_xr @ C
            p_cv = C @ pos_xr
            normal_w = R_cv @ plane_normal_cv
            normals_w = normals_cv @ R_cv.T
            head = np.mean([C @ cams[t][c][0] for c in cams[t]], axis=0)
            to_head = head - p_cv
            to_head /= np.linalg.norm(to_head)
            facing.append(np.degrees(np.arccos(np.clip(abs(normal_w @ to_head), 0, 1))))
            best = 0
            for c in cams[t]:
                d = C @ cams[t][c][0] - p_cv
                d /= np.linalg.norm(d)
                best = max(best, int((normals_w @ d > np.cos(np.radians(FACING_LIMIT_DEG))).sum()))
            facing_leds.append(best)
            distance.append(np.linalg.norm(pos_xr - np.mean([cams[t][c][0] for c in cams[t]], axis=0)))
            speed.append(np.degrees(w))
            tracked.append(t in opt[device])
            when.append((t - t0) * 1e-9)
        facing, facing_leds, speed, tracked, distance = map(
            np.array, (facing, facing_leds, speed, tracked, distance))
        print(f"device {device}: {len(tracked)} exposures after the first optical pose, within 10 s of one, tracked "
              f"{tracked.mean() * 100:.1f}%")
        table("ring plane angle to the headset (deg; 0 face-on, 90 edge-on)", facing, tracked,
              [0, 15, 30, 45, 60, 75, 91])
        table("most LEDs facing one camera (normal within 70 deg)", facing_leds.astype(float), tracked,
              [0, 3, 5, 7, 9, 11, 18])
        table("distance from the headset (cm)", distance * 100, tracked, [0, 20, 30, 40, 50, 60, 80, 200])
        ok = ~np.isnan(speed)
        table("rotation speed (deg/s)", speed[ok], tracked[ok], [0, 30, 90, 180, 360, 720, 5000])
        if tracked.any():
            print(f"  fastest tracked rotation p95 "
                  f"{np.nanpercentile(speed[tracked], 95):.0f} deg/s, max {np.nanmax(speed[tracked]):.0f} deg/s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

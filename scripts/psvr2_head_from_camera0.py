#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Estimate head_from_camera0 (where camera 0 sits relative to the PS VR2 head pose) from resting controllers.

Record with PSVR2_CONSTELLATION_WORLD=1 while the controllers rest and only the head moves (turn, nod, tilt). In a
world-frame recording camera 0's world pose is head * X_recorded, so for every exposure

    head_i = camera0_i * X_recorded^-1        controller in camera 0: C_i = camera0_i^-1 * world_i

and while a controller rests its true world pose head_i * X * C_i is constant. X is found by the classic hand-eye
solution (A X = X B over pairs of exposures within each rest) and refined jointly with each rest's world pose by
robust least squares. Inputs come from constellation_replay (joint tracker):

    CONSTELLATION_TRACKER_JOINT=1 constellation_replay SESSION/constellation.ctd --geometry PREFIX \\
        --tracking-csv TRK.csv --tracker-csv TR.csv
    psvr2_head_from_camera0.py PREFIX TRK.csv TR.csv --recorded-calibration SESSION/calibration.json \\
        --output CALIBRATION-with-head.json

The output is the recorded calibration plus head_from_camera0_xrt and the fit; runtime_usable stays false.
"""
from __future__ import annotations

import argparse
import copy
import csv
import json
from collections import defaultdict
from pathlib import Path

import numpy as np
from scipy.optimize import least_squares
from scipy.spatial.transform import Rotation

ROTATION_WEIGHT_M = 0.1  # 1 rad of world-orientation spread costs as much as 10 cm of position spread


class Pose:
    def __init__(self, R: Rotation, t: np.ndarray):
        self.R, self.t = R, np.asarray(t, float)

    def __mul__(self, other: "Pose") -> "Pose":
        return Pose(self.R * other.R, self.R.apply(other.t) + self.t)

    def inv(self) -> "Pose":
        Ri = self.R.inv()
        return Pose(Ri, -Ri.apply(self.t))

    @staticmethod
    def from_json(obj) -> "Pose":
        o, p = obj["orientation"], obj["position"]
        return Pose(Rotation.from_quat([o["x"], o["y"], o["z"], o["w"]]), [p["x"], p["y"], p["z"]])

    def to_json(self) -> dict:
        q = self.R.as_quat()
        return {"orientation": {"x": q[0], "y": q[1], "z": q[2], "w": q[3]},
                "position": {"x": self.t[0], "y": self.t[1], "z": self.t[2]}}


def identity() -> Pose:
    return Pose(Rotation.identity(), np.zeros(3))


def rests(imu: dict[int, np.ndarray], opt: dict[int, Pose], max_speed_deg_s: float, min_s: float) -> list[list[int]]:
    """Runs of exposures with an optical pose while the controller's IMU orientation barely changes."""
    out, run, prev = [], [], None
    for t in sorted(imu):
        q = Rotation.from_quat(imu[t])
        still = prev is not None and 0 < t - prev[0] < 50_000_000 and \
            np.degrees((q * prev[1].inv()).magnitude()) / ((t - prev[0]) * 1e-9) < max_speed_deg_s
        prev = (t, q)
        if still:
            if t in opt:
                run.append(t)
        else:
            if run and (run[-1] - run[0]) * 1e-9 >= min_s:
                out.append(run)
            run = []
    if run and (run[-1] - run[0]) * 1e-9 >= min_s:
        out.append(run)
    return out


def hand_eye_init(segments: list[list[tuple[Pose, Pose]]], rng: np.random.Generator) -> Pose:
    """Rotation from A X = X B axis alignment (Kabsch on rotation vectors), then translation by linear least squares."""
    pairs = []
    for seg in segments:
        n = len(seg)
        for _ in range(min(400, n * (n - 1) // 2)):
            i, j = rng.choice(n, 2, replace=False)
            A = seg[j][0].inv() * seg[i][0]
            B = seg[j][1] * seg[i][1].inv()
            if A.R.magnitude() > np.radians(5):
                pairs.append((A, B))
    if len(pairs) < 3:
        raise SystemExit("not enough head rotation while the controllers rested (need turns of > 5 deg)")
    a = np.array([p[0].R.as_rotvec() for p in pairs])
    b = np.array([p[1].R.as_rotvec() for p in pairs])
    R_x, _ = Rotation.align_vectors(a, b)  # a = R_x b
    M = np.vstack([p[0].R.as_matrix() - np.eye(3) for p in pairs])
    v = np.concatenate([R_x.apply(p[1].t) - p[0].t for p in pairs])
    t_x, *_ = np.linalg.lstsq(M, v, rcond=None)
    return Pose(R_x, t_x)


def spread(segments: list[list[tuple[Pose, Pose]]], X: Pose) -> tuple[np.ndarray, np.ndarray]:
    """Per-exposure distance (mm) and angle (deg) of head*X*C from each rest's median world pose."""
    dist, ang = [], []
    for seg in segments:
        W = [h * X * c for h, c in seg]
        P = np.array([w.t for w in W])
        centre = np.median(P, axis=0)
        mean_R = Rotation.concatenate([w.R for w in W]).mean()
        dist += list(np.linalg.norm(P - centre, axis=1) * 1000)
        ang += [np.degrees((mean_R.inv() * w.R).magnitude()) for w in W]
    return np.array(dist), np.array(ang)


def refine(segments: list[list[tuple[Pose, Pose]]], X0: Pose) -> Pose:
    W0 = []
    for seg in segments:
        W = [h * X0 * c for h, c in seg]
        W0.append(Pose(Rotation.concatenate([w.R for w in W]).mean(), np.median([w.t for w in W], axis=0)))
    x0 = np.concatenate([X0.R.as_rotvec(), X0.t] + [np.concatenate([w.R.as_rotvec(), w.t]) for w in W0])

    def residuals(x):
        X = Pose(Rotation.from_rotvec(x[:3]), x[3:6])
        out = []
        for k, seg in enumerate(segments):
            W = Pose(Rotation.from_rotvec(x[6 + 6 * k:9 + 6 * k]), x[9 + 6 * k:12 + 6 * k])
            Wi = W.inv()
            for h, c in seg:
                d = Wi * (h * X * c)
                out.append(d.t)
                out.append(d.R.as_rotvec() * ROTATION_WEIGHT_M)
        return np.concatenate(out)

    fit = least_squares(residuals, x0, loss="huber", f_scale=0.005)
    return Pose(Rotation.from_rotvec(fit.x[:3]), fit.x[3:6])


def load(prefix: str, trk_path: str, tr_path: str):
    cam0 = {}
    for r in csv.DictReader(open(prefix + "-cameras.csv")):
        if r["camera"] == "0":
            cam0[int(r["timestamp_ns"])] = Pose(Rotation.from_quat([float(r[k]) for k in ("qx", "qy", "qz", "qw")]),
                                                [float(r[k]) for k in ("px", "py", "pz")])
    imu = defaultdict(dict)
    for r in csv.DictReader(open(trk_path)):
        if r["camera"] == "0" and int(r["flags"]) & 1:
            imu[int(r["device"])][int(r["timestamp_ns"])] = np.array([float(r[k]) for k in ("qx", "qy", "qz", "qw")])
    opt = defaultdict(dict)
    for r in csv.DictReader(open(tr_path)):
        opt[int(r["device"])][int(r["timestamp_ns"])] = Pose(
            Rotation.from_quat([float(r[k]) for k in ("qx", "qy", "qz", "qw")]), [float(r[k]) for k in ("px", "py", "pz")])
    return cam0, imu, opt


def build_segments(cam0, imu, opt, X_recorded: Pose, max_speed: float, min_s: float):
    X_rec_inv = X_recorded.inv()
    segments, info = [], []
    for device in sorted(opt):
        for run in rests(imu[device], opt[device], max_speed, min_s):
            seg = [(cam0[t] * X_rec_inv, cam0[t].inv() * opt[device][t]) for t in run if t in cam0]
            if len(seg) < 20:
                continue
            heads = Rotation.concatenate([h.R for h, _ in seg])
            head_range = max(np.degrees((heads[0].inv() * heads[k]).magnitude()) for k in range(len(seg)))
            segments.append(seg)
            info.append((device, (run[0] - min(cam0)) * 1e-9, (run[-1] - run[0]) * 1e-9, len(seg), head_range))
    return segments, info


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("geometry_prefix")
    parser.add_argument("tracking_csv")
    parser.add_argument("tracker_csv")
    parser.add_argument("--recorded-calibration", type=Path, required=True,
                        help="the session's calibration.json (its head_from_camera0_xrt, identity if absent)")
    parser.add_argument("--output", type=Path, help="write the recorded calibration plus head_from_camera0_xrt")
    parser.add_argument("--max-rest-speed", type=float, default=3.0, help="deg/s of controller rotation still counted as rest")
    parser.add_argument("--min-rest", type=float, default=1.0, help="shortest rest used, seconds")
    args = parser.parse_args()

    calibration = json.loads(args.recorded_calibration.read_text())
    X_recorded = Pose.from_json(calibration["head_from_camera0_xrt"]) if "head_from_camera0_xrt" in calibration \
        else identity()
    cam0, imu, opt = load(args.geometry_prefix, args.tracking_csv, args.tracker_csv)
    segments, info = build_segments(cam0, imu, opt, X_recorded, args.max_rest_speed, args.min_rest)
    print(f"{len(segments)} rests")
    for device, start, length, n, head_range in info:
        print(f"  device {device} at {start:5.1f} s for {length:4.1f} s: {n:4d} poses, head turned up to {head_range:5.1f} deg")
    if not segments:
        print("no rests with optical poses; is this a PSVR2_CONSTELLATION_WORLD=1 recording with resting controllers?")
        return 1

    X_init = hand_eye_init(segments, np.random.default_rng(0))
    X = refine(segments, X_init)
    for name, pose in (("recorded", X_recorded), ("hand-eye", X_init), ("refined", X)):
        d, a = spread(segments, pose)
        print(f"{name:9s} world spread of resting controllers: mm p50 {np.median(d):6.1f} p95 {np.percentile(d, 95):6.1f}; "
              f"deg p50 {np.median(a):5.2f} p95 {np.percentile(a, 95):5.2f}")
    print("head_from_camera0: position m", np.round(X.t, 4), " rotation deg", round(np.degrees(X.R.magnitude()), 2),
          " about", np.round(X.R.as_rotvec() / max(X.R.magnitude(), 1e-9), 3))

    if args.output:
        if str(args.output.resolve()).startswith(("/tmp", "/private/tmp")):
            raise SystemExit("refusing to write under /tmp")
        out = copy.deepcopy(calibration)
        out["runtime_usable"] = False
        out["head_from_camera0_xrt"] = X.to_json()
        d, a = spread(segments, X)
        out["head_from_camera0_fit"] = {
            "method": "hand-eye (A X = X B) then robust joint refinement over resting controllers",
            "rests": len(segments), "poses": int(sum(len(s) for s in segments)),
            "spread_mm_p50": float(np.median(d)), "spread_mm_p95": float(np.percentile(d, 95)),
            "spread_deg_p50": float(np.median(a)), "spread_deg_p95": float(np.percentile(a, 95)),
        }
        args.output.write_text(json.dumps(out, indent=2) + "\n")
        print("wrote", args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

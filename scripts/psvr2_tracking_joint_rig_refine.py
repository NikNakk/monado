#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Constrained multi-pose PSVR2 mode-4 rig refinement from forced PRESCAN data.

Cameras 0/1 are used as a metric stereo anchor. Their PRESCAN points are
matched without LED identities, triangulated into rig-space 3D points, and
checked against only the Sense model's pair-distance distribution. Cameras 2/3
are then fitted to those measured 3D points across six poses. Fisheye D is kept
fixed; K and upper-camera extrinsics are allowed to move only inside broad
physical bounds. The output is diagnostic and is never marked runtime-usable.
"""
from __future__ import annotations

import argparse
import itertools
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

try:
    from scipy.optimize import least_squares
except ImportError as exc:
    raise SystemExit("This offline solver requires scipy: python3 -m pip install scipy") from exc

ACTIVE_W = 508
MODE4 = {0: (4, 0), 1: (4, 1), 2: (5, 0), 3: (5, 1)}


@dataclass
class Camera:
    K: np.ndarray
    D: np.ndarray
    T: np.ndarray  # T_rig_camera


def pose_matrix(rvec, tvec):
    out = np.eye(4)
    out[:3, :3] = cv2.Rodrigues(
        np.ascontiguousarray(np.asarray(rvec).reshape(3, 1), dtype=np.float64)
    )[0]
    out[:3, 3] = np.asarray(tvec).reshape(3)
    return out


def pose_params(T):
    rvec = cv2.Rodrigues(np.ascontiguousarray(T[:3, :3], dtype=np.float64))[0].reshape(3)
    return np.r_[rvec, T[:3, 3]]


def params_pose(x):
    return pose_matrix(x[:3], x[3:6])


def rot_delta(a, b):
    r = a[:3, :3].T @ b[:3, :3]
    return float(np.arccos(np.clip((np.trace(r) - 1.0) * 0.5, -1.0, 1.0)))


def load_cameras(data):
    if data.get("format") != "psvr2-mode4-constellation-calibration-v1":
        raise ValueError("expected psvr2-mode4-constellation-calibration-v1")
    result = []
    for index, item in enumerate(data["cameras"]):
        if item["camera"] != index:
            raise ValueError("camera entries must be ordered 0..3")
        intrinsics = item["calibration"]["intrinsics"]
        distortion = item["calibration"]["distortion"]
        K = np.array(
            [
                [intrinsics["fx"], 0, intrinsics["cx"]],
                [0, intrinsics["fy"], intrinsics["cy"]],
                [0, 0, 1],
            ],
            dtype=np.float64,
        )
        D = np.array(
            [distortion["k1"], distortion["k2"], distortion["k3"], distortion["k4"]],
            dtype=np.float64,
        )
        T = np.array(item["transform_to_rig_T_rig_camera_opencv"], dtype=np.float64)
        result.append(Camera(K, D, T))
    return result


def load_led_positions(repo_root, hand):
    text = (repo_root / "src/xrt/drivers/pssense/pssense_led_model.h").read_text()
    section = text.split(f"pssense_{hand}_leds[] = {{", 1)[1]
    if hand == "left":
        section = section.split("pssense_right_leds[]", 1)[0]
    else:
        section = section.split("};", 1)[0]
    blocks = re.findall(r"\.position = \{([^}]+)\}", section)
    points = np.array([[float(v) for v in block.split(",")] for block in blocks], dtype=np.float64)
    if points.shape != (17, 3):
        raise ValueError(f"expected 17 {hand} LEDs, got {len(points)}")
    return points


def mapped_K_from_base(path):
    if path is None:
        return {}
    from psvr2_tracking_geometry import load_camera_models
    from psvr2_tracking_intrinsics_refine import initial_intrinsics

    models = load_camera_models(json.loads(path.read_text()))
    return {i: np.asarray(initial_intrinsics(model), dtype=np.float64) for i, model in enumerate(models)}


def pose_dirs(root):
    out = []
    for i in range(6):
        found = sorted(p for p in root.glob(f"F{i:02d}*") if p.is_dir())
        if len(found) != 1:
            raise ValueError(f"expected exactly one F{i:02d}* directory, found {len(found)}")
        out.append(found[0])
    return out


def merged_blinks(root, camera):
    camera_set, plane = MODE4[camera]
    paths = sorted(root.glob(f"mode-04-size-*-set-{camera_set}-example-*-plane{plane}.pgm"))
    images = []
    for path in paths:
        image = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if image is None or image.shape != (508, 512):
            raise ValueError(f"bad mode-4 image: {path}")
        image = image.copy()
        image[:, ACTIVE_W:] = 0
        images.append(image)
    if len(images) < 2:
        raise ValueError(f"{root}: insufficient camera {camera} frames")
    stack = np.stack(images)
    delta = stack.max(0).astype(np.int16) - stack.min(0).astype(np.int16)
    count, _, stats, centroids = cv2.connectedComponentsWithStats((delta >= 80).astype(np.uint8), 8)
    pending = [
        centroids[i]
        for i in range(1, count)
        if 1 <= stats[i, cv2.CC_STAT_AREA] <= 100 and centroids[i][0] < ACTIVE_W
    ]
    result = []
    while pending:
        group = [np.asarray(pending.pop(0), dtype=np.float64)]
        changed = True
        while changed:
            changed = False
            centre = np.mean(group, axis=0)
            keep = []
            for point in pending:
                if np.linalg.norm(point - centre) <= 4.5:
                    group.append(np.asarray(point, dtype=np.float64))
                    changed = True
                else:
                    keep.append(point)
            pending = keep
        result.append(np.mean(group, axis=0))
    return np.asarray(result, dtype=np.float64).reshape(-1, 2)


def stereo_cloud(cam0, cam1, obs0, obs1, model_distances):
    u0 = cv2.fisheye.undistortPoints(obs0.reshape(-1, 1, 2), cam0.K, cam0.D).reshape(-1, 2)
    u1 = cv2.fisheye.undistortPoints(obs1.reshape(-1, 1, 2), cam1.K, cam1.D).reshape(-1, 2)
    T10 = np.linalg.inv(cam1.T) @ cam0.T
    P0 = np.c_[np.eye(3), np.zeros(3)]
    P1 = T10[:3]
    zero_large = len(u0) >= len(u1)
    large, small = (u0, u1) if zero_large else (u1, u0)
    best = None
    for perm in itertools.permutations(range(len(large)), len(small)):
        a = np.array([u0[i] for i in perm]) if zero_large else u0
        b = u1 if zero_large else np.array([u1[i] for i in perm])
        Xh = cv2.triangulatePoints(P0, P1, a.T, b.T)
        X0 = (Xh[:3] / Xh[3]).T
        X1 = (T10[:3, :3] @ X0.T).T + T10[:3, 3]
        if np.any(X0[:, 2] < 0.20) or np.any(X0[:, 2] > 1.20) or np.any(X1[:, 2] < 0.20):
            continue
        pairs = np.array(
            [np.linalg.norm(X0[i] - X0[j]) for i, j in itertools.combinations(range(len(X0)), 2)]
        )
        diameter = float(pairs.max()) if len(pairs) else 0.0
        if diameter > 0.18:
            continue
        pair_error = (
            float(np.mean([np.min(np.abs(model_distances - distance)) for distance in pairs]))
            if len(pairs)
            else 0.0
        )
        r0 = X0[:, :2] / X0[:, 2:3]
        r1 = X1[:, :2] / X1[:, 2:3]
        repro = float(
            np.sqrt(np.mean(np.r_[np.sum((r0 - a) ** 2, 1), np.sum((r1 - b) ** 2, 1)]))
        )
        score = 10000 * repro + 400 * pair_error + 1000 * max(0, diameter - 0.145)
        if best is None or score < best[0]:
            Xrig = (cam0.T[:3, :3] @ X0.T).T + cam0.T[:3, 3]
            best = (score, Xrig, diameter, pair_error, repro)
    if best is None:
        raise RuntimeError("no physically plausible camera0/1 stereo assignment")
    return best


def project(points, camera):
    Tcr = np.linalg.inv(camera.T)
    pc = (Tcr[:3, :3] @ points.T).T + Tcr[:3, 3]
    rvec = np.zeros((3, 1), dtype=np.float64)
    uv, _ = cv2.fisheye.projectPoints(
        pc.reshape(-1, 1, 3), rvec, np.zeros((3, 1)), camera.K, camera.D
    )
    uv = uv.reshape(-1, 2)
    valid = (
        (pc[:, 2] > 0.02)
        & (uv[:, 0] >= 0)
        & (uv[:, 0] < ACTIVE_W)
        & (uv[:, 1] >= 0)
        & (uv[:, 1] < 508)
    )
    return uv, valid


def match(projected, valid, observed, limit):
    choices = []
    for i in np.flatnonzero(valid):
        for j, point in enumerate(observed):
            error = float(np.linalg.norm(projected[i] - point))
            if error <= limit:
                choices.append((error, int(i), j))
    used_i = set()
    used_j = set()
    out = []
    for item in sorted(choices):
        if item[1] in used_i or item[2] in used_j:
            continue
        used_i.add(item[1])
        used_j.add(item[2])
        out.append(item)
    return out


def score_camera(camera, clouds, observations, limit=10, included=None):
    errors = []
    per_pose = []
    for pose, (points, observed) in enumerate(zip(clouds, observations)):
        if included is not None and pose not in included:
            per_pose.append([])
            continue
        uv, valid = project(points, camera)
        row = match(uv, valid, observed, limit)
        per_pose.append(row)
        errors += [item[0] for item in row]
    rms = float(np.sqrt(np.mean(np.square(errors)))) if errors else float("inf")
    return len(errors), rms, per_pose


def neighbours(points, depth):
    out = []
    for anchor, point in enumerate(points):
        nearest = [
            int(i)
            for i in np.argsort(np.linalg.norm(points - point, axis=1))
            if int(i) != anchor
        ][:depth]
        out += [(anchor, first, second) for first, second in itertools.permutations(nearest, 2)]
    return out


def bootstrap_T(base, K, clouds, observations):
    source = max(range(6), key=lambda i: min(len(clouds[i]), len(observations[i])))
    points = clouds[source]
    observed = observations[source]
    rays = cv2.fisheye.undistortPoints(observed.reshape(-1, 1, 2), K, base.D).reshape(-1, 2)
    ranked = []
    for model_ids in neighbours(points, min(3, len(points) - 1)):
        for image_ids in neighbours(observed, min(3, len(observed) - 1)):
            ok, rvecs, tvecs = cv2.solveP3P(
                points[list(model_ids)].reshape(3, 1, 3),
                rays[list(image_ids)].reshape(3, 1, 2),
                np.eye(3),
                None,
                flags=cv2.SOLVEPNP_AP3P,
            )
            if not ok:
                continue
            for rvec, tvec in zip(rvecs, tvecs):
                T = np.linalg.inv(pose_matrix(rvec.reshape(3), tvec.reshape(3)))
                translation_delta = float(np.linalg.norm(T[:3, 3] - base.T[:3, 3]))
                rotation_delta = math.degrees(rot_delta(base.T, T))
                if translation_delta > 0.18 or np.linalg.norm(T[:3, 3]) > 0.35:
                    continue
                count, rms, _ = score_camera(Camera(K, base.D, T), clouds, observations, 50)
                if count < 10:
                    continue
                ranked.append(
                    (rms - 0.4 * count + 30 * translation_delta + 0.05 * rotation_delta, T)
                )
    ranked.sort(key=lambda item: item[0])
    starts = [base.T.copy()]
    for _, T in ranked:
        if any(
            np.linalg.norm(T[:3, 3] - other[:3, 3]) < 0.01
            and rot_delta(T, other) < np.deg2rad(8)
            for other in starts
        ):
            continue
        starts.append(T)
        if len(starts) >= 10:
            break
    return starts


def optimize(base, T0, K0, clouds, observations, included=None):
    x = np.r_[pose_params(T0), K0]
    lower = np.r_[
        -np.pi,
        -np.pi,
        -np.pi,
        base.T[:3, 3] - 0.12,
        [80, 80, 40, 40],
    ]
    upper = np.r_[
        np.pi,
        np.pi,
        np.pi,
        base.T[:3, 3] + 0.12,
        [300, 300, 468, 468],
    ]
    history = []
    for outer, limit in enumerate((40, 25, 15, 10)):
        camera = Camera(
            np.array([[x[6], 0, x[8]], [0, x[7], x[9]], [0, 0, 1.0]]),
            base.D,
            params_pose(x[:6]),
        )
        fixed = []
        for pose, (points, observed) in enumerate(zip(clouds, observations)):
            if included is not None and pose not in included:
                fixed.append([])
                continue
            uv, valid = project(points, camera)
            fixed.append(match(uv, valid, observed, limit))

        def residual(xx):
            K = np.array([[xx[6], 0, xx[8]], [0, xx[7], xx[9]], [0, 0, 1.0]])
            T = params_pose(xx[:6])
            candidate = Camera(K, base.D, T)
            values = []
            for pose, (points, observed) in enumerate(zip(clouds, observations)):
                if included is not None and pose not in included:
                    continue
                uv, _ = project(points, candidate)
                for _, point_index, blob_index in fixed[pose]:
                    values.extend(uv[point_index] - observed[blob_index])
            values.extend((T[:3, 3] - base.T[:3, 3]) / 0.05)
            values.append(rot_delta(base.T, T) / np.deg2rad(30))
            values.extend((xx[6:10] - K0) / np.array([60, 60, 100, 100]))
            values.append((xx[6] - xx[7]) / 45)
            return np.asarray(values)

        if sum(len(row) for row in fixed) < 6:
            break
        fit = least_squares(
            residual,
            x,
            bounds=(lower, upper),
            loss="huber",
            f_scale=4,
            max_nfev=300,
        )
        x = fit.x
        history.append(
            {
                "outer": outer + 1,
                "limit_px": limit,
                "cost": float(fit.cost),
                "nfev": fit.nfev,
            }
        )
    K = np.array([[x[6], 0, x[8]], [0, x[7], x[9]], [0, 0, 1.0]])
    return Camera(K, base.D, params_pose(x[:6])), history


def fit_upper(index, base, K_starts, clouds, observations):
    attempts = []
    for Kvec in K_starts:
        K = np.array([[Kvec[0], 0, Kvec[2]], [0, Kvec[1], Kvec[3]], [0, 0, 1.0]])
        for T0 in bootstrap_T(base, K, clouds, observations):
            camera, history = optimize(base, T0, Kvec, clouds, observations)
            count, rms, per_pose = score_camera(camera, clouds, observations, 10)
            translation_delta = float(np.linalg.norm(camera.T[:3, 3] - base.T[:3, 3]))
            rotation_delta = math.degrees(rot_delta(base.T, camera.T))
            support = sum(len(row) >= 2 for row in per_pose)
            attempts.append(
                (
                    (-support, -count, rms, 20 * translation_delta + rotation_delta / 30),
                    camera,
                    history,
                    (count, rms, translation_delta, rotation_delta, per_pose),
                    Kvec,
                )
            )
    if not attempts:
        return base, {"camera": index, "accepted": False, "reason": "no bootstrap candidates"}
    attempts.sort(key=lambda item: item[0])
    _, camera, history, quality, start_K = attempts[0]
    count, rms, translation_delta, rotation_delta, per_pose = quality
    leave_one_out = []
    for held in range(6):
        trained, _ = optimize(
            base,
            camera.T,
            np.array([camera.K[0, 0], camera.K[1, 1], camera.K[0, 2], camera.K[1, 2]]),
            clouds,
            observations,
            set(range(6)) - {held},
        )
        _, _, held_per_pose = score_camera(trained, clouds, observations, 10, {held})
        row = held_per_pose[held]
        leave_one_out.append(
            {
                "held_out": held,
                "matches": len(row),
                "rms_px": (
                    float(np.sqrt(np.mean(np.square([item[0] for item in row])))) if row else None
                ),
            }
        )
    accepted = (
        translation_delta <= 0.12
        and rotation_delta <= 60
        and count >= 18
        and rms < 8
        and all(
            row["matches"] >= 2 and row["rms_px"] is not None and row["rms_px"] < 10
            for row in leave_one_out
        )
    )
    return camera, {
        "camera": index,
        "accepted": accepted,
        "start_K": start_K.tolist(),
        "K": camera.K.tolist(),
        "T_rig_camera": camera.T.tolist(),
        "matches": count,
        "rms_px": rms,
        "translation_delta_m": translation_delta,
        "rotation_delta_deg": rotation_delta,
        "per_pose_matches": [len(row) for row in per_pose],
        "leave_one_out": leave_one_out,
        "optimization": history,
        "attempts": len(attempts),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=Path)
    parser.add_argument("capture_root", type=Path)
    parser.add_argument("--hand", choices=("left", "right"), default="left")
    parser.add_argument("--base-calibration", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    data = json.loads(args.calibration.read_text())
    cameras = load_cameras(data)
    repo_root = Path(__file__).resolve().parents[1]
    leds = load_led_positions(repo_root, args.hand)
    model_distances = np.array(
        [np.linalg.norm(leds[i] - leds[j]) for i, j in itertools.combinations(range(17), 2)]
    )
    directories = pose_dirs(args.capture_root)
    observations = [[merged_blinks(directory, camera) for camera in range(4)] for directory in directories]
    print("blob counts:", ["/".join(map(str, map(len, row))) for row in observations])

    clouds = []
    stereo = []
    for index, row in enumerate(observations):
        solution = stereo_cloud(cameras[0], cameras[1], row[0], row[1], model_distances)
        clouds.append(solution[1])
        stereo.append(
            {
                "pose": index,
                "points": solution[1].tolist(),
                "diameter_m": solution[2],
                "model_pair_error_m": solution[3],
                "normalized_rms": solution[4],
            }
        )
        print(
            f"F{index:02d}: {len(solution[1])} points, diameter {solution[2] * 1000:.1f} mm, "
            f"pair error {solution[3] * 1000:.2f} mm"
        )

    mapped = mapped_K_from_base(args.base_calibration)
    upper = {}
    for camera_index in (2, 3):
        starts = [
            np.array(
                [
                    cameras[camera_index].K[0, 0],
                    cameras[camera_index].K[1, 1],
                    cameras[camera_index].K[0, 2],
                    cameras[camera_index].K[1, 2],
                ]
            )
        ]
        if camera_index in mapped and not np.allclose(starts[0], mapped[camera_index]):
            starts.append(mapped[camera_index])
        _, diagnostics = fit_upper(
            camera_index,
            cameras[camera_index],
            starts,
            clouds,
            [row[camera_index] for row in observations],
        )
        upper[str(camera_index)] = diagnostics
        print(
            f"camera {camera_index}: accepted={diagnostics.get('accepted')} "
            f"matches={diagnostics.get('matches')} rms={diagnostics.get('rms_px')} "
            f"dT={diagnostics.get('translation_delta_m')} dR={diagnostics.get('rotation_delta_deg')}"
        )

    result = {
        "format": "psvr2-mode4-forced-prescan-joint-rig-v1",
        "accepted": all(upper[str(index)].get("accepted", False) for index in (2, 3)),
        "runtime_usable": False,
        "inputs": {
            "calibration": str(args.calibration),
            "base_calibration": None if args.base_calibration is None else str(args.base_calibration),
            "capture_root": str(args.capture_root),
            "hand": args.hand,
            "blob_counts": [[len(points) for points in row] for row in observations],
        },
        "lower_stereo": stereo,
        "upper_cameras": upper,
        "note": "Diagnostic only. Do not install as a runtime calibration without independent validation.",
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("wrote", args.output)
    return 0 if result["accepted"] else 2


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Compare native mode-4 camera model families using F* training and V* validation.

The F00..F05 forced-PRESCAN poses are the only data used for fitting.  V* poses
are reconstructed independently from cameras 0/1 and are used only for held-out
scoring.  This distinguishes three possible causes of poor spatial
generalisation in the first fixed-rig native readout fit:

  kb2_fixed   fx/fy/cx/cy + image roll + k1/k2, physical rig fixed
  kb4_fixed   as above, but full k1..k4 fisheye distortion
  kb4_tilt    full KB4 plus a small bounded optical-axis tilt, camera centre fixed
  kb4_pose    full KB4 plus small bounded tilt and camera-centre translation

There is deliberately no free camera-axis roll in the physical pose: image roll
already represents the mode-4 readout X/Y basis and would otherwise be
degenerate with a physical z rotation.

This is diagnostic only.  No result is marked runtime-usable.
"""
from __future__ import annotations

import argparse
import itertools
import json
import math
from pathlib import Path

import cv2
import numpy as np

try:
    from scipy.optimize import least_squares
except ImportError as exc:
    raise SystemExit("This diagnostic requires scipy: python3 -m pip install scipy") from exc

from psvr2_tracking_bridge_validate import pose_directories, unique_matches
from psvr2_tracking_geometry import sha256_file
from psvr2_tracking_joint_rig_refine import load_cameras, load_led_positions, merged_blinks, stereo_cloud
from psvr2_tracking_native_heldout_validate import (
    candidate_assessment,
    robust_stereo_cloud,
    validation_dirs,
)
from psvr2_tracking_native_readout_validate import fit_native, initial_parameters

ACTIVE_W = 508
ACTIVE_H = 508
FIT_LIMITS = (20.0, 15.0, 10.0, 7.0, 5.0)
SCORE_LIMITS = (5.0, 10.0, 20.0, 40.0)
FAMILIES = ("kb2_fixed", "kb4_fixed", "kb4_tilt", "kb4_pose")


def rodrigues_xy(rx: float, ry: float) -> np.ndarray:
    return cv2.Rodrigues(np.asarray([rx, ry, 0.0], dtype=np.float64))[0]


def unpack(parameters: np.ndarray, family: str):
    fx, fy, cx, cy, theta = parameters[:5]
    if family == "kb2_fixed":
        distortion = np.asarray([parameters[5], parameters[6], 0.0, 0.0], dtype=np.float64)
        tilt = np.zeros(2, dtype=np.float64)
        translation = np.zeros(3, dtype=np.float64)
    else:
        distortion = np.asarray(parameters[5:9], dtype=np.float64)
        offset = 9
        tilt = np.asarray(parameters[offset : offset + 2], dtype=np.float64) if family in ("kb4_tilt", "kb4_pose") else np.zeros(2)
        offset += 2 if family in ("kb4_tilt", "kb4_pose") else 0
        translation = np.asarray(parameters[offset : offset + 3], dtype=np.float64) if family == "kb4_pose" else np.zeros(3)
    return float(fx), float(fy), float(cx), float(cy), float(theta), distortion, tilt, translation


def effective_transform(camera, tilt: np.ndarray, translation: np.ndarray) -> np.ndarray:
    T = camera.T.copy()
    R = camera.T[:3, :3]
    T[:3, :3] = R @ rodrigues_xy(float(tilt[0]), float(tilt[1]))
    T[:3, 3] = camera.T[:3, 3] + R @ translation
    return T


def project_model(points_rig: np.ndarray, camera, parameters: np.ndarray, family: str):
    fx, fy, cx, cy, theta, distortion, tilt, translation = unpack(parameters, family)
    c = math.cos(theta)
    s = math.sin(theta)
    image_linear = np.diag([fx, fy]) @ np.asarray([[c, -s], [s, c]], dtype=np.float64)

    T = effective_transform(camera, tilt, translation)
    T_camera_rig = np.linalg.inv(T)
    points_camera = (T_camera_rig[:3, :3] @ points_rig.T).T + T_camera_rig[:3, 3]
    normalized, _ = cv2.fisheye.projectPoints(
        points_camera.reshape(-1, 1, 3),
        np.zeros((3, 1), dtype=np.float64),
        np.zeros((3, 1), dtype=np.float64),
        np.eye(3, dtype=np.float64),
        distortion,
    )
    normalized = normalized.reshape(-1, 2)
    projected = (image_linear @ normalized.T).T + np.asarray([cx, cy], dtype=np.float64)
    valid = (
        (points_camera[:, 2] > 0.02)
        & (projected[:, 0] >= 0.0)
        & (projected[:, 0] < ACTIVE_W)
        & (projected[:, 1] >= 0.0)
        & (projected[:, 1] < ACTIVE_H)
    )
    return projected, valid


def model_bounds(family: str):
    base_lo = [60.0, 60.0, -50.0, -50.0, math.radians(-30.0)]
    base_hi = [300.0, 300.0, 558.0, 558.0, math.radians(30.0)]
    if family == "kb2_fixed":
        return np.asarray(base_lo + [-1.2, -1.2]), np.asarray(base_hi + [1.2, 1.2])
    lo = base_lo + [-1.2] * 4
    hi = base_hi + [1.2] * 4
    if family in ("kb4_tilt", "kb4_pose"):
        lo += [math.radians(-12.0), math.radians(-12.0)]
        hi += [math.radians(12.0), math.radians(12.0)]
    if family == "kb4_pose":
        lo += [-0.02, -0.02, -0.02]
        hi += [0.02, 0.02, 0.02]
    return np.asarray(lo, dtype=np.float64), np.asarray(hi, dtype=np.float64)


def extend_parameters(kb2: np.ndarray, family: str) -> np.ndarray:
    if family == "kb2_fixed":
        return np.asarray(kb2, dtype=np.float64).copy()
    out = list(np.asarray(kb2, dtype=np.float64)[:7])
    # Convert [fx,fy,cx,cy,theta,k1,k2] to full KB4 ordering.
    out = out[:5] + [out[5], out[6], 0.0, 0.0]
    if family in ("kb4_tilt", "kb4_pose"):
        out += [0.0, 0.0]
    if family == "kb4_pose":
        out += [0.0, 0.0, 0.0]
    return np.asarray(out, dtype=np.float64)


def score_model(camera, parameters, family, clouds, observations, limit_px: float) -> dict:
    errors = []
    per_pose = []
    for points, observed in zip(clouds, observations):
        projected, valid = project_model(points, camera, parameters, family)
        matches = unique_matches(projected, valid, observed, limit_px)
        row_errors = [float(item[0]) for item in matches]
        errors.extend(row_errors)
        per_pose.append(
            {
                "matches": len(matches),
                "rms_px": None if not row_errors else float(np.sqrt(np.mean(np.square(row_errors)))),
                "median_px": None if not row_errors else float(np.median(row_errors)),
                "errors_px": row_errors,
            }
        )
    return {
        "matches": len(errors),
        "rms_px": None if not errors else float(np.sqrt(np.mean(np.square(errors)))),
        "median_px": None if not errors else float(np.median(errors)),
        "p95_px": None if not errors else float(np.percentile(errors, 95)),
        "per_pose": per_pose,
    }


def fit_family(camera, start: np.ndarray, family: str, clouds, observations):
    parameters = start.copy()
    lower, upper = model_bounds(family)
    parameters = np.minimum(np.maximum(parameters, lower + 1e-10), upper - 1e-10)
    prior = parameters.copy()
    history = []

    for limit_px in FIT_LIMITS:
        fixed = []
        total = 0
        for points, observed in zip(clouds, observations):
            projected, valid = project_model(points, camera, parameters, family)
            matches = unique_matches(projected, valid, observed, limit_px)
            fixed.append(matches)
            total += len(matches)
        if total < 8:
            history.append({"limit_px": limit_px, "matches": total, "rms_px": None})
            break

        def residuals(candidate):
            values = []
            for points, observed, matches in zip(clouds, observations, fixed):
                projected, _ = project_model(points, camera, candidate, family)
                for _, point_index, blob_index in matches:
                    values.extend(projected[point_index] - observed[blob_index])

            # Weak regularisation: enough to expose whether a family genuinely
            # improves held-out geometry without letting unlabeled assignments
            # wander into an implausible optical model.
            values.extend((candidate[:2] - prior[:2]) / 60.0)
            values.extend((candidate[2:4] - prior[2:4]) / 100.0)
            values.append((candidate[4] - prior[4]) / math.radians(12.0))
            if family == "kb2_fixed":
                values.extend((candidate[5:7] - prior[5:7]) / 0.6)
            else:
                values.extend((candidate[5:9] - prior[5:9]) / 0.6)
                offset = 9
                if family in ("kb4_tilt", "kb4_pose"):
                    values.extend(candidate[offset : offset + 2] / math.radians(5.0))
                    offset += 2
                if family == "kb4_pose":
                    values.extend(candidate[offset : offset + 3] / 0.010)
            return np.asarray(values, dtype=np.float64)

        result = least_squares(
            residuals,
            parameters,
            bounds=(lower, upper),
            loss="huber",
            f_scale=3.0,
            max_nfev=800,
        )
        parameters = result.x
        score = score_model(camera, parameters, family, clouds, observations, limit_px)
        history.append(
            {
                "limit_px": limit_px,
                "matches": score["matches"],
                "rms_px": score["rms_px"],
                "cost": float(result.cost),
            }
        )
    return parameters, history


def parameters_json(parameters: np.ndarray, family: str) -> dict:
    fx, fy, cx, cy, theta, distortion, tilt, translation = unpack(parameters, family)
    return {
        "fx": fx,
        "fy": fy,
        "cx": cx,
        "cy": cy,
        "image_rotation_deg": math.degrees(theta),
        "distortion": dict(zip(("k1", "k2", "k3", "k4"), distortion.tolist())),
        "optical_axis_tilt_deg": [math.degrees(float(tilt[0])), math.degrees(float(tilt[1]))],
        "camera_local_translation_mm": (1000.0 * translation).tolist(),
    }


def training_data(root: Path, cameras, model_distances):
    directories = pose_directories(root)
    observations = [[merged_blinks(directory, camera) for camera in range(4)] for directory in directories]
    clouds = []
    anchors = []
    for index, row in enumerate(observations):
        solution = stereo_cloud(cameras[0], cameras[1], row[0], row[1], model_distances)
        clouds.append(solution[1])
        anchors.append(
            {
                "pose": f"F{index:02d}",
                "point_count": int(len(solution[1])),
                "diameter_m": float(solution[2]),
                "model_pair_error_m": float(solution[3]),
                "normalized_ray_rms": float(solution[4]),
            }
        )
    return clouds, observations, anchors


def heldout_data(root: Path, cameras, model_distances):
    names, clouds, rows, anchors, skipped = [], [], [], [], []
    for directory in validation_dirs(root):
        row = [merged_blinks(directory, camera) for camera in range(4)]
        try:
            solution, selection = robust_stereo_cloud(cameras[0], cameras[1], row[0], row[1], model_distances)
        except RuntimeError as exc:
            skipped.append({"pose": directory.name, "reason": str(exc), "blob_counts": [len(x) for x in row]})
            continue
        names.append(directory.name)
        clouds.append(solution[1])
        rows.append(row)
        anchors.append(
            {
                "pose": directory.name,
                "point_count": int(len(solution[1])),
                "diameter_m": float(solution[2]),
                "model_pair_error_m": float(solution[3]),
                "normalized_ray_rms": float(solution[4]),
                "stereo_subset_selection": selection,
            }
        )
    return names, clouds, rows, anchors, skipped


def named_score(score: dict, names: list[str]) -> dict:
    out = dict(score)
    out["per_pose"] = [{"pose": name, **row} for name, row in zip(names, score["per_pose"])]
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=Path)
    parser.add_argument("training_root", type=Path)
    parser.add_argument("validation_root", type=Path)
    parser.add_argument("--hand", choices=("left", "right"), default="left")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    calibration_data = json.loads(args.calibration.read_text())
    cameras = load_cameras(calibration_data)
    positions = load_led_positions(Path(__file__).resolve().parents[1], args.hand)
    model_distances = np.asarray(
        [np.linalg.norm(positions[i] - positions[j]) for i, j in itertools.combinations(range(len(positions)), 2)]
    )

    train_clouds, train_rows, train_anchors = training_data(args.training_root, cameras, model_distances)
    val_names, val_clouds, val_rows, val_anchors, skipped = heldout_data(args.validation_root, cameras, model_distances)
    if len(val_clouds) < 4:
        raise SystemExit(f"only {len(val_clouds)} usable held-out poses; expected at least 4")

    output_cameras = {}
    for camera_index in (2, 3):
        camera = cameras[camera_index]
        train_obs = [row[camera_index] for row in train_rows]
        val_obs = [row[camera_index] for row in val_rows]

        kb2, kb2_history = fit_native(camera, train_clouds, train_obs)
        starts = {family: extend_parameters(kb2, family) for family in FAMILIES}
        fits = {"kb2_fixed": (kb2, kb2_history)}
        for family in FAMILIES[1:]:
            fits[family] = fit_family(camera, starts[family], family, train_clouds, train_obs)

        family_results = {}
        possible = [min(len(cloud), len(observed)) for cloud, observed in zip(val_clouds, val_obs)]
        for family in FAMILIES:
            parameters, history = fits[family]
            training_scores = {
                str(int(limit)): score_model(camera, parameters, family, train_clouds, train_obs, limit)
                for limit in SCORE_LIMITS
            }
            heldout_scores = {
                str(int(limit)): named_score(
                    score_model(camera, parameters, family, val_clouds, val_obs, limit), val_names
                )
                for limit in SCORE_LIMITS
            }
            assessment = candidate_assessment(heldout_scores, val_anchors, possible)
            family_results[family] = {
                "parameters": parameters_json(parameters, family),
                "optimization": history,
                "training": training_scores,
                "heldout": heldout_scores,
                "assessment": assessment,
            }
            train10 = training_scores["10"]
            held10 = heldout_scores["10"]
            print(
                f"camera {camera_index} {family}: training={train10['matches']}/{train10['rms_px']} "
                f"heldout={held10['matches']}/{held10['rms_px']} pass={assessment['passed']}"
            )
            params = family_results[family]["parameters"]
            if family in ("kb4_tilt", "kb4_pose"):
                print(
                    f"  tilt={params['optical_axis_tilt_deg']} deg "
                    f"translation={params['camera_local_translation_mm']} mm"
                )

        output_cameras[str(camera_index)] = {"camera": camera_index, "families": family_results}

    result = {
        "format": "psvr2-tracking-native-model-comparison-v1",
        "runtime_usable": False,
        "calibration": {"path": str(args.calibration), "sha256": sha256_file(args.calibration)},
        "training_root": str(args.training_root),
        "validation_root": str(args.validation_root),
        "hand": args.hand,
        "training_anchors": train_anchors,
        "heldout_anchors": val_anchors,
        "skipped_heldout": skipped,
        "cameras": output_cameras,
        "note": (
            "F* poses only are used for fitting. V* poses are reconstructed from unchanged cameras 0/1 and are used "
            "only for held-out scoring. kb4_tilt changes optical-axis direction but not camera centre; kb4_pose permits "
            "at most 20 mm camera-local translation and 12 degree x/y tilt. None of these diagnostic models is runtime usable."
        ),
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

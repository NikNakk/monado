#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Fit a diagnostic native mode-4 readout while keeping PSVR2 rig extrinsics fixed.

The visible->tracking affine bridge is experimentally constrained only in the
visible/tracking overlap.  For the upper cameras, Sense LEDs in the wider
tracking readout can lie well outside the angular range used to fit the visible
fisheye distortion polynomial.  Extrapolating that polynomial can therefore
look like a large camera-extrinsic error.

This script uses cameras 0/1 as a metric stereo anchor, triangulates the forced-
PRESCAN blobs into rig space, and fits cameras 2/3 with their reviewed physical
T_rig_camera held exactly fixed.  Only a diagnostic native readout model is fit:

    p = diag(fx, fy) R(theta) fisheye(q; k1, k2) + (cx, cy)

where q is the ray in the fixed physical camera frame.  The model deliberately
starts with zero tracking distortion rather than extrapolating the visible
fisheye D outside its calibrated field.  Leave-one-pose-out results test whether
one readout model generalises across the six forced-PRESCAN poses.

No output from this script is runtime-usable calibration.
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
    raise SystemExit("This offline diagnostic requires scipy: python3 -m pip install scipy") from exc

from psvr2_tracking_bridge_validate import (
    best_zero_skew_rotation,
    pose_directories,
    score_camera,
    stereo_cloud,
    unique_matches,
)
from psvr2_tracking_geometry import load_camera_models, load_led_model
from psvr2_tracking_joint_rig_refine import merged_blinks

ACTIVE_W = 508
ACTIVE_H = 508
FIT_LIMITS = (60.0, 45.0, 35.0, 25.0, 15.0, 10.0, 7.0, 5.0)


def initial_parameters(camera) -> np.ndarray:
    M = np.asarray(camera.H_mode3_to_mode4 @ camera.K, dtype=np.float64)
    B = M[:2, :2]
    theta = best_zero_skew_rotation(B)
    c = math.cos(theta)
    s = math.sin(theta)
    R = np.asarray([[c, -s], [s, c]], dtype=np.float64)
    C = B @ R.T
    return np.asarray([C[0, 0], C[1, 1], M[0, 2], M[1, 2], theta, 0.0, 0.0], dtype=np.float64)


def project_native(points_rig: np.ndarray, camera, parameters: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    fx, fy, cx, cy, theta, k1, k2 = parameters
    c = math.cos(theta)
    s = math.sin(theta)
    R = np.asarray([[c, -s], [s, c]], dtype=np.float64)
    A = np.diag([fx, fy]) @ R

    T_camera_rig = np.linalg.inv(camera.T_rig_camera)
    points_camera = (T_camera_rig[:3, :3] @ points_rig.T).T + T_camera_rig[:3, 3]
    distortion = np.asarray([k1, k2, 0.0, 0.0], dtype=np.float64)
    normalized, _ = cv2.fisheye.projectPoints(
        points_camera.reshape(-1, 1, 3),
        np.zeros((3, 1), dtype=np.float64),
        np.zeros((3, 1), dtype=np.float64),
        np.eye(3, dtype=np.float64),
        distortion,
    )
    normalized = normalized.reshape(-1, 2)
    projected = (A @ normalized.T).T + np.asarray([cx, cy], dtype=np.float64)
    valid = (
        (points_camera[:, 2] > 0.02)
        & (projected[:, 0] >= 0.0)
        & (projected[:, 0] < ACTIVE_W)
        & (projected[:, 1] >= 0.0)
        & (projected[:, 1] < ACTIVE_H)
    )
    return projected, valid


def score_native(camera, parameters, clouds, observations, limit_px: float, included=None) -> dict:
    errors = []
    per_pose = []
    for pose, (points, observed) in enumerate(zip(clouds, observations)):
        if included is not None and pose not in included:
            per_pose.append(0)
            continue
        projected, valid = project_native(points, camera, parameters)
        matches = unique_matches(projected, valid, observed, limit_px)
        per_pose.append(len(matches))
        errors.extend(match[0] for match in matches)
    return {
        "matches": len(errors),
        "per_pose_matches": per_pose,
        "rms_px": None if not errors else float(np.sqrt(np.mean(np.square(errors)))),
        "median_px": None if not errors else float(np.median(errors)),
        "p95_px": None if not errors else float(np.percentile(errors, 95)),
    }


def fit_native(camera, clouds, observations, included=None):
    parameters = initial_parameters(camera)
    initial = parameters.copy()
    lower = np.asarray([60.0, 60.0, -50.0, -50.0, math.radians(-30.0), -0.8, -0.8])
    upper = np.asarray([300.0, 300.0, 558.0, 558.0, math.radians(30.0), 0.8, 0.8])
    history = []

    for limit_px in FIT_LIMITS:
        fixed = []
        total = 0
        for pose, (points, observed) in enumerate(zip(clouds, observations)):
            if included is not None and pose not in included:
                fixed.append([])
                continue
            projected, valid = project_native(points, camera, parameters)
            matches = unique_matches(projected, valid, observed, limit_px)
            fixed.append(matches)
            total += len(matches)
        if total < 8:
            history.append({"limit_px": limit_px, "matches": total, "rms_px": None})
            break

        def residuals(candidate):
            values = []
            for pose, (points, observed, matches) in enumerate(zip(clouds, observations, fixed)):
                if included is not None and pose not in included:
                    continue
                projected, _ = project_native(points, camera, candidate)
                for _, point_index, blob_index in matches:
                    values.extend(projected[point_index] - observed[blob_index])

            # Weak priors prevent an unlabeled point-set fit from drifting into
            # a mathematically valid but clearly non-camera-like solution.
            values.extend((candidate[:2] - initial[:2]) / 50.0)
            values.extend((candidate[2:4] - initial[2:4]) / 80.0)
            values.append((candidate[4] - initial[4]) / math.radians(10.0))
            values.extend(candidate[5:7] / 0.4)
            return np.asarray(values, dtype=np.float64)

        result = least_squares(
            residuals,
            parameters,
            bounds=(lower, upper),
            loss="huber",
            f_scale=3.0,
            max_nfev=500,
        )
        parameters = result.x
        score = score_native(camera, parameters, clouds, observations, limit_px, included)
        history.append(
            {
                "limit_px": limit_px,
                "matches": score["matches"],
                "rms_px": score["rms_px"],
                "cost": float(result.cost),
            }
        )

    return parameters, history


def angular_extrapolation(camera, clouds) -> dict:
    angles = []
    visible_theta_d = []
    for points in clouds:
        T_camera_rig = np.linalg.inv(camera.T_rig_camera)
        pc = (T_camera_rig[:3, :3] @ points.T).T + T_camera_rig[:3, 3]
        theta = np.arctan2(np.linalg.norm(pc[:, :2], axis=1), pc[:, 2])
        theta2 = theta * theta
        k1, k2, k3, k4 = camera.D
        theta_d = theta * (
            1.0 + k1 * theta2 + k2 * theta2**2 + k3 * theta2**3 + k4 * theta2**4
        )
        angles.extend(np.degrees(theta).tolist())
        visible_theta_d.extend(theta_d.tolist())
    return {
        "ray_angle_deg": {
            "min": float(np.min(angles)),
            "median": float(np.median(angles)),
            "max": float(np.max(angles)),
        },
        "visible_distortion_theta_d": {
            "min": float(np.min(visible_theta_d)),
            "median": float(np.median(visible_theta_d)),
            "max": float(np.max(visible_theta_d)),
        },
        "note": "theta_d is the visible-mode OpenCV fisheye radial angle evaluated on these tracking rays; large values indicate extrapolation outside the visible calibration domain.",
    }


def parameters_json(parameters: np.ndarray) -> dict:
    return {
        "fx": float(parameters[0]),
        "fy": float(parameters[1]),
        "cx": float(parameters[2]),
        "cy": float(parameters[3]),
        "image_rotation_deg": math.degrees(float(parameters[4])),
        "distortion": {"k1": float(parameters[5]), "k2": float(parameters[6]), "k3": 0.0, "k4": 0.0},
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=Path)
    parser.add_argument("capture_root", type=Path)
    parser.add_argument("--hand", choices=("left", "right"), default="left")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    calibration = json.loads(args.calibration.read_text())
    cameras = load_camera_models(calibration)
    positions, _ = load_led_model(Path(__file__).resolve().parents[1], args.hand)
    model_distances = np.asarray(
        [np.linalg.norm(positions[i] - positions[j]) for i, j in itertools.combinations(range(len(positions)), 2)]
    )

    directories = pose_directories(args.capture_root)
    observations = [[merged_blinks(directory, camera) for camera in range(4)] for directory in directories]
    clouds = []
    stereo = []
    for pose, row in enumerate(observations):
        solution = stereo_cloud(cameras[0], cameras[1], row[0], row[1], model_distances)
        clouds.append(solution[1])
        stereo.append(
            {
                "pose": pose,
                "point_count": len(solution[1]),
                "diameter_m": solution[2],
                "model_pair_error_m": solution[3],
                "normalized_ray_rms": solution[4],
            }
        )

    camera_results = {}
    for camera_index in (2, 3):
        camera = cameras[camera_index]
        camera_observations = [row[camera_index] for row in observations]
        parameters, history = fit_native(camera, clouds, camera_observations)
        fitted_scores = {
            str(int(limit)): score_native(camera, parameters, clouds, camera_observations, limit)
            for limit in (5.0, 10.0, 20.0, 40.0)
        }

        leave_one_out = []
        for held_out in range(len(clouds)):
            included = set(range(len(clouds))) - {held_out}
            held_parameters, _ = fit_native(camera, clouds, camera_observations, included=included)
            leave_one_out.append(
                {
                    "held_out": held_out,
                    "parameters": parameters_json(held_parameters),
                    "score_5px": score_native(
                        camera, held_parameters, clouds, camera_observations, 5.0, included={held_out}
                    ),
                    "score_10px": score_native(
                        camera, held_parameters, clouds, camera_observations, 10.0, included={held_out}
                    ),
                }
            )

        full_bridge = score_camera(camera, clouds, camera_observations)
        camera_results[str(camera_index)] = {
            "camera": camera_index,
            "physical_extrinsics_fixed": True,
            "T_rig_camera": camera.T_rig_camera.tolist(),
            "angular_extrapolation": angular_extrapolation(camera, clouds),
            "full_visible_bridge": full_bridge,
            "fitted_native_readout": {
                "parameters": parameters_json(parameters),
                "scores": fitted_scores,
                "optimization": history,
                "leave_one_pose_out": leave_one_out,
            },
        }

        score10 = fitted_scores["10"]
        angle = camera_results[str(camera_index)]["angular_extrapolation"]["ray_angle_deg"]
        theta_d = camera_results[str(camera_index)]["angular_extrapolation"]["visible_distortion_theta_d"]
        print(
            f"camera {camera_index}: fixed-extrinsics native fit {score10['matches']} matches, "
            f"rms={score10['rms_px']:.3f}px; ray angle {angle['min']:.1f}..{angle['max']:.1f}deg; "
            f"visible theta_d median/max={theta_d['median']:.2f}/{theta_d['max']:.2f}"
        )

    result = {
        "format": "psvr2-tracking-native-readout-validation-v1",
        "runtime_usable": False,
        "calibration": str(args.calibration),
        "capture_root": str(args.capture_root),
        "hand": args.hand,
        "stereo_anchor": stereo,
        "cameras": camera_results,
        "note": (
            "Diagnostic only. Cameras 2/3 keep the reviewed physical rig extrinsics exactly fixed. A good fitted/held-out "
            "native readout result therefore argues that visible-mode intrinsics/distortion extrapolation, rather than a "
            "large physical camera-pose correction, explains the upper-camera mode-4 mismatch. The fitted model is not "
            "yet a production calibration because the 3D points are unlabeled and are concentrated in the Sense capture region."
        ),
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("wrote", args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

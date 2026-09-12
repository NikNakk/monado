#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Assignment-based multi-pose PSVR2 mode-4 upper-camera rig refinement.

This consumes the same forced-PRESCAN capture set as
psvr2_tracking_joint_rig_refine.py. Cameras 0/1 are used only to triangulate a
metric 3D point cloud for each controller pose. Cameras 2/3 are then fitted by
minimizing a one-to-one assignment cost between those 3D points and the upper
camera PRESCAN points across all six poses.

The upper-camera translation is kept close to the physical rig while rotation
and native mode-4 K may move substantially. Fisheye D remains fixed. Output is
strictly diagnostic and is never marked runtime-usable.
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
    from scipy.optimize import differential_evolution, least_squares, linear_sum_assignment
except ImportError as exc:
    raise SystemExit("This offline solver requires scipy: python3 -m pip install scipy") from exc

from psvr2_tracking_joint_rig_refine import (
    Camera,
    load_cameras,
    load_led_positions,
    mapped_K_from_base,
    merged_blinks,
    pose_dirs,
    rot_delta,
    stereo_cloud,
)

ACTIVE_W = 508
HEIGHT = 508
ROTATION_DELTA_BOUND_RAD = 1.4
TRANSLATION_DELTA_BOUND_M = 0.06
FX_FY_BOUNDS = (80.0, 300.0)
CX_CY_BOUNDS = (40.0, 468.0)


def rotation_vector(matrix: np.ndarray) -> np.ndarray:
    return cv2.Rodrigues(np.ascontiguousarray(matrix, dtype=np.float64))[0].reshape(3)


def relative_parameters(base: Camera, camera: Camera) -> np.ndarray:
    delta_rotation = base.T[:3, :3].T @ camera.T[:3, :3]
    return np.r_[
        rotation_vector(delta_rotation),
        camera.T[:3, 3] - base.T[:3, 3],
        camera.K[0, 0],
        camera.K[1, 1],
        camera.K[0, 2],
        camera.K[1, 2],
    ]


def unpack(base: Camera, parameters: np.ndarray) -> Camera:
    delta_rotation = cv2.Rodrigues(np.asarray(parameters[:3], dtype=np.float64).reshape(3, 1))[0]
    transform = np.eye(4)
    transform[:3, :3] = base.T[:3, :3] @ delta_rotation
    transform[:3, 3] = base.T[:3, 3] + parameters[3:6]
    K = np.asarray(
        [
            [parameters[6], 0.0, parameters[8]],
            [0.0, parameters[7], parameters[9]],
            [0.0, 0.0, 1.0],
        ],
        dtype=np.float64,
    )
    return Camera(K, base.D, transform)


def project(points: np.ndarray, camera: Camera) -> tuple[np.ndarray, np.ndarray]:
    T_camera_rig = np.linalg.inv(camera.T)
    camera_points = (T_camera_rig[:3, :3] @ points.T).T + T_camera_rig[:3, 3]
    pixels, _ = cv2.fisheye.projectPoints(
        camera_points.reshape(-1, 1, 3),
        np.zeros((3, 1), dtype=np.float64),
        np.zeros((3, 1), dtype=np.float64),
        camera.K,
        camera.D,
    )
    return pixels.reshape(-1, 2), camera_points


def assignment(points: np.ndarray, observed: np.ndarray, camera: Camera) -> dict:
    projected, camera_points = project(points, camera)
    distances = np.linalg.norm(projected[:, None, :] - observed[None, :, :], axis=2)
    rows, cols = linear_sum_assignment(distances)
    errors = distances[rows, cols]
    return {
        "projected": projected,
        "camera_points": camera_points,
        "point_indices": rows,
        "blob_indices": cols,
        "errors": errors,
    }


def robust_distance_cost(errors: np.ndarray, transition_px: float = 20.0) -> float:
    absolute = np.asarray(errors, dtype=np.float64)
    return float(
        np.sum(
            np.where(
                absolute <= transition_px,
                absolute * absolute,
                2.0 * transition_px * absolute - transition_px * transition_px,
            )
        )
    )


def search_objective(
    parameters: np.ndarray,
    base: Camera,
    k_reference: np.ndarray,
    clouds: list[np.ndarray],
    observations: list[np.ndarray],
    included: set[int],
) -> float:
    camera = unpack(base, parameters)
    value = 0.0
    for pose in sorted(included):
        result = assignment(clouds[pose], observations[pose], camera)
        if len(result["errors"]) != len(clouds[pose]):
            value += 5000.0 * (len(clouds[pose]) - len(result["errors"]))
        behind = int(np.count_nonzero(result["camera_points"][:, 2] <= 0.02))
        value += 5000.0 * behind
        value += robust_distance_cost(result["errors"])
        projected = result["projected"]
        outside = (
            np.maximum(0.0, -projected[:, 0])
            + np.maximum(0.0, projected[:, 0] - ACTIVE_W)
            + np.maximum(0.0, -projected[:, 1])
            + np.maximum(0.0, projected[:, 1] - HEIGHT)
        )
        value += 5.0 * float(np.sum(outside))

    # Keep the camera centre close to the measured physical rig. The provisional
    # upper-camera orientation/K are not trusted enough to receive tight priors.
    value += 10.0 * float(np.sum(np.square(parameters[3:6] / 0.03)))
    value += 2.0 * float(np.sum(np.square(parameters[:3] / 0.60)))
    value += float(
        np.sum(
            np.square(
                (parameters[6:10] - k_reference)
                / np.asarray([60.0, 60.0, 120.0, 120.0], dtype=np.float64)
            )
        )
    )
    return value


def bounds() -> list[tuple[float, float]]:
    return (
        [(-ROTATION_DELTA_BOUND_RAD, ROTATION_DELTA_BOUND_RAD)] * 3
        + [(-TRANSLATION_DELTA_BOUND_M, TRANSLATION_DELTA_BOUND_M)] * 3
        + [FX_FY_BOUNDS, FX_FY_BOUNDS, CX_CY_BOUNDS, CX_CY_BOUNDS]
    )


def fixed_correspondences(camera, clouds, observations, included):
    result = {}
    for pose in sorted(included):
        current = assignment(clouds[pose], observations[pose], camera)
        result[pose] = (
            current["point_indices"].copy(),
            current["blob_indices"].copy(),
        )
    return result


def polish(
    parameters,
    base,
    k_reference,
    clouds,
    observations,
    included,
    correspondences,
    reference_parameters=None,
):
    lower = np.asarray([entry[0] for entry in bounds()], dtype=np.float64)
    upper = np.asarray([entry[1] for entry in bounds()], dtype=np.float64)

    def residual(current):
        camera = unpack(base, current)
        values = []
        for pose in sorted(included):
            projected, _ = project(clouds[pose], camera)
            point_indices, blob_indices = correspondences[pose]
            values.extend(
                (projected[point_indices] - observations[pose][blob_indices]).reshape(-1)
            )
        if reference_parameters is None:
            values.extend(2.0 * current[3:6] / 0.03)
            values.extend(0.5 * current[:3] / 0.80)
            values.extend(
                (current[6:10] - k_reference)
                / np.asarray([100.0, 100.0, 180.0, 180.0], dtype=np.float64)
            )
        else:
            values.extend((current[:3] - reference_parameters[:3]) / 0.25)
            values.extend((current[3:6] - reference_parameters[3:6]) / 0.02)
            values.extend(
                (current[6:10] - reference_parameters[6:10])
                / np.asarray([30.0, 30.0, 50.0, 50.0], dtype=np.float64)
            )
        return np.asarray(values, dtype=np.float64)

    return least_squares(
        residual,
        np.minimum(np.maximum(parameters, lower + 1e-10), upper - 1e-10),
        bounds=(lower, upper),
        loss="huber",
        f_scale=3.0,
        max_nfev=400,
    ).x


def score(camera, clouds, observations) -> dict:
    per_pose = []
    all_errors = []
    for pose, (points, observed) in enumerate(zip(clouds, observations)):
        result = assignment(points, observed, camera)
        errors = result["errors"]
        all_errors.extend(errors.tolist())
        per_pose.append(
            {
                "pose": pose,
                "matches": int(len(errors)),
                "rms_px": float(np.sqrt(np.mean(np.square(errors)))) if len(errors) else None,
                "max_px": float(np.max(errors)) if len(errors) else None,
                "pairs": [
                    {
                        "point_index": int(point_index),
                        "blob_index": int(blob_index),
                        "error_px": float(error),
                    }
                    for point_index, blob_index, error in zip(
                        result["point_indices"], result["blob_indices"], errors
                    )
                ],
            }
        )
    return {
        "matches": len(all_errors),
        "rms_px": float(np.sqrt(np.mean(np.square(all_errors)))) if all_errors else None,
        "max_px": float(max(all_errors)) if all_errors else None,
        "per_pose": per_pose,
    }


def fit_camera(index, base, k_reference, clouds, observations):
    included = set(range(len(clouds)))
    result = differential_evolution(
        lambda parameters: search_objective(
            parameters, base, k_reference, clouds, observations, included
        ),
        bounds(),
        seed=100 + index,
        popsize=10,
        maxiter=140,
        tol=0.005,
        polish=False,
        workers=1,
        updating="immediate",
    )
    parameters = result.x

    # Alternate one-to-one assignment and a smooth least-squares solve twice.
    for _ in range(2):
        camera = unpack(base, parameters)
        correspondences = fixed_correspondences(camera, clouds, observations, included)
        parameters = polish(
            parameters,
            base,
            k_reference,
            clouds,
            observations,
            included,
            correspondences,
        )

    camera = unpack(base, parameters)
    full = score(camera, clouds, observations)

    leave_one_out = []
    for held_out in range(len(clouds)):
        training = included - {held_out}
        training_correspondences = fixed_correspondences(
            camera, clouds, observations, training
        )
        trained_parameters = polish(
            parameters,
            base,
            k_reference,
            clouds,
            observations,
            training,
            training_correspondences,
            reference_parameters=parameters,
        )
        held_camera = unpack(base, trained_parameters)
        held = assignment(clouds[held_out], observations[held_out], held_camera)
        leave_one_out.append(
            {
                "held_out": held_out,
                "matches": int(len(held["errors"])),
                "rms_px": float(np.sqrt(np.mean(np.square(held["errors"]))))
                if len(held["errors"])
                else None,
                "max_px": float(np.max(held["errors"])) if len(held["errors"]) else None,
            }
        )

    translation_delta = float(np.linalg.norm(camera.T[:3, 3] - base.T[:3, 3]))
    rotation_delta_deg = math.degrees(rot_delta(base.T, camera.T))
    k_delta = np.asarray(
        [
            camera.K[0, 0] - base.K[0, 0],
            camera.K[1, 1] - base.K[1, 1],
            camera.K[0, 2] - base.K[0, 2],
            camera.K[1, 2] - base.K[1, 2],
        ]
    )
    all_points_matched = all(
        row["matches"] == len(clouds[row["pose"]]) for row in full["per_pose"]
    )
    held_out_ok = all(
        row["matches"] == len(clouds[row["held_out"]])
        and row["rms_px"] is not None
        and row["rms_px"] < 8.0
        for row in leave_one_out
    )
    diagnostic_pass = (
        all_points_matched
        and full["rms_px"] is not None
        and full["rms_px"] < 3.0
        and translation_delta < 0.04
        and rotation_delta_deg < 85.0
        and held_out_ok
    )
    return {
        "camera": index,
        "diagnostic_pass": diagnostic_pass,
        "search_cost": float(result.fun),
        "search_nit": int(result.nit),
        "K_reference": k_reference.tolist(),
        "K": camera.K.tolist(),
        "K_delta_from_provisional": k_delta.tolist(),
        "T_rig_camera": camera.T.tolist(),
        "translation_delta_m": translation_delta,
        "rotation_delta_deg": rotation_delta_deg,
        "full_fit": full,
        "leave_one_out": leave_one_out,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=Path)
    parser.add_argument("capture_root", type=Path)
    parser.add_argument("--hand", choices=("left", "right"), default="left")
    parser.add_argument("--base-calibration", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    calibration = json.loads(args.calibration.read_text())
    cameras = load_cameras(calibration)
    repo_root = Path(__file__).resolve().parents[1]
    leds = load_led_positions(repo_root, args.hand)
    model_distances = np.asarray(
        [np.linalg.norm(leds[i] - leds[j]) for i, j in itertools.combinations(range(17), 2)],
        dtype=np.float64,
    )
    directories = pose_dirs(args.capture_root)
    observations = [
        [merged_blinks(directory, camera) for camera in range(4)]
        for directory in directories
    ]
    print("blob counts:", ["/".join(map(str, map(len, row))) for row in observations])

    clouds = []
    stereo = []
    for pose, row in enumerate(observations):
        current = stereo_cloud(cameras[0], cameras[1], row[0], row[1], model_distances)
        clouds.append(current[1])
        stereo.append(
            {
                "pose": pose,
                "points": current[1].tolist(),
                "diameter_m": current[2],
                "model_pair_error_m": current[3],
                "normalized_rms": current[4],
            }
        )
        print(
            f"F{pose:02d}: {len(current[1])} points, diameter {current[2] * 1000:.1f} mm, "
            f"pair error {current[3] * 1000:.2f} mm"
        )

    mapped = mapped_K_from_base(args.base_calibration)
    upper = {}
    for index in (2, 3):
        provisional_reference = np.asarray(
            [
                cameras[index].K[0, 0],
                cameras[index].K[1, 1],
                cameras[index].K[0, 2],
                cameras[index].K[1, 2],
            ],
            dtype=np.float64,
        )
        k_reference = mapped.get(index, provisional_reference)
        diagnostics = fit_camera(
            index,
            cameras[index],
            k_reference,
            clouds,
            [row[index] for row in observations],
        )
        upper[str(index)] = diagnostics
        print(
            f"camera {index}: pass={diagnostics['diagnostic_pass']} "
            f"rms={diagnostics['full_fit']['rms_px']:.3f}px "
            f"dT={diagnostics['translation_delta_m'] * 1000:.1f}mm "
            f"dR={diagnostics['rotation_delta_deg']:.1f}deg "
            f"K=({diagnostics['K'][0][0]:.1f},{diagnostics['K'][1][1]:.1f},"
            f"{diagnostics['K'][0][2]:.1f},{diagnostics['K'][1][2]:.1f})"
        )
        print(
            "  held-out RMS:",
            ", ".join(
                "null" if row["rms_px"] is None else f"{row['rms_px']:.2f}"
                for row in diagnostics["leave_one_out"]
            ),
        )

    output = {
        "format": "psvr2-mode4-forced-prescan-joint-rig-v2",
        "diagnostic_pass": all(upper[str(index)]["diagnostic_pass"] for index in (2, 3)),
        "runtime_usable": False,
        "inputs": {
            "calibration": str(args.calibration),
            "base_calibration": None
            if args.base_calibration is None
            else str(args.base_calibration),
            "capture_root": str(args.capture_root),
            "hand": args.hand,
            "blob_counts": [[len(points) for points in row] for row in observations],
        },
        "lower_stereo": stereo,
        "upper_cameras": upper,
        "note": (
            "Diagnostic geometric fit only. Translation is physically constrained and each pose is "
            "held out in turn, but the native mode-4 K/orientation corrections still require an "
            "independent runtime validation before installation."
        ),
    }
    args.output.write_text(json.dumps(output, indent=2) + "\n")
    print(f"wrote {args.output}")
    return 0 if output["diagnostic_pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())

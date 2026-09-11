#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Fit one provisional native mode-4 fisheye camera model from Sense LEDs."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np

from psvr2_tracking_affine_refine import load_view, required_camera_validation, robust_cost
from psvr2_tracking_geometry import (
    CameraModel,
    bootstrap_pose,
    load_camera_models,
    load_capture_blobs,
    load_led_model,
    parameters_pose,
    pose_parameters,
    project_model,
    sha256_capture_images,
    sha256_file,
)


def initial_intrinsics(camera: CameraModel) -> np.ndarray:
    mapped = camera.H_mode3_to_mode4 @ camera.K
    return np.asarray(
        [
            np.hypot(mapped[0, 0], mapped[0, 1]),
            np.hypot(mapped[1, 0], mapped[1, 1]),
            mapped[0, 2],
            mapped[1, 2],
        ],
        dtype=np.float64,
    )


def initial_parameters(camera: CameraModel, views) -> np.ndarray:
    return np.concatenate((initial_intrinsics(camera), *(pose_parameters(view.pose) for view in views)))


def unpack_parameters(parameters, base_cameras, camera_index, view_count):
    cameras = list(base_cameras)
    fx, fy, cx, cy = parameters[:4]
    K = np.asarray([[fx, 0.0, cx], [0.0, fy, cy], [0.0, 0.0, 1.0]])
    source = base_cameras[camera_index]
    cameras[camera_index] = CameraModel(K, source.D, np.eye(3), source.T_rig_camera)
    poses = [parameters_pose(parameters[4 + index * 6 : 10 + index * 6]) for index in range(view_count)]
    return cameras, poses


def residuals(parameters, base_cameras, camera_index, positions, normals, views, initial):
    cameras, poses = unpack_parameters(parameters, base_cameras, camera_index, len(views))
    values = []
    for view, pose in zip(views, poses):
        for index, camera in enumerate(cameras):
            projected, _ = project_model(pose, camera, positions, normals, use_facing=False)
            for match in view.matches[index]:
                values.extend(projected[match["led_id"]] - np.asarray(match["observed_mode4_px"]))
    data_count = len(values)
    values.extend((parameters[:4] - initial[:4]) / np.asarray([40.0, 40.0, 60.0, 60.0]))
    values.append((parameters[0] - parameters[1]) / 15.0)
    return np.asarray(values, dtype=np.float64), data_count


def refine_intrinsics(base_cameras, camera_index, positions, normals, views, iterations=30):
    parameters = initial_parameters(base_cameras[camera_index], views)
    initial = parameters.copy()
    damping = 1.0
    history = []
    for iteration in range(iterations):
        current, data_count = residuals(
            parameters, base_cameras, camera_index, positions, normals, views, initial
        )
        weights = np.ones(len(current))
        absolute = np.abs(current[:data_count])
        weights[:data_count] = np.where(absolute <= 4.0, 1.0, 4.0 / np.maximum(absolute, 1e-9))
        sqrt_weights = np.sqrt(weights)
        jacobian = np.empty((len(current), len(parameters)), dtype=np.float64)
        for column in range(len(parameters)):
            step = 1e-4 if column < 4 else 1e-5
            offset = np.zeros(len(parameters))
            offset[column] = step
            plus = residuals(
                parameters + offset, base_cameras, camera_index, positions, normals, views, initial
            )[0]
            minus = residuals(
                parameters - offset, base_cameras, camera_index, positions, normals, views, initial
            )[0]
            jacobian[:, column] = (plus - minus) / (2.0 * step)
        weighted = jacobian * sqrt_weights[:, None]
        lhs = weighted.T @ weighted + damping * np.eye(len(parameters))
        rhs = weighted.T @ (-current * sqrt_weights)
        try:
            delta = np.linalg.solve(lhs, rhs)
        except np.linalg.LinAlgError:
            break
        baseline = robust_cost(current, data_count)
        improved = False
        for scale in (1.0, 0.5, 0.25, 0.125):
            trial = parameters + scale * delta
            trial_residual, trial_count = residuals(
                trial, base_cameras, camera_index, positions, normals, views, initial
            )
            if robust_cost(trial_residual, trial_count) < baseline:
                parameters = trial
                damping = max(damping * 0.5, 1e-6)
                improved = True
                break
        if not improved:
            damping *= 10.0
        latest, latest_count = residuals(
            parameters, base_cameras, camera_index, positions, normals, views, initial
        )
        history.append(
            {
                "iteration": iteration + 1,
                "data_rms_px": float(np.sqrt(np.mean(np.square(latest[:latest_count])))),
                "robust_cost": robust_cost(latest, latest_count),
            }
        )
    return unpack_parameters(parameters, base_cameras, camera_index, len(views)), history


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=Path)
    parser.add_argument("--camera", type=int, choices=range(4), required=True)
    parser.add_argument("--geometry", action="append", type=Path, default=[])
    parser.add_argument("--candidate-geometry", action="append", type=Path, default=[])
    parser.add_argument("--validation-capture", type=Path, required=True)
    parser.add_argument("--hand", choices=("left", "right"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.geometry:
        parser.error("at least one --geometry is required")

    calibration_sha256 = sha256_file(args.calibration)
    base_cameras = load_camera_models(json.loads(args.calibration.read_text()))
    repo_root = Path(__file__).resolve().parents[1]
    positions, normals = load_led_model(repo_root, args.hand)
    views = [load_view(path, args.hand, False, calibration_sha256) for path in args.geometry]
    views.extend(load_view(path, args.hand, True, calibration_sha256) for path in args.candidate_geometry)
    (cameras, poses), history = refine_intrinsics(base_cameras, args.camera, positions, normals, views)

    observations = load_capture_blobs(args.validation_capture)
    validation_pose, diagnostics = bootstrap_pose(cameras, positions, normals, observations)
    camera_validation = required_camera_validation(diagnostics, [args.camera])
    image_sha256, image_count = sha256_capture_images(args.validation_capture)
    fitted = cameras[args.camera]
    result = {
        "format": "psvr2-tracking-intrinsics-refinement-v1",
        "runtime_usable": False,
        "camera": args.camera,
        "calibration": {"path": str(args.calibration), "sha256": calibration_sha256},
        "hand": args.hand,
        "training": [
            {"path": str(view.path), "sha256": sha256_file(view.path), "admitted_as": view.admitted_as}
            for view in views
        ],
        "initial_K": initial_intrinsics(base_cameras[args.camera]).reshape(4).tolist(),
        "refined_K": fitted.K.tolist(),
        "fixed_D": fitted.D.tolist(),
        "optimization": {"iterations": history, "final_data_rms_px": history[-1]["data_rms_px"]},
        "validation": {
            "capture": str(args.validation_capture),
            "mode4_image_count": image_count,
            "mode4_images_sha256": image_sha256,
            "survey_sha256": sha256_file(args.validation_capture / "survey.json"),
            "blob_counts": [len(points) for points in observations],
            "diagnostics": diagnostics,
            "required_camera_validation": camera_validation,
            "T_rig_controller": None if validation_pose is None else validation_pose.tolist(),
        },
        "note": "Provisional single-camera mode-4 K with visible-mode distortion fixed; broader validation is required.",
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Camera {args.camera}: training RMS={history[-1]['data_rms_px']:.3f}px")
    print(
        f"Validation: {diagnostics['status']}; matches={diagnostics.get('matched_blobs', 0)} "
        f"cameras={diagnostics.get('supporting_cameras', 0)} RMS={diagnostics.get('rms_px', float('inf')):.3f}px; "
        f"required-camera={camera_validation['status']}"
    )
    print(f"Wrote {args.output}")
    return 0 if diagnostics["status"] == "bootstrap_accepted" and camera_validation["status"] == "passed" else 2


if __name__ == "__main__":
    raise SystemExit(main())

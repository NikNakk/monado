#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Jointly refine provisional mode-3-to-mode-4 camera readout transforms.

This consumes geometry-bootstrap correspondences and keeps the calibrated
visible fisheye lenses plus physical rig fixed.  It is an offline bridge to a
proper mode-4 calibration, not a runtime camera model.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path

import numpy as np

from psvr2_tracking_geometry import (
    CameraModel,
    bootstrap_pose,
    load_camera_models,
    load_capture_blobs,
    load_led_model,
    parameters_pose,
    pose_parameters,
    project_model,
    score_pose,
    sha256_capture_images,
    sha256_file,
)


@dataclass
class TrainingView:
    path: Path
    pose: np.ndarray
    matches: list[list[dict]]
    admitted_as: str


def load_view(path: Path, hand: str, allow_candidate: bool, calibration_sha256: str) -> TrainingView:
    data = json.loads(path.read_text())
    if data.get("format") != "psvr2-tracking-geometry-bootstrap-v1":
        raise ValueError(f"{path}: unsupported geometry format")
    if data.get("hand") != hand:
        raise ValueError(f"{path}: geometry is for {data.get('hand')}, not {hand}")
    if data.get("inputs", {}).get("calibration", {}).get("sha256") != calibration_sha256:
        raise ValueError(f"{path}: geometry used a different calibration file")
    accepted_pose = data.get("T_rig_controller")
    if accepted_pose is not None:
        pose = accepted_pose
        admitted_as = "accepted"
    elif allow_candidate:
        pose = data["diagnostics"].get("best_candidate_T_rig_controller")
        if pose is None:
            raise ValueError(f"{path}: rejected result has no preserved candidate pose")
        admitted_as = "explicit_candidate"
    else:
        raise ValueError(f"{path}: rejected geometry must be passed with --candidate-geometry")
    per_camera = data["diagnostics"]["per_camera"]
    if len(per_camera) != 4:
        raise ValueError(f"{path}: expected four cameras")
    return TrainingView(path, np.asarray(pose, dtype=np.float64), [entry["matches"] for entry in per_camera], admitted_as)


def initial_parameters(cameras: list[CameraModel], views: list[TrainingView]) -> np.ndarray:
    affines = np.concatenate([camera.H_mode3_to_mode4[:2].reshape(6) for camera in cameras])
    poses = np.concatenate([pose_parameters(view.pose) for view in views])
    return np.concatenate((affines, poses))


def unpack_parameters(parameters: np.ndarray, cameras: list[CameraModel], view_count: int):
    refined_cameras = []
    for camera_index, camera in enumerate(cameras):
        affine = np.eye(3)
        affine[:2] = parameters[camera_index * 6 : camera_index * 6 + 6].reshape(2, 3)
        refined_cameras.append(CameraModel(camera.K, camera.D, affine, camera.T_rig_camera))
    poses = [parameters_pose(parameters[24 + index * 6 : 30 + index * 6]) for index in range(view_count)]
    return refined_cameras, poses


def fixed_residuals(parameters, cameras, positions, normals, views, initial_affines, include_prior=True):
    refined_cameras, poses = unpack_parameters(parameters, cameras, len(views))
    residuals = []
    for view, pose in zip(views, poses):
        for camera_index, camera in enumerate(refined_cameras):
            projected, _ = project_model(pose, camera, positions, normals, use_facing=False)
            for match in view.matches[camera_index]:
                residuals.extend(projected[match["led_id"]] - np.asarray(match["observed_mode4_px"]))
    data_count = len(residuals)
    if include_prior:
        sigma = np.tile([0.08, 0.08, 40.0, 0.08, 0.08, 40.0], 4)
        residuals.extend((parameters[:24] - initial_affines) / sigma)
    return np.asarray(residuals, dtype=np.float64), data_count


def robust_cost(residuals: np.ndarray, data_count: int, huber_px=4.0) -> float:
    data = np.abs(residuals[:data_count])
    cost = np.where(data <= huber_px, 0.5 * data**2, huber_px * (data - 0.5 * huber_px)).sum()
    return float(cost + 0.5 * np.square(residuals[data_count:]).sum())


def refine_affines(cameras, positions, normals, views, iterations=30):
    parameters = initial_parameters(cameras, views)
    initial = parameters.copy()
    initial_affines = initial[:24].copy()
    damping = 1.0
    history = []
    for iteration in range(iterations):
        residual, data_count = fixed_residuals(parameters, cameras, positions, normals, views, initial_affines)
        weights = np.ones(len(residual))
        absolute = np.abs(residual[:data_count])
        weights[:data_count] = np.where(absolute <= 4.0, 1.0, 4.0 / np.maximum(absolute, 1e-9))
        sqrt_weights = np.sqrt(weights)
        jacobian = np.empty((len(residual), len(parameters)), dtype=np.float64)
        for column in range(len(parameters)):
            step = 1e-4 if column < 24 else 1e-5
            offset = np.zeros(len(parameters))
            offset[column] = step
            plus = fixed_residuals(
                parameters + offset, cameras, positions, normals, views, initial_affines
            )[0]
            minus = fixed_residuals(
                parameters - offset, cameras, positions, normals, views, initial_affines
            )[0]
            jacobian[:, column] = (plus - minus) / (2.0 * step)
        weighted_jacobian = jacobian * sqrt_weights[:, None]
        lhs = weighted_jacobian.T @ weighted_jacobian + damping * np.eye(len(parameters))
        rhs = weighted_jacobian.T @ (-residual * sqrt_weights)
        try:
            delta = np.linalg.solve(lhs, rhs)
        except np.linalg.LinAlgError:
            break
        baseline = robust_cost(residual, data_count)
        improved = False
        for scale in (1.0, 0.5, 0.25, 0.125):
            trial = parameters + scale * delta
            trial_residual, trial_data_count = fixed_residuals(
                trial, cameras, positions, normals, views, initial_affines
            )
            if robust_cost(trial_residual, trial_data_count) < baseline:
                parameters = trial
                damping = max(damping * 0.5, 1e-6)
                improved = True
                break
        if not improved:
            damping *= 10.0
        current, current_data_count = fixed_residuals(parameters, cameras, positions, normals, views, initial_affines)
        history.append(
            {
                "iteration": iteration + 1,
                "data_rms_px": float(np.sqrt(np.mean(np.square(current[:current_data_count])))),
                "robust_cost": robust_cost(current, current_data_count),
            }
        )
    return unpack_parameters(parameters, cameras, len(views)), history


def score_training_view(view, pose, cameras, positions, normals):
    errors = []
    per_camera = []
    for camera_index, camera in enumerate(cameras):
        projected, _ = project_model(pose, camera, positions, normals, use_facing=False)
        camera_errors = [
            float(np.linalg.norm(projected[m["led_id"]] - np.asarray(m["observed_mode4_px"])))
            for m in view.matches[camera_index]
        ]
        errors.extend(camera_errors)
        per_camera.append({"camera": camera_index, "matches": len(camera_errors), "rms_px": float(np.sqrt(np.mean(np.square(camera_errors)))) if camera_errors else None})
    return {
        "matches": len(errors),
        "rms_px": float(np.sqrt(np.mean(np.square(errors)))) if errors else None,
        "per_camera": per_camera,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=Path)
    parser.add_argument("--geometry", action="append", type=Path, default=[], help="accepted bootstrap JSON")
    parser.add_argument("--candidate-geometry", action="append", type=Path, default=[], help="explicit rejected near-miss JSON")
    parser.add_argument("--validation-capture", type=Path)
    parser.add_argument("--hand", choices=("left", "right"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.geometry:
        parser.error("at least one --geometry is required")

    calibration_sha256 = sha256_file(args.calibration)
    calibration = json.loads(args.calibration.read_text())
    cameras = load_camera_models(calibration)
    repo_root = Path(__file__).resolve().parents[1]
    positions, normals = load_led_model(repo_root, args.hand)
    views = [load_view(path, args.hand, False, calibration_sha256) for path in args.geometry]
    views.extend(load_view(path, args.hand, True, calibration_sha256) for path in args.candidate_geometry)
    (refined_cameras, refined_poses), history = refine_affines(cameras, positions, normals, views)

    validation = None
    if args.validation_capture is not None:
        observations = load_capture_blobs(args.validation_capture)
        image_sha256, image_count = sha256_capture_images(args.validation_capture)
        validation_pose, diagnostics = bootstrap_pose(refined_cameras, positions, normals, observations)
        validation = {
            "capture": str(args.validation_capture),
            "mode4_image_count": image_count,
            "mode4_images_sha256": image_sha256,
            "survey_sha256": sha256_file(args.validation_capture / "survey.json"),
            "blob_counts": [len(points) for points in observations],
            "diagnostics": diagnostics,
            "T_rig_controller": None if validation_pose is None else validation_pose.tolist(),
        }

    result = {
        "format": "psvr2-tracking-affine-refinement-v1",
        "calibration": {"path": str(args.calibration), "sha256": calibration_sha256},
        "led_model_sha256": sha256_file(repo_root / "src/xrt/drivers/pssense/pssense_led_model.h"),
        "hand": args.hand,
        "training": [
            {
                "path": str(view.path),
                "sha256": sha256_file(view.path),
                "admitted_as": view.admitted_as,
                **score_training_view(view, pose, refined_cameras, positions, normals),
            }
            for view, pose in zip(views, refined_poses)
        ],
        "cameras": [
            {
                "camera": index,
                "initial_H_mode3_to_mode4": cameras[index].H_mode3_to_mode4.tolist(),
                "refined_H_mode3_to_mode4": camera.H_mode3_to_mode4.tolist(),
            }
            for index, camera in enumerate(refined_cameras)
        ],
        "optimization": {"iterations": history, "final_data_rms_px": history[-1]["data_rms_px"]},
        "validation": validation,
        "runtime_usable": False,
        "note": "Provisional affine bridge only; retain visible fisheye intrinsics and require broader held-out validation before runtime use.",
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Training views: {len(views)}; final fixed-correspondence RMS={history[-1]['data_rms_px']:.3f}px")
    if validation is not None:
        diagnostics = validation["diagnostics"]
        print(
            f"Validation: {diagnostics['status']}; matches={diagnostics.get('matched_blobs', 0)} "
            f"cameras={diagnostics.get('supporting_cameras', 0)} RMS={diagnostics.get('rms_px', float('inf')):.3f}px"
        )
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

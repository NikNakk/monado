#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Validate the PSVR2 visible->tracking bridge without moving physical cameras.

This diagnostic keeps the reviewed visible-camera rig extrinsics fixed. It uses
forced-PRESCAN mode-4 observations from cameras 0/1 to triangulate unlabeled
Sense LED points, then asks whether cameras 2/3 predict those points when using:

  1. the full visible-fisheye + measured affine bridge model,
  2. the current diagonal-K native collapse (which discards bridge rotation),
  3. a diagonal-K native approximation that absorbs the bridge's in-plane
     rotation into camera roll while keeping the camera centre unchanged.

It also reports the residual shear that cannot be represented by a diagonal K.
No output from this script is runtime-usable calibration.
"""
from __future__ import annotations

import argparse
import itertools
import json
import math
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

from psvr2_tracking_geometry import (
    CameraModel,
    load_camera_models,
    load_led_model,
    mode4_points_to_rays,
)
from psvr2_tracking_joint_rig_refine import merged_blinks

ACTIVE_W = 508
ACTIVE_H = 508


@dataclass
class NativeDecomposition:
    camera: CameraModel
    theta_rad: float
    M: np.ndarray
    linear_after_rotation: np.ndarray
    residual_linear: np.ndarray
    residual_frobenius: float


def rotation_z(theta: float) -> np.ndarray:
    c = math.cos(theta)
    s = math.sin(theta)
    result = np.eye(4, dtype=np.float64)
    result[:3, :3] = np.asarray([[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]])
    return result


def zero_skew_cost(theta: float, B: np.ndarray) -> float:
    c = math.cos(theta)
    s = math.sin(theta)
    R = np.asarray([[c, -s], [s, c]], dtype=np.float64)
    C = B @ R.T
    return float(C[0, 1] ** 2 + C[1, 0] ** 2)


def best_zero_skew_rotation(B: np.ndarray) -> float:
    # A dense 0.01-degree search is deterministic, dependency-free, and ample
    # for a diagnostic whose image residuals are measured in pixels.
    angles = np.linspace(-math.pi, math.pi, 36001, endpoint=True)
    costs = np.empty(len(angles), dtype=np.float64)
    for index, theta in enumerate(angles):
        costs[index] = zero_skew_cost(float(theta), B)
    theta = float(angles[int(np.argmin(costs))])

    # Resolve the pi-equivalent solution so focal lengths are positive.
    for candidate in (theta, theta + math.pi, theta - math.pi):
        c = math.cos(candidate)
        s = math.sin(candidate)
        C = B @ np.asarray([[c, s], [-s, c]], dtype=np.float64)
        if C[0, 0] > 0.0 and C[1, 1] > 0.0:
            return float((candidate + math.pi) % (2.0 * math.pi) - math.pi)
    return theta


def roll_preserving_native(camera: CameraModel) -> NativeDecomposition:
    M = np.asarray(camera.H_mode3_to_mode4 @ camera.K, dtype=np.float64)
    B = M[:2, :2]
    theta = best_zero_skew_rotation(B)
    c = math.cos(theta)
    s = math.sin(theta)
    R2 = np.asarray([[c, -s], [s, c]], dtype=np.float64)
    C = B @ R2.T
    K = np.asarray(
        [[C[0, 0], 0.0, M[0, 2]], [0.0, C[1, 1], M[1, 2]], [0.0, 0.0, 1.0]],
        dtype=np.float64,
    )
    reconstructed = K[:2, :2] @ R2
    residual = B - reconstructed

    # M ~= K_native Rz(theta). Since T_rig_camera maps camera->rig,
    # q_native = Rz(theta) q_visible is represented by
    # T_rig_camera_native = T_rig_camera_visible Rz(-theta).
    T = camera.T_rig_camera @ rotation_z(-theta)
    native = CameraModel(K, camera.D.copy(), np.eye(3), T)
    return NativeDecomposition(
        camera=native,
        theta_rad=theta,
        M=M,
        linear_after_rotation=C,
        residual_linear=residual,
        residual_frobenius=float(np.linalg.norm(residual)),
    )


def current_native_collapse(camera: CameraModel) -> CameraModel:
    mapped = camera.H_mode3_to_mode4 @ camera.K
    K = np.asarray(
        [
            [np.hypot(mapped[0, 0], mapped[0, 1]), 0.0, mapped[0, 2]],
            [0.0, np.hypot(mapped[1, 0], mapped[1, 1]), mapped[1, 2]],
            [0.0, 0.0, 1.0],
        ],
        dtype=np.float64,
    )
    return CameraModel(K, camera.D.copy(), np.eye(3), camera.T_rig_camera.copy())


def project_rig_points(points_rig: np.ndarray, camera: CameraModel) -> tuple[np.ndarray, np.ndarray]:
    T_camera_rig = np.linalg.inv(camera.T_rig_camera)
    points_camera = (T_camera_rig[:3, :3] @ points_rig.T).T + T_camera_rig[:3, 3]
    projected, _ = cv2.fisheye.projectPoints(
        points_camera.reshape(-1, 1, 3),
        np.zeros((3, 1), dtype=np.float64),
        np.zeros((3, 1), dtype=np.float64),
        camera.K,
        camera.D,
    )
    projected = projected.astype(np.float32)
    if not np.allclose(camera.H_mode3_to_mode4, np.eye(3)):
        projected = cv2.perspectiveTransform(projected, camera.H_mode3_to_mode4)
    projected = projected.reshape(-1, 2).astype(np.float64)
    valid = (
        (points_camera[:, 2] > 0.02)
        & (projected[:, 0] >= 0.0)
        & (projected[:, 0] < ACTIVE_W)
        & (projected[:, 1] >= 0.0)
        & (projected[:, 1] < ACTIVE_H)
    )
    return projected, valid


def unique_matches(projected: np.ndarray, valid: np.ndarray, observed: np.ndarray, limit_px: float):
    candidates = []
    for point_index in np.flatnonzero(valid):
        for blob_index, blob in enumerate(observed):
            error = float(np.linalg.norm(projected[point_index] - blob))
            if error <= limit_px:
                candidates.append((error, int(point_index), int(blob_index)))
    used_points = set()
    used_blobs = set()
    result = []
    for candidate in sorted(candidates):
        if candidate[1] in used_points or candidate[2] in used_blobs:
            continue
        used_points.add(candidate[1])
        used_blobs.add(candidate[2])
        result.append(candidate)
    return result


def score_camera(camera: CameraModel, clouds, observations, limits=(5.0, 10.0, 20.0, 40.0)) -> dict:
    result = {}
    for limit in limits:
        errors = []
        counts = []
        for points, observed in zip(clouds, observations):
            projected, valid = project_rig_points(points, camera)
            matches = unique_matches(projected, valid, observed, limit)
            counts.append(len(matches))
            errors.extend(match[0] for match in matches)
        result[str(int(limit))] = {
            "matches": len(errors),
            "per_pose_matches": counts,
            "rms_px": None if not errors else float(np.sqrt(np.mean(np.square(errors)))),
            "median_px": None if not errors else float(np.median(errors)),
            "p95_px": None if not errors else float(np.percentile(errors, 95)),
        }
    return result


def stereo_cloud(cam0, cam1, obs0, obs1, model_distances):
    rays0 = mode4_points_to_rays(obs0, cam0)
    rays1 = mode4_points_to_rays(obs1, cam1)
    T10 = np.linalg.inv(cam1.T_rig_camera) @ cam0.T_rig_camera
    P0 = np.c_[np.eye(3), np.zeros(3)]
    P1 = T10[:3]
    zero_large = len(rays0) >= len(rays1)
    large, small = (rays0, rays1) if zero_large else (rays1, rays0)
    best = None
    for permutation in itertools.permutations(range(len(large)), len(small)):
        a = np.asarray([rays0[i] for i in permutation]) if zero_large else rays0
        b = rays1 if zero_large else np.asarray([rays1[i] for i in permutation])
        homogeneous = cv2.triangulatePoints(P0, P1, a.T, b.T)
        X0 = (homogeneous[:3] / homogeneous[3]).T
        X1 = (T10[:3, :3] @ X0.T).T + T10[:3, 3]
        if np.any(X0[:, 2] < 0.20) or np.any(X0[:, 2] > 1.20) or np.any(X1[:, 2] < 0.20):
            continue
        distances = np.asarray(
            [np.linalg.norm(X0[i] - X0[j]) for i, j in itertools.combinations(range(len(X0)), 2)]
        )
        diameter = float(distances.max()) if len(distances) else 0.0
        if diameter > 0.18:
            continue
        pair_error = (
            float(np.mean([np.min(np.abs(model_distances - value)) for value in distances]))
            if len(distances)
            else 0.0
        )
        repro0 = X0[:, :2] / X0[:, 2:3]
        repro1 = X1[:, :2] / X1[:, 2:3]
        ray_rms = float(
            np.sqrt(np.mean(np.r_[np.sum((repro0 - a) ** 2, axis=1), np.sum((repro1 - b) ** 2, axis=1)]))
        )
        score = 10000.0 * ray_rms + 400.0 * pair_error + 1000.0 * max(0.0, diameter - 0.145)
        if best is None or score < best[0]:
            Xrig = (cam0.T_rig_camera[:3, :3] @ X0.T).T + cam0.T_rig_camera[:3, 3]
            best = (score, Xrig, diameter, pair_error, ray_rms)
    if best is None:
        raise RuntimeError("no physically plausible camera0/1 stereo assignment")
    return best


def synthetic_equivalence(reference: CameraModel, candidate: CameraModel) -> dict:
    # Sample rays in the reference visible-camera frame, transform them into
    # rig space, then compare the two models wherever the reference projects
    # into the active tracking raster.
    points = []
    for polar in np.linspace(0.0, math.radians(85.0), 50):
        for azimuth in np.linspace(-math.pi, math.pi, 96, endpoint=False):
            direction = np.asarray(
                [
                    math.sin(polar) * math.cos(azimuth),
                    math.sin(polar) * math.sin(azimuth),
                    math.cos(polar),
                ]
            )
            points.append(reference.T_rig_camera[:3, :3] @ direction + reference.T_rig_camera[:3, 3])
    points = np.asarray(points, dtype=np.float64)
    ref_px, ref_valid = project_rig_points(points, reference)
    cand_px, cand_valid = project_rig_points(points, candidate)
    valid = ref_valid & cand_valid
    errors = np.linalg.norm(ref_px[valid] - cand_px[valid], axis=1)
    return {
        "sample_count": int(valid.sum()),
        "median_px": None if not len(errors) else float(np.median(errors)),
        "p95_px": None if not len(errors) else float(np.percentile(errors, 95)),
        "max_px": None if not len(errors) else float(np.max(errors)),
    }


def pose_directories(root: Path):
    directories = []
    for index in range(6):
        matches = sorted(path for path in root.glob(f"F{index:02d}*") if path.is_dir())
        if len(matches) != 1:
            raise ValueError(f"expected exactly one F{index:02d}* directory, found {len(matches)}")
        directories.append(matches[0])
    return directories


def camera_summary(index, source, decomp, current):
    centre_shift = float(np.linalg.norm(decomp.camera.T_rig_camera[:3, 3] - source.T_rig_camera[:3, 3]))
    M = decomp.M
    B = M[:2, :2]
    return {
        "camera": index,
        "mapped_matrix_HK": M.tolist(),
        "mapped_linear": B.tolist(),
        "roll_preserving": {
            "theta_deg": math.degrees(decomp.theta_rad),
            "K": decomp.camera.K.tolist(),
            "T_rig_camera": decomp.camera.T_rig_camera.tolist(),
            "camera_centre_shift_m": centre_shift,
            "linear_after_rotation": decomp.linear_after_rotation.tolist(),
            "residual_linear": decomp.residual_linear.tolist(),
            "residual_frobenius": decomp.residual_frobenius,
            "equivalence_to_full_bridge": synthetic_equivalence(source, decomp.camera),
        },
        "current_diagonal_collapse": {
            "K": current.K.tolist(),
            "T_rig_camera": current.T_rig_camera.tolist(),
            "equivalence_to_full_bridge": synthetic_equivalence(source, current),
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=Path, help="reviewed calibration containing visible cameras and mode-12 bridge")
    parser.add_argument("capture_root", type=Path, help="forced-PRESCAN F00..F05 root")
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
    print("blob counts:", ["/".join(str(len(points)) for points in row) for row in observations])

    clouds = []
    stereo = []
    for pose_index, row in enumerate(observations):
        solution = stereo_cloud(cameras[0], cameras[1], row[0], row[1], model_distances)
        clouds.append(solution[1])
        stereo.append(
            {
                "pose": pose_index,
                "point_count": len(solution[1]),
                "diameter_m": solution[2],
                "model_pair_error_m": solution[3],
                "normalized_ray_rms": solution[4],
            }
        )
        print(
            f"F{pose_index:02d}: {len(solution[1])} points, diameter={1000*solution[2]:.1f}mm, "
            f"pair-error={1000*solution[3]:.2f}mm"
        )

    models = {}
    for camera_index, source in enumerate(cameras):
        decomp = roll_preserving_native(source)
        current = current_native_collapse(source)
        entry = camera_summary(camera_index, source, decomp, current)
        entry["forced_prescan"] = {
            "full_bridge": score_camera(source, clouds, [row[camera_index] for row in observations]),
            "roll_preserving_native": score_camera(
                decomp.camera, clouds, [row[camera_index] for row in observations]
            ),
            "current_diagonal_collapse": score_camera(
                current, clouds, [row[camera_index] for row in observations]
            ),
        }
        models[str(camera_index)] = entry
        eq_new = entry["roll_preserving"]["equivalence_to_full_bridge"]
        eq_old = entry["current_diagonal_collapse"]["equivalence_to_full_bridge"]
        bridge20 = entry["forced_prescan"]["full_bridge"]["20"]
        roll20 = entry["forced_prescan"]["roll_preserving_native"]["20"]
        old20 = entry["forced_prescan"]["current_diagonal_collapse"]["20"]
        print(
            f"camera {camera_index}: bridge roll={math.degrees(decomp.theta_rad):+.2f}deg; "
            f"native-vs-bridge p95={eq_new['p95_px']:.2f}px (old collapse {eq_old['p95_px']:.2f}px); "
            f"forced@20px full={bridge20['matches']}/{bridge20['rms_px']} "
            f"roll={roll20['matches']}/{roll20['rms_px']} old={old20['matches']}/{old20['rms_px']}"
        )

    result = {
        "format": "psvr2-tracking-bridge-validation-v1",
        "runtime_usable": False,
        "calibration": str(args.calibration),
        "capture_root": str(args.capture_root),
        "hand": args.hand,
        "stereo_anchor": stereo,
        "cameras": models,
        "note": (
            "Diagnostic only. The roll-preserving native model keeps the physical camera centre fixed and absorbs only "
            "the measured bridge's in-plane rotation into camera roll. Residual shear is reported rather than silently "
            "fitted as an arbitrary 3D camera-pose change."
        ),
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print("wrote", args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

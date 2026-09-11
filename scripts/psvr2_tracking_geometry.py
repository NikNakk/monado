#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Bootstrap Sense LED identities jointly across the PSVR2 mode-4 camera rig.

This is an offline calibration aid, not a runtime tracker. It uses the measured
visible/tracking overlap only to initialize rays, then requires a single Sense
pose and LED assignments to agree across multiple synchronized cameras.
"""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import re
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np

from psvr2_cross_mode_register import available_examples, blinking_centroids, mode4_camera


@dataclass
class CameraModel:
    K: np.ndarray
    D: np.ndarray
    H_mode3_to_mode4: np.ndarray
    T_rig_camera: np.ndarray


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_capture_images(root: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    paths = sorted(root.glob("visit-*-mode-04-*.pgm"))
    for path in paths:
        digest.update(path.relative_to(root).as_posix().encode())
        digest.update(b"\0")
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    return digest.hexdigest(), len(paths)


def load_led_model(repo_root: Path, hand: str) -> tuple[np.ndarray, np.ndarray]:
    text = (repo_root / "src/xrt/drivers/pssense/pssense_led_model.h").read_text()
    start = f"pssense_{hand}_leds[] = {{"
    following = "pssense_right_leds[]" if hand == "left" else "};"
    section = text.split(start, 1)[1].split(following, 1)[0]
    blocks = re.findall(r"\.position = \{([^}]+)\},\s*\.normal = \{([^}]+)\}", section)
    positions = np.asarray([[float(value) for value in position.split(",")] for position, _ in blocks])
    normals = np.asarray([[float(value) for value in normal.split(",")] for _, normal in blocks])
    if positions.shape != (17, 3) or normals.shape != (17, 3):
        raise ValueError(f"expected 17 {hand} Sense LEDs, parsed {len(positions)}")
    return positions, normals


def load_camera_models(calibration: dict) -> list[CameraModel]:
    overlap = calibration["tracking_readout"]["mode12_visible_to_tracking"]
    models = overlap.get("affine_bootstrap_models", [])
    if overlap.get("status") != "estimated_overlap_only" or len(models) != 4:
        raise ValueError("calibration must embed four mode-12 visible/tracking overlap estimates")
    visible_half = np.asarray([[0.5, 0.0, -0.25], [0.0, 0.5, -0.25], [0.0, 0.0, 1.0]])
    tracking_double = np.asarray([[2.0, 0.0, 0.5], [0.0, 2.0, 0.5], [0.0, 0.0, 1.0]])
    result = []
    for camera in range(4):
        camera_json = calibration["visible_cameras"][f"camera{camera}"]
        affine = next(model for model in models if int(model["visible_plane"]) == camera)
        visible_to_tracking = np.vstack((np.asarray(affine["matrix_visible_to_tracking"]), [0.0, 0.0, 1.0]))
        result.append(
            CameraModel(
                K=np.asarray(camera_json["K"], dtype=np.float64),
                D=np.asarray(camera_json["D"], dtype=np.float64),
                H_mode3_to_mode4=tracking_double @ visible_to_tracking @ visible_half,
                T_rig_camera=np.asarray(camera_json["transform_to_rig_T_rig_camera"], dtype=np.float64),
            )
        )
    return result


def load_capture_blobs(root: Path) -> list[np.ndarray]:
    survey = json.loads((root / "survey.json").read_text())
    images = [[] for _ in range(4)]
    for visit, mode in enumerate(survey.get("capture_plan", [])):
        if mode != 4:
            continue
        for example in available_examples(root, visit, 4):
            for camera in range(4):
                image = mode4_camera(root, visit, example, camera)
                if image is not None:
                    images[camera].append(image)
    return [blinking_centroids(camera_images, 4, 400).astype(np.float64) for camera_images in images]


def mode4_points_to_rays(points: np.ndarray, camera: CameraModel) -> np.ndarray:
    mode3 = cv2.perspectiveTransform(
        points.astype(np.float32).reshape(-1, 1, 2), np.linalg.inv(camera.H_mode3_to_mode4)
    )
    return cv2.fisheye.undistortPoints(mode3, camera.K, camera.D).reshape(-1, 2).astype(np.float64)


def project_model(
    T_rig_device: np.ndarray,
    camera: CameraModel,
    positions: np.ndarray,
    normals: np.ndarray,
    use_facing: bool = True,
) -> tuple[np.ndarray, np.ndarray]:
    T_camera_device = np.linalg.inv(camera.T_rig_camera) @ T_rig_device
    rotation = np.ascontiguousarray(T_camera_device[:3, :3])
    translation = np.ascontiguousarray(T_camera_device[:3, 3].reshape(3, 1))
    rvec, _ = cv2.Rodrigues(rotation)
    mode3, _ = cv2.fisheye.projectPoints(positions.reshape(-1, 1, 3), rvec, translation, camera.K, camera.D)
    mode4 = cv2.perspectiveTransform(mode3.astype(np.float32), camera.H_mode3_to_mode4).reshape(-1, 2)
    camera_points = (rotation @ positions.T).T + translation.reshape(1, 3)
    camera_normals = (rotation @ normals.T).T
    toward_camera = -camera_points / np.linalg.norm(camera_points, axis=1).reshape(-1, 1)
    facing = np.sum(camera_normals * toward_camera, axis=1) >= 0.0
    visible = (
        (camera_points[:, 2] > 0.02)
        & (facing if use_facing else True)
        & (mode4[:, 0] >= 0.0)
        & (mode4[:, 0] < 512.0)
        & (mode4[:, 1] >= 0.0)
        & (mode4[:, 1] < 508.0)
    )
    return mode4.astype(np.float64), visible


def unique_nearest(projected: np.ndarray, observed: np.ndarray, visible: np.ndarray, limit_px: float) -> list[tuple]:
    candidates = []
    for led in np.flatnonzero(visible):
        for blob, point in enumerate(observed):
            error = float(np.linalg.norm(projected[led] - point))
            if error <= limit_px:
                candidates.append((error, int(led), blob))
    used_leds = set()
    used_blobs = set()
    matches = []
    for candidate in sorted(candidates):
        _, led, blob = candidate
        if led not in used_leds and blob not in used_blobs:
            used_leds.add(led)
            used_blobs.add(blob)
            matches.append(candidate)
    return matches


def score_pose(T_rig_device, cameras, positions, normals, observations, limit_px=10.0, use_facing=True) -> dict:
    per_camera = []
    errors = []
    for camera_index, (camera, observed) in enumerate(zip(cameras, observations)):
        projected, visible = project_model(T_rig_device, camera, positions, normals, use_facing=use_facing)
        matches = unique_nearest(projected, observed, visible, limit_px)
        errors.extend(match[0] for match in matches)
        per_camera.append(
            {
                "camera": camera_index,
                "observed": len(observed),
                "predicted_visible": int(visible.sum()),
                "matches": matches,
            }
        )
    return {
        "matched": len(errors),
        "rms_px": float(np.sqrt(np.mean(np.square(errors)))) if errors else float("inf"),
        "per_camera": per_camera,
    }


def neighbour_triplets(points: np.ndarray, depth: int) -> list[tuple[int, int, int]]:
    triplets = []
    for anchor, point in enumerate(points):
        distance = np.linalg.norm(points - point, axis=1)
        neighbours = [int(index) for index in np.argsort(distance) if int(index) != anchor][:depth]
        triplets.extend((anchor, first, second) for first, second in itertools.permutations(neighbours, 2))
    return triplets


def pose_matrix(rvec: np.ndarray, tvec: np.ndarray) -> np.ndarray:
    result = np.eye(4)
    result[:3, :3] = cv2.Rodrigues(rvec)[0]
    result[:3, 3] = tvec.reshape(3)
    return result


def rotation_distance(first: np.ndarray, second: np.ndarray) -> float:
    relative = first[:3, :3].T @ second[:3, :3]
    return float(np.arccos(np.clip((np.trace(relative) - 1.0) * 0.5, -1.0, 1.0)))


def pose_parameters(transform: np.ndarray) -> np.ndarray:
    rvec, _ = cv2.Rodrigues(np.ascontiguousarray(transform[:3, :3]))
    return np.concatenate((rvec.reshape(3), transform[:3, 3]))


def parameters_pose(parameters: np.ndarray) -> np.ndarray:
    return pose_matrix(parameters[:3].reshape(3, 1), parameters[3:].reshape(3, 1))


def correspondence_residuals(parameters, cameras, positions, normals, observations, per_camera) -> np.ndarray:
    transform = parameters_pose(parameters)
    residuals = []
    for camera, observed, entry in zip(cameras, observations, per_camera):
        projected, _ = project_model(transform, camera, positions, normals, use_facing=False)
        for _, led, blob in entry["matches"]:
            residuals.extend(projected[led] - observed[blob])
    return np.asarray(residuals, dtype=np.float64)


def refine_pose(transform, cameras, positions, normals, observations) -> np.ndarray:
    """Refine a rig pose with alternating association and finite-difference LM."""
    parameters = pose_parameters(transform)
    for _ in range(3):
        association = score_pose(
            parameters_pose(parameters), cameras, positions, normals, observations, limit_px=10.0, use_facing=True
        )
        if association["matched"] < 6:
            break
        for _ in range(12):
            residual = correspondence_residuals(
                parameters, cameras, positions, normals, observations, association["per_camera"]
            )
            jacobian = np.empty((len(residual), 6), dtype=np.float64)
            steps = np.asarray([1e-5, 1e-5, 1e-5, 1e-5, 1e-5, 1e-5])
            for column, step in enumerate(steps):
                offset = np.zeros(6)
                offset[column] = step
                plus = correspondence_residuals(
                    parameters + offset, cameras, positions, normals, observations, association["per_camera"]
                )
                minus = correspondence_residuals(
                    parameters - offset, cameras, positions, normals, observations, association["per_camera"]
                )
                jacobian[:, column] = (plus - minus) / (2.0 * step)
            lhs = jacobian.T @ jacobian + np.diag([1e-3, 1e-3, 1e-3, 1.0, 1.0, 1.0])
            try:
                delta = np.linalg.solve(lhs, -(jacobian.T @ residual))
            except np.linalg.LinAlgError:
                break
            improved = False
            baseline = float(residual @ residual)
            for scale in (1.0, 0.5, 0.25, 0.125):
                trial = parameters + scale * delta
                trial_residual = correspondence_residuals(
                    trial, cameras, positions, normals, observations, association["per_camera"]
                )
                if float(trial_residual @ trial_residual) < baseline:
                    parameters = trial
                    improved = True
                    break
            if not improved or float(np.linalg.norm(delta)) < 1e-7:
                break
    return parameters_pose(parameters)


def bootstrap_pose(cameras, positions, normals, observations, neighbour_depth=5) -> tuple[np.ndarray | None, dict]:
    model_triplets = neighbour_triplets(positions, neighbour_depth)
    candidates = []
    identity = np.eye(3)
    for camera_index, observed in enumerate(observations):
        if len(observed) < 4:
            continue
        rays = mode4_points_to_rays(observed, cameras[camera_index])
        image_triplets = neighbour_triplets(observed, min(4, len(observed) - 1))
        local_candidates = []
        for image_ids in image_triplets:
            image_points = rays[list(image_ids)].reshape(3, 1, 2)
            for model_ids in model_triplets:
                ok, rvecs, tvecs = cv2.solveP3P(
                    positions[list(model_ids)].reshape(3, 1, 3),
                    image_points,
                    identity,
                    None,
                    flags=cv2.SOLVEPNP_AP3P,
                )
                if not ok:
                    continue
                for rvec, tvec in zip(rvecs, tvecs):
                    T_rig_device = cameras[camera_index].T_rig_camera @ pose_matrix(rvec, tvec)
                    distance = float(np.linalg.norm(T_rig_device[:3, 3]))
                    if not 0.08 <= distance <= 2.0:
                        continue
                    score = score_pose(
                        T_rig_device,
                        [cameras[camera_index]],
                        positions,
                        normals,
                        [observed],
                        limit_px=8.0,
                        use_facing=False,
                    )
                    if score["matched"] >= min(4, len(observed)):
                        local_candidates.append((score["matched"], score["rms_px"], T_rig_device))
        local_candidates.sort(key=lambda item: (-item[0], item[1]))
        distinct = []
        for candidate in local_candidates:
            if any(
                np.linalg.norm(candidate[2][:3, 3] - other[2][:3, 3]) < 0.005
                and rotation_distance(candidate[2], other[2]) < np.deg2rad(5.0)
                for other in distinct
            ):
                continue
            distinct.append(candidate)
            if len(distinct) == 300:
                break
        candidates.extend(candidate[2] for candidate in distinct)

    ranked = []
    for candidate in candidates:
        score = score_pose(candidate, cameras, positions, normals, observations, limit_px=8.0, use_facing=True)
        supporting = sum(len(entry["matches"]) >= 3 for entry in score["per_camera"])
        ranked.append((score["matched"], supporting, score["rms_px"], candidate, score))
    ranked.sort(key=lambda item: (-item[0], -item[1], item[2]))
    if not ranked:
        return None, {"status": "no_pose", "candidate_count": 0}

    refined = []
    for item in ranked[:20]:
        refined.append(item)
        transform = refine_pose(item[3], cameras, positions, normals, observations)
        score = score_pose(transform, cameras, positions, normals, observations, limit_px=8.0, use_facing=True)
        supporting = sum(len(entry["matches"]) >= 3 for entry in score["per_camera"])
        refined.append((score["matched"], supporting, score["rms_px"], transform, score))
    refined.sort(key=lambda item: (-item[0], -item[1], item[2]))
    best = refined[0]
    accepted = best[0] >= 12 and best[1] >= 2 and best[2] <= 5.0
    public_cameras = []
    for camera_index, entry in enumerate(best[4]["per_camera"]):
        projected, _ = project_model(best[3], cameras[camera_index], positions, normals)
        public_cameras.append(
            {
                **{key: value for key, value in entry.items() if key != "matches"},
                "matches": [
                    {
                        "error_px": error,
                        "led_id": led,
                        "blob_index": blob,
                        "observed_mode4_px": observations[camera_index][blob].tolist(),
                        "projected_mode4_px": projected[led].tolist(),
                    }
                    for error, led, blob in entry["matches"]
                ],
            }
        )
    return (best[3] if accepted else None), {
        "status": "bootstrap_accepted" if accepted else "bootstrap_rejected",
        "candidate_count": len(ranked),
        "matched_blobs": best[0],
        "supporting_cameras": best[1],
        "rms_px": best[2],
        "match_limit_px": 8.0,
        "best_candidate_T_rig_controller": best[3].tolist(),
        "per_camera": public_cameras,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=Path)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--hand", choices=("left", "right"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    calibration = json.loads(args.calibration.read_text())
    cameras = load_camera_models(calibration)
    repo_root = Path(__file__).resolve().parents[1]
    positions, normals = load_led_model(repo_root, args.hand)
    observations = load_capture_blobs(args.capture)
    pose, diagnostics = bootstrap_pose(cameras, positions, normals, observations)
    capture_digest, capture_image_count = sha256_capture_images(args.capture)
    led_model_path = repo_root / "src/xrt/drivers/pssense/pssense_led_model.h"
    result = {
        "format": "psvr2-tracking-geometry-bootstrap-v1",
        "inputs": {
            "calibration": {"path": str(args.calibration), "sha256": sha256_file(args.calibration)},
            "capture": {
                "path": str(args.capture),
                "mode4_image_count": capture_image_count,
                "mode4_images_sha256": capture_digest,
                "survey_sha256": sha256_file(args.capture / "survey.json"),
            },
            "led_model": {"path": str(led_model_path), "sha256": sha256_file(led_model_path)},
        },
        "hand": args.hand,
        "blob_counts": [len(points) for points in observations],
        "diagnostics": diagnostics,
        "T_rig_controller": None if pose is None else pose.tolist(),
        "runtime_usable": False,
        "note": "Overlap-derived rays are initialization only; accepted identities require later intrinsic refinement and held-out validation.",
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Blob counts: {result['blob_counts']}")
    print(
        f"Geometry bootstrap: {diagnostics['status']}; matches={diagnostics.get('matched_blobs', 0)} "
        f"cameras={diagnostics.get('supporting_cameras', 0)} RMS={diagnostics.get('rms_px', float('inf')):.3f}px"
    )
    print(f"Wrote {args.output}")
    return 0 if pose is not None else 2


if __name__ == "__main__":
    raise SystemExit(main())

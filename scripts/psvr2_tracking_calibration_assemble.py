#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Assemble four validated mode-4 fits into a provisional Monado camera rig."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np

from psvr2_tracking_geometry import sha256_file


FORMAT = "psvr2-mode4-constellation-calibration-v1"
REFINEMENT_FORMAT = "psvr2-tracking-intrinsics-refinement-v1"


def rotation_matrix_to_quaternion(matrix: np.ndarray) -> dict[str, float]:
    """Return an x/y/z/w quaternion for a proper 3x3 rotation matrix."""
    m = np.asarray(matrix, dtype=np.float64)
    trace = float(np.trace(m))
    if trace > 0.0:
        scale = math.sqrt(trace + 1.0) * 2.0
        w = 0.25 * scale
        x = (m[2, 1] - m[1, 2]) / scale
        y = (m[0, 2] - m[2, 0]) / scale
        z = (m[1, 0] - m[0, 1]) / scale
    else:
        index = int(np.argmax(np.diag(m)))
        if index == 0:
            scale = math.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2.0
            w = (m[2, 1] - m[1, 2]) / scale
            x, y, z = 0.25 * scale, (m[0, 1] + m[1, 0]) / scale, (m[0, 2] + m[2, 0]) / scale
        elif index == 1:
            scale = math.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2.0
            w = (m[0, 2] - m[2, 0]) / scale
            x, y, z = (m[0, 1] + m[1, 0]) / scale, 0.25 * scale, (m[1, 2] + m[2, 1]) / scale
        else:
            scale = math.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2.0
            w = (m[1, 0] - m[0, 1]) / scale
            x, y, z = (m[0, 2] + m[2, 0]) / scale, (m[1, 2] + m[2, 1]) / scale, 0.25 * scale
    quaternion = np.asarray([x, y, z, w], dtype=np.float64)
    quaternion /= np.linalg.norm(quaternion)
    return dict(zip(("x", "y", "z", "w"), quaternion.tolist()))


def xrt_pose_from_opencv_transform(transform: np.ndarray) -> dict:
    """Convert C T C from OpenCV (+x right,+y down,+z forward) to XRT."""
    transform = np.asarray(transform, dtype=np.float64)
    conversion = np.diag([1.0, -1.0, -1.0, 1.0])
    xrt = conversion @ transform @ conversion
    return {
        "orientation": rotation_matrix_to_quaternion(xrt[:3, :3]),
        "position": dict(zip(("x", "y", "z"), xrt[:3, 3].tolist())),
    }


def _matrix(value, shape, label):
    result = np.asarray(value, dtype=np.float64)
    if result.shape != shape or not np.all(np.isfinite(result)):
        raise ValueError(f"{label} must be a finite {shape[0]}x{shape[1]} matrix")
    return result


def assemble(calibration_path: Path, refinement_paths: list[Path], repo_root: Path) -> dict:
    calibration = json.loads(calibration_path.read_text())
    calibration_hash = sha256_file(calibration_path)
    if calibration.get("schema_version") != 3:
        raise ValueError("base calibration must use schema_version 3")
    if set(calibration.get("visible_cameras", {})) != {f"camera{i}" for i in range(4)}:
        raise ValueError("base calibration must contain exactly cameras 0 through 3")
    if len(refinement_paths) != 4:
        raise ValueError("exactly four refinement files are required")

    refinements = {}
    for path in refinement_paths:
        refinement = json.loads(path.read_text())
        if refinement.get("format") != REFINEMENT_FORMAT:
            raise ValueError(f"{path}: unexpected refinement format")
        camera = refinement.get("camera")
        if camera not in range(4) or camera in refinements:
            raise ValueError(f"{path}: duplicate or invalid camera {camera}")
        if refinement.get("calibration", {}).get("sha256") != calibration_hash:
            raise ValueError(f"{path}: base calibration hash mismatch")
        validation = refinement.get("validation", {})
        if validation.get("diagnostics", {}).get("status") != "bootstrap_accepted":
            raise ValueError(f"{path}: held-out bootstrap was not accepted")
        if validation.get("required_camera_validation", {}).get("status") != "passed":
            raise ValueError(f"{path}: named-camera validation did not pass")
        refinements[camera] = (path, refinement)
    if set(refinements) != set(range(4)):
        raise ValueError("refinements must cover cameras 0 through 3")

    cameras = []
    for camera in range(4):
        path, refinement = refinements[camera]
        visible = calibration["visible_cameras"][f"camera{camera}"]
        K = _matrix(refinement["refined_K"], (3, 3), f"camera {camera} K")
        D = np.asarray(refinement["fixed_D"], dtype=np.float64).reshape(-1)
        visible_D = np.asarray(visible["D"], dtype=np.float64).reshape(-1)
        if D.shape != (4,) or not np.all(np.isfinite(D)):
            raise ValueError(f"camera {camera} D must contain four finite values")
        if not np.array_equal(D, visible_D):
            raise ValueError(f"camera {camera} distortion differs from the fixed visible calibration")
        if K[0, 0] <= 0 or K[1, 1] <= 0 or not np.allclose(K[2], [0.0, 0.0, 1.0]):
            raise ValueError(f"camera {camera} has invalid intrinsics")
        T_rig_camera = _matrix(
            visible["transform_to_rig_T_rig_camera"], (4, 4), f"camera {camera} transform"
        )
        if not np.allclose(T_rig_camera[3], [0.0, 0.0, 0.0, 1.0], atol=1e-9):
            raise ValueError(f"camera {camera} transform is not homogeneous")
        cameras.append(
            {
                "camera": camera,
                "calibration": {
                    "resolution": {"width": 512, "height": 508},
                    "model": "fisheye_equidistant4",
                    "intrinsics": {
                        "fx": float(K[0, 0]), "fy": float(K[1, 1]),
                        "cx": float(K[0, 2]), "cy": float(K[1, 2]),
                    },
                    "distortion": dict(zip(("k1", "k2", "k3", "k4"), D.tolist())),
                },
                "transform_to_rig_T_rig_camera_opencv": T_rig_camera.tolist(),
                "pose_in_tracking_origin_xrt": xrt_pose_from_opencv_transform(T_rig_camera),
                "refinement": {
                    "path": str(path), "sha256": sha256_file(path),
                    "hand": refinement.get("hand"),
                    "training_rms_px": refinement.get("optimization", {}).get("final_data_rms_px"),
                    "validation": refinement["validation"],
                },
            }
        )

    led_path = repo_root / "src/xrt/drivers/pssense/pssense_led_model.h"
    return {
        "format": FORMAT,
        "runtime_usable": False,
        "status": "provisional_individually_validated",
        "headset_serial": calibration.get("headset_serial"),
        "tracking_origin": "camera0_opencv_converted_to_xrt",
        "base_calibration": {"path": str(calibration_path), "sha256": calibration_hash},
        "cameras": cameras,
        "controller_models": {
            "source": str(led_path.relative_to(repo_root)),
            "sha256": sha256_file(led_path),
            "left": {"led_count": 17, "status": "used_for_calibration"},
            "right": {"led_count": 17, "status": "available_mirrored_model_not_yet_heldout_validated"},
        },
        "note": "Opt-in diagnostics only: peripheral coverage and right-controller optical validation remain incomplete.",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=Path)
    parser.add_argument("refinements", nargs=4, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = assemble(args.calibration, args.refinements, Path(__file__).resolve().parents[1])
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"Assembled cameras 0-3; status={result['status']}; runtime_usable=false")
    print(f"Wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

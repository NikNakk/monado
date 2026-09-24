#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Place the direct native ChArUco rig in the reviewed camera0 physical frame.

OpenCV transforms are camera -> rig: p_rig = T_rig_camera p_camera.
The native rig's origin is its camera0.  The reviewed rig's origin is visible
camera0.  A single SE(3) map is estimated from cameras 0/1 only, minimizing
equal-weight squared orientation chordal error and camera-centre error.  The
native and visible camera axes need not be identical; their disagreement is
reported, not hidden by a per-camera adjustment.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
from pathlib import Path

import numpy as np
from scipy.spatial.transform import Rotation

from psvr2_tracking_calibration_assemble import xrt_pose_from_opencv_transform
from psvr2_tracking_geometry import sha256_file


def degrees(a: np.ndarray, b: np.ndarray) -> float:
    return math.degrees(float(np.linalg.norm(Rotation.from_matrix(a.T @ b).as_rotvec())))


def matrix(value, label: str) -> np.ndarray:
    t = np.asarray(value, dtype=float)
    if t.shape != (4, 4) or not np.isfinite(t).all() or not np.allclose(t[3], [0, 0, 0, 1]):
        raise ValueError(f"{label}: invalid homogeneous transform")
    if not np.allclose(t[:3, :3].T @ t[:3, :3], np.eye(3), atol=1e-5) or np.linalg.det(t[:3, :3]) < 0.999:
        raise ValueError(f"{label}: rotation is not proper")
    return t


def align(direct: dict, reviewed: dict) -> tuple[np.ndarray, dict]:
    native = [matrix(direct["native_relative_rig"]["transforms_T_camera0_camera"][str(i)], f"native camera{i}") for i in range(4)]
    physical = [matrix(reviewed["visible_cameras"][f"camera{i}"]["transform_to_rig_T_rig_camera"], f"reviewed camera{i}") for i in range(2)]
    if not np.allclose(native[0], np.eye(4)) or not np.allclose(physical[0], np.eye(4)):
        raise ValueError("both rigs must use camera0 as their origin")

    # Wahba/orthogonal Procrustes: choose the one common rotation that best
    # matches the two camera orientations.  Translation then minimizes both
    # camera-centre errors.  This is a frame alignment, not a rig refit.
    cross = sum(physical[i][:3, :3] @ native[i][:3, :3].T for i in (0, 1))
    u, _, vh = np.linalg.svd(cross)
    sign = np.diag([1.0, 1.0, np.linalg.det(u @ vh)])
    r = u @ sign @ vh
    t = np.mean([physical[i][:3, 3] - r @ native[i][:3, 3] for i in (0, 1)], axis=0)
    result = np.eye(4)
    result[:3, :3] = r
    result[:3, 3] = t
    before = {
        "translation_vector_difference_mm": float(1000 * np.linalg.norm(native[1][:3, 3] - physical[1][:3, 3])),
        "baseline_length_difference_mm": float(1000 * (np.linalg.norm(native[1][:3, 3]) - np.linalg.norm(physical[1][:3, 3]))),
        "relative_rotation_difference_deg": degrees(physical[1][:3, :3], native[1][:3, :3]),
    }
    after = {}
    for i in (0, 1):
        placed = result @ native[i]
        after[str(i)] = {
            "camera_centre_error_mm": float(1000 * np.linalg.norm(placed[:3, 3] - physical[i][:3, 3])),
            "rotation_difference_deg": degrees(physical[i][:3, :3], placed[:3, :3]),
        }
    return result, {"before_alignment": before, "after_alignment": after,
                    "T_physicalRig_nativeRig_opencv": result.tolist(),
                    "method": "equal-weight two-camera orientation Procrustes; least-squares camera-centre translation",
                    "limitation": "The two lower-camera relative rotations differ; no common rigid transform can make both native image axes equal to visible image axes."}


def build(direct: dict, reviewed: dict, provisional: dict, paths: dict) -> dict:
    if direct.get("format") != "psvr2-mode4-charuco-direct-calibration-v1":
        raise ValueError("expected direct ChArUco calibration v1")
    if provisional.get("format") != "psvr2-mode4-constellation-calibration-v1":
        raise ValueError("expected standard provisional constellation calibration v1")
    if len(provisional.get("cameras", [])) != 4:
        raise ValueError("expected four provisional cameras")
    for i in range(2):
        old = matrix(provisional["cameras"][i]["transform_to_rig_T_rig_camera_opencv"], f"provisional camera{i}")
        trusted = matrix(reviewed["visible_cameras"][f"camera{i}"]["transform_to_rig_T_rig_camera"], f"reviewed camera{i}")
        if not np.allclose(old, trusted, rtol=0, atol=1e-9):
            raise ValueError(f"provisional camera{i} differs from reviewed physical transform")
    transform, diagnostic = align(direct, reviewed)
    result = copy.deepcopy(provisional)
    result["runtime_usable"] = False
    result["status"] = "candidate_direct_charuco_native_rig_pending_independent_validation"
    result["tracking_origin"] = "reviewed_visible_camera0_opencv_converted_to_xrt"
    result["controller_models"]["left"]["status"] = "available_for_independent_validation_only"
    result["controller_models"]["right"]["status"] = "not_used_or_validated"
    for i in range(4):
        camera = result["cameras"][i]
        native = matrix(direct["native_relative_rig"]["transforms_T_camera0_camera"][str(i)], f"native camera{i}")
        placed = transform @ native
        calibration = copy.deepcopy(direct["cameras"][str(i)]["calibration"])
        # The runtime frame has a 512-byte row stride; columns 508..511 are
        # padding. K/D retain the direct fit's native 508x508 pixel coordinates.
        calibration["resolution"] = {"width": 512, "height": 508}
        camera.clear()
        camera.update({"camera": i, "calibration": calibration,
                       "transform_to_rig_T_rig_camera_opencv": placed.tolist(),
                       "pose_in_tracking_origin_xrt": xrt_pose_from_opencv_transform(placed),
                       "direct_charuco_fit": copy.deepcopy(direct["cameras"][str(i)]["fit"]),
                       "native_active_image": {"width": 508, "height": 508,
                                               "padding_columns_ignored": [508, 509, 510, 511]}})
    result["candidate_provenance"] = {
        "direct_charuco": {"path": str(paths["direct"]), "sha256": sha256_file(paths["direct"]),
                           "capture_root": direct["capture_root"], "capture_images": direct.get("capture_images"),
                           "board": direct["board"],
                           "native_rig_fit": {k: direct["native_relative_rig"]["fit"][k] for k in ("rms_px", "median_px", "p95_px")}},
        "reviewed_visible": {"path": str(paths["reviewed"]), "sha256": sha256_file(paths["reviewed"])},
        "provisional": {"path": str(paths["provisional"]), "sha256": sha256_file(paths["provisional"])},
        "alignment": diagnostic,
        "coordinate_convention": "T_rig_camera maps OpenCV camera (+x right,+y down,+z forward) to reviewed visible-camera0 rig; XRT uses (+x right,+y up,-z forward) via C*T*C where C=diag(1,-1,-1,1)",
        "reviewed_slam_hand_eye": {
            "trusted_rotation": reviewed.get("slam", {}).get("trusted_rotation"),
            "trusted_translation": reviewed.get("slam", {}).get("trusted_translation"),
            "T_tracker_visibleRig_diagnostic": reviewed.get("slam", {}).get("best_nominal", {}).get("T_tracker_rig"),
            "applied": False,
            "reason": "Reviewed SLAM-to-visible-rig translation is not trusted; the candidate remains in the existing visible-camera0 tracking origin.",
        },
        "intrinsics_source": "direct native mode-4 ChArUco only; no Sense-derived intrinsics or per-camera pose correction",
    }
    result["note"] = "Offline candidate; lower-camera native/visible axis mismatch is recorded in provenance. Validate independently before any opt-in live test."
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("direct", type=Path)
    parser.add_argument("reviewed", type=Path)
    parser.add_argument("provisional", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    paths = {name: getattr(args, name) for name in ("direct", "reviewed", "provisional")}
    data = {name: json.loads(path.read_text()) for name, path in paths.items()}
    result = build(**data, paths=paths)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["candidate_provenance"]["alignment"], indent=2))
    print(f"wrote {args.output}; runtime_usable=false")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

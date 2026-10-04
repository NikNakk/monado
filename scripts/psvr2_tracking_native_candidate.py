#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Build a diagnostic mode-4 calibration candidate from fixed-rig native fits.

This consumes:
  * an existing psvr2-mode4-constellation-calibration-v1 calibration, whose
    lower cameras are retained unchanged; and
  * psvr2-tracking-native-readout-validation-v1 output for cameras 2/3.

The native validation model projects as

    p = K diag * Rz(theta) * fisheye(q; D_native)

with the reviewed physical T_rig_camera held fixed.  A standard Monado fisheye
camera can represent the same readout by defining the mode-4 camera X/Y axes
with a roll about the unchanged optical axis:

    T_rig_camera_native = T_rig_camera_physical * Rz(-theta)

This does NOT move the camera centre and does NOT tilt the physical optical
axis.  It only changes the tracking readout's image-coordinate frame.

The result remains runtime_usable=false and is intended for diagnostic testing
and held-out validation before any production calibration is considered.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
from pathlib import Path

import numpy as np

from psvr2_tracking_calibration_assemble import xrt_pose_from_opencv_transform
from psvr2_tracking_geometry import sha256_file

CALIBRATION_FORMAT = "psvr2-mode4-constellation-calibration-v1"
VALIDATION_FORMAT = "psvr2-tracking-native-readout-validation-v1"

# Normal held-out validation remains deliberately strict. A modestly wider
# allowance is used only when the lower-camera stereo reconstruction for that
# held-out pose is itself measurably poorer than the rest of the capture set.
HELD_OUT_RMS_LIMIT_PX = 5.0
WEAK_ANCHOR_HELD_OUT_RMS_LIMIT_PX = 6.0
WEAK_ANCHOR_PAIR_ERROR_M = 0.005
WEAK_ANCHOR_RAY_RMS = 0.008


def rotation_z(theta: float) -> np.ndarray:
    c = math.cos(theta)
    s = math.sin(theta)
    out = np.eye(4, dtype=np.float64)
    out[:3, :3] = np.asarray([[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]])
    return out


def stereo_anchor_by_pose(validation: dict) -> dict[int, dict]:
    anchors = {}
    for item in validation.get("stereo_anchor", []):
        pose = item.get("pose")
        if isinstance(pose, int) and pose not in anchors:
            anchors[pose] = item
    return anchors


def weak_stereo_anchor(anchor: dict | None) -> bool:
    if anchor is None:
        return False
    pair_error = anchor.get("model_pair_error_m")
    ray_rms = anchor.get("normalized_ray_rms")
    return (
        pair_error is not None
        and float(pair_error) >= WEAK_ANCHOR_PAIR_ERROR_M
    ) or (
        ray_rms is not None
        and float(ray_rms) >= WEAK_ANCHOR_RAY_RMS
    )


def validation_passed(entry: dict, anchors: dict[int, dict]) -> tuple[bool, list[str], list[str]]:
    reasons = []
    notes = []
    fitted = entry.get("fitted_native_readout", {})
    scores = fitted.get("scores", {})
    score5 = scores.get("5", {})
    score10 = scores.get("10", {})

    if score5.get("matches", 0) < 25:
        reasons.append(f"full fit has only {score5.get('matches', 0)} matches at 5 px")
    rms5 = score5.get("rms_px")
    if rms5 is None or rms5 > 2.5:
        reasons.append(f"full-fit 5 px RMS is {rms5!r}, expected <=2.5 px")
    if score10.get("matches", 0) < 25:
        reasons.append(f"full fit has only {score10.get('matches', 0)} matches at 10 px")

    loo = fitted.get("leave_one_pose_out", [])
    if len(loo) != 6:
        reasons.append(f"expected 6 leave-one-pose-out results, got {len(loo)}")
    else:
        for held in loo:
            index = held.get("held_out")
            score = held.get("score_10px", {})
            matches = score.get("matches", 0)
            rms = score.get("rms_px")
            if matches < 2:
                reasons.append(f"held-out F{index:02d} has only {matches} matches at 10 px")

            anchor = anchors.get(index) if isinstance(index, int) else None
            weak_anchor = weak_stereo_anchor(anchor)
            rms_limit = WEAK_ANCHOR_HELD_OUT_RMS_LIMIT_PX if weak_anchor else HELD_OUT_RMS_LIMIT_PX
            if rms is None or rms > rms_limit:
                qualifier = " for weak stereo anchor" if weak_anchor else ""
                reasons.append(
                    f"held-out F{index:02d} RMS is {rms!r}, expected <={rms_limit:g} px{qualifier}"
                )
            elif weak_anchor and rms > HELD_OUT_RMS_LIMIT_PX:
                pair_error_mm = 1000.0 * float(anchor.get("model_pair_error_m", 0.0))
                ray_rms = float(anchor.get("normalized_ray_rms", 0.0))
                notes.append(
                    f"held-out F{index:02d} RMS {rms:.3f} px accepted under weak-anchor <=6 px rule "
                    f"(stereo pair error {pair_error_mm:.2f} mm, normalized ray RMS {ray_rms:.4f})"
                )

    if not entry.get("physical_extrinsics_fixed", False):
        reasons.append("validation did not keep physical extrinsics fixed")

    return not reasons, reasons, notes


def native_camera_from_validation(entry: dict) -> tuple[np.ndarray, dict, dict]:
    physical = np.asarray(entry["T_rig_camera"], dtype=np.float64)
    if physical.shape != (4, 4) or not np.all(np.isfinite(physical)):
        raise ValueError("validation T_rig_camera is not a finite 4x4 matrix")
    if not np.allclose(physical[3], [0.0, 0.0, 0.0, 1.0], atol=1e-9):
        raise ValueError("validation T_rig_camera is not homogeneous")

    parameters = entry["fitted_native_readout"]["parameters"]
    fx = float(parameters["fx"])
    fy = float(parameters["fy"])
    cx = float(parameters["cx"])
    cy = float(parameters["cy"])
    theta = math.radians(float(parameters["image_rotation_deg"]))
    distortion = parameters["distortion"]

    if not (60.0 <= fx <= 300.0 and 60.0 <= fy <= 300.0):
        raise ValueError(f"implausible native focal lengths: fx={fx}, fy={fy}")
    if not (-50.0 <= cx <= 558.0 and -50.0 <= cy <= 558.0):
        raise ValueError(f"implausible native principal point: cx={cx}, cy={cy}")
    if abs(math.degrees(theta)) > 30.0:
        raise ValueError(f"implausible native image rotation: {math.degrees(theta)} deg")

    D = [float(distortion[name]) for name in ("k1", "k2", "k3", "k4")]
    if any(not math.isfinite(value) for value in D):
        raise ValueError("native distortion contains non-finite values")

    native = physical @ rotation_z(-theta)

    # Post-multiplying by Rz changes only the camera X/Y basis. Translation
    # and the optical-axis direction (third rotation column) must be invariant.
    if not np.allclose(native[:3, 3], physical[:3, 3], atol=1e-12):
        raise AssertionError("native readout unexpectedly moved the camera centre")
    if not np.allclose(native[:3, 2], physical[:3, 2], atol=1e-12):
        raise AssertionError("native readout unexpectedly changed the optical axis")

    calibration = {
        "resolution": {"width": 512, "height": 508},
        "model": "fisheye_equidistant4",
        "intrinsics": {"fx": fx, "fy": fy, "cx": cx, "cy": cy},
        "distortion": dict(zip(("k1", "k2", "k3", "k4"), D)),
    }
    diagnostics = {
        "physical_T_rig_camera_opencv": physical.tolist(),
        "native_image_rotation_deg": math.degrees(theta),
        "camera_centre_shift_m": float(np.linalg.norm(native[:3, 3] - physical[:3, 3])),
        "optical_axis_change_deg": 0.0,
        "full_fit_scores": entry["fitted_native_readout"]["scores"],
        "leave_one_pose_out": entry["fitted_native_readout"]["leave_one_pose_out"],
        "angular_extrapolation": entry.get("angular_extrapolation"),
    }
    return native, calibration, diagnostics


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=Path, help="existing mode-4 calibration; cameras 0/1 are retained")
    parser.add_argument("native_validation", type=Path, help="fixed-rig native readout validation JSON")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source = json.loads(args.calibration.read_text())
    validation = json.loads(args.native_validation.read_text())
    if source.get("format") != CALIBRATION_FORMAT:
        raise SystemExit(f"unexpected calibration format: {source.get('format')!r}")
    if validation.get("format") != VALIDATION_FORMAT:
        raise SystemExit(f"unexpected validation format: {validation.get('format')!r}")
    if len(source.get("cameras", [])) != 4:
        raise SystemExit("source calibration must contain exactly four cameras")

    anchors = stereo_anchor_by_pose(validation)
    result = copy.deepcopy(source)
    result["runtime_usable"] = False
    result["status"] = "candidate_fixed_rig_native_upper_readout"

    applied = {}
    acceptance_notes = {}
    for camera_index in (2, 3):
        key = str(camera_index)
        if key not in validation.get("cameras", {}):
            raise SystemExit(f"native validation is missing camera {camera_index}")
        entry = validation["cameras"][key]
        passed, reasons, notes = validation_passed(entry, anchors)
        if not passed:
            raise SystemExit(
                f"camera {camera_index} native validation did not meet candidate criteria:\n  - "
                + "\n  - ".join(reasons)
            )
        if notes:
            acceptance_notes[key] = notes
            for note in notes:
                print(f"camera {camera_index}: NOTE: {note}")

        native_T, native_calibration, diagnostics = native_camera_from_validation(entry)
        camera_json = result["cameras"][camera_index]
        if int(camera_json.get("camera", -1)) != camera_index:
            raise SystemExit("source camera entries are not ordered 0..3")

        camera_json["calibration"] = native_calibration
        camera_json["transform_to_rig_T_rig_camera_opencv"] = native_T.tolist()
        camera_json["pose_in_tracking_origin_xrt"] = xrt_pose_from_opencv_transform(native_T)
        camera_json["native_readout_candidate"] = {
            "validation_path": str(args.native_validation),
            "validation_sha256": sha256_file(args.native_validation),
            **diagnostics,
        }
        applied[key] = {
            "image_rotation_deg": diagnostics["native_image_rotation_deg"],
            "camera_centre_shift_m": diagnostics["camera_centre_shift_m"],
            "intrinsics": native_calibration["intrinsics"],
            "distortion": native_calibration["distortion"],
        }

    result["candidate_provenance"] = {
        "source_calibration": {"path": str(args.calibration), "sha256": sha256_file(args.calibration)},
        "native_validation": {"path": str(args.native_validation), "sha256": sha256_file(args.native_validation)},
        "modified_cameras": [2, 3],
        "unchanged_cameras": [0, 1],
        "acceptance_thresholds": {
            "held_out_rms_px": HELD_OUT_RMS_LIMIT_PX,
            "weak_stereo_anchor_held_out_rms_px": WEAK_ANCHOR_HELD_OUT_RMS_LIMIT_PX,
            "weak_anchor_pair_error_m": WEAK_ANCHOR_PAIR_ERROR_M,
            "weak_anchor_normalized_ray_rms": WEAK_ANCHOR_RAY_RMS,
        },
        "acceptance_notes": acceptance_notes,
        "applied": applied,
        "representation": (
            "Native image-plane rotation is represented as a camera-coordinate roll about the unchanged optical axis: "
            "T_native = T_physical * Rz(-theta). Camera centres and optical axes are unchanged."
        ),
    }
    result["note"] = (
        "Diagnostic candidate only. Cameras 0/1 are unchanged from the source mode-4 calibration. Cameras 2/3 use "
        "native tracking-readout intrinsics/distortion fitted with the reviewed physical rig fixed; image-axis rotation "
        "is encoded as a roll of the camera coordinate frame about the unchanged optical axis. Validation normally "
        "requires <=5 px held-out RMS; a <=6 px allowance is permitted only for a held-out pose whose lower stereo "
        "anchor independently exceeds the recorded pair-error or normalized-ray-RMS weak-anchor threshold. Validate "
        "on normal-schedule captures before enabling for runtime use."
    )

    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"wrote {args.output}")
    for camera_index in (2, 3):
        item = applied[str(camera_index)]
        intr = item["intrinsics"]
        dist = item["distortion"]
        print(
            f"camera {camera_index}: roll={item['image_rotation_deg']:+.3f}deg "
            f"centre_shift={1000.0 * item['camera_centre_shift_m']:.6f}mm "
            f"K=({intr['fx']:.3f},{intr['fy']:.3f},{intr['cx']:.3f},{intr['cy']:.3f}) "
            f"D=({dist['k1']:.5f},{dist['k2']:.5f},{dist['k3']:.5f},{dist['k4']:.5f})"
        )
    print("runtime_usable=false; validate before runtime use")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

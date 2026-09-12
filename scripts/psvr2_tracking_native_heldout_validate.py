#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Compare provisional and native mode-4 upper-camera models on held-out poses.

This script performs no fitting. Cameras 0/1, which must be identical in the
source and candidate calibrations, are used as a metric stereo anchor for each
held-out V* pose. The resulting unlabeled rig-space Sense LED cloud is then
projected into cameras 2/3 using both calibrations and matched one-to-one to the
observed upper-camera blobs.

The result is therefore a direct before/after test of the upper-camera optical
model on data that did not contribute to the native readout fit.
"""
from __future__ import annotations

import argparse
import itertools
import json
from pathlib import Path

import numpy as np

from psvr2_tracking_geometry import sha256_file
from psvr2_tracking_joint_rig_refine import (
    load_cameras,
    load_led_positions,
    merged_blinks,
    score_camera,
    stereo_cloud,
)

LIMITS = (5.0, 10.0, 20.0, 40.0)
NORMAL_RMS_LIMIT_PX = 5.0
WEAK_ANCHOR_RMS_LIMIT_PX = 6.0
WEAK_ANCHOR_PAIR_ERROR_M = 0.005
WEAK_ANCHOR_RAY_RMS = 0.008


def validation_dirs(root: Path) -> list[Path]:
    result = sorted(path for path in root.iterdir() if path.is_dir() and path.name.startswith("V"))
    if not result:
        raise ValueError(f"{root}: no V* validation-pose directories found")
    return result


def cameras_equal(a, b) -> bool:
    return (
        np.allclose(a.K, b.K, rtol=0.0, atol=1e-12)
        and np.allclose(a.D, b.D, rtol=0.0, atol=1e-12)
        and np.allclose(a.T, b.T, rtol=0.0, atol=1e-12)
    )


def weak_anchor(anchor: dict) -> bool:
    return (
        float(anchor["model_pair_error_m"]) >= WEAK_ANCHOR_PAIR_ERROR_M
        or float(anchor["normalized_ray_rms"]) >= WEAK_ANCHOR_RAY_RMS
    )


def summarize_score(camera, clouds, observations, names) -> dict:
    result = {}
    for limit in LIMITS:
        matches, rms, rows = score_camera(camera, clouds, observations, limit=limit)
        per_pose = []
        errors = []
        for name, row in zip(names, rows):
            row_errors = [float(item[0]) for item in row]
            errors.extend(row_errors)
            per_pose.append(
                {
                    "pose": name,
                    "matches": len(row),
                    "rms_px": None
                    if not row_errors
                    else float(np.sqrt(np.mean(np.square(row_errors)))),
                    "median_px": None if not row_errors else float(np.median(row_errors)),
                    "errors_px": row_errors,
                }
            )
        result[str(int(limit))] = {
            "matches": matches,
            "rms_px": None if not np.isfinite(rms) else float(rms),
            "median_px": None if not errors else float(np.median(errors)),
            "p95_px": None if not errors else float(np.percentile(errors, 95)),
            "per_pose": per_pose,
        }
    return result


def candidate_assessment(scores: dict, anchors: list[dict], possible: list[int]) -> dict:
    score10 = scores["10"]
    reasons = []
    notes = []
    minimum_total = int(np.ceil(0.75 * sum(possible)))
    if score10["matches"] < minimum_total:
        reasons.append(
            f"only {score10['matches']} held-out matches at 10 px; expected at least {minimum_total} "
            f"of {sum(possible)} geometrically matchable points"
        )
    if score10["rms_px"] is None or score10["rms_px"] > NORMAL_RMS_LIMIT_PX:
        reasons.append(f"aggregate held-out RMS is {score10['rms_px']!r}, expected <=5 px")

    for index, (row, anchor, max_matches) in enumerate(zip(score10["per_pose"], anchors, possible)):
        required_matches = min(2, max_matches)
        if row["matches"] < required_matches:
            reasons.append(
                f"{row['pose']} has {row['matches']} matches at 10 px; expected at least {required_matches}"
            )
        if row["matches"] == 0:
            continue
        is_weak = weak_anchor(anchor)
        limit = WEAK_ANCHOR_RMS_LIMIT_PX if is_weak else NORMAL_RMS_LIMIT_PX
        if row["rms_px"] is None or row["rms_px"] > limit:
            qualifier = " for weak stereo anchor" if is_weak else ""
            reasons.append(
                f"{row['pose']} RMS is {row['rms_px']!r}, expected <={limit:g} px{qualifier}"
            )
        elif is_weak and row["rms_px"] > NORMAL_RMS_LIMIT_PX:
            notes.append(
                f"{row['pose']} RMS {row['rms_px']:.3f} px accepted under weak-anchor <=6 px rule"
            )

    return {
        "passed": not reasons,
        "reasons": reasons,
        "notes": notes,
        "minimum_total_matches_at_10px": minimum_total,
        "possible_matches": int(sum(possible)),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_calibration", type=Path)
    parser.add_argument("candidate_calibration", type=Path)
    parser.add_argument("validation_root", type=Path)
    parser.add_argument("--hand", choices=("left", "right"), default="left")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    source_data = json.loads(args.source_calibration.read_text())
    candidate_data = json.loads(args.candidate_calibration.read_text())
    source = load_cameras(source_data)
    candidate = load_cameras(candidate_data)
    for camera_index in (0, 1):
        if not cameras_equal(source[camera_index], candidate[camera_index]):
            raise SystemExit(f"candidate unexpectedly changes stereo-anchor camera {camera_index}")

    positions = load_led_positions(Path(__file__).resolve().parents[1], args.hand)
    model_distances = np.asarray(
        [np.linalg.norm(positions[i] - positions[j]) for i, j in itertools.combinations(range(len(positions)), 2)]
    )

    directories = validation_dirs(args.validation_root)
    names = [path.name for path in directories]
    observations = [[merged_blinks(directory, camera) for camera in range(4)] for directory in directories]

    clouds = []
    anchors = []
    print("blob counts:")
    for name, row in zip(names, observations):
        print(f"  {name}: " + "/".join(str(len(points)) for points in row))
        solution = stereo_cloud(source[0], source[1], row[0], row[1], model_distances)
        cloud = solution[1]
        anchor = {
            "pose": name,
            "point_count": len(cloud),
            "diameter_m": float(solution[2]),
            "model_pair_error_m": float(solution[3]),
            "normalized_ray_rms": float(solution[4]),
        }
        clouds.append(cloud)
        anchors.append(anchor)
        print(
            f"  {name}: stereo {len(cloud)} points, diameter={1000.0*solution[2]:.1f}mm, "
            f"pair-error={1000.0*solution[3]:.2f}mm, ray-rms={solution[4]:.4f}"
        )

    cameras = {}
    overall_pass = True
    for camera_index in (2, 3):
        camera_observations = [row[camera_index] for row in observations]
        old_scores = summarize_score(source[camera_index], clouds, camera_observations, names)
        new_scores = summarize_score(candidate[camera_index], clouds, camera_observations, names)
        possible = [min(len(cloud), len(observed)) for cloud, observed in zip(clouds, camera_observations)]
        assessment = candidate_assessment(new_scores, anchors, possible)
        overall_pass &= assessment["passed"]
        cameras[str(camera_index)] = {
            "camera": camera_index,
            "possible_matches_per_pose": possible,
            "source": old_scores,
            "candidate": new_scores,
            "assessment": assessment,
        }
        old10 = old_scores["10"]
        new10 = new_scores["10"]
        print(
            f"camera {camera_index}: 10px source={old10['matches']}/{old10['rms_px']} "
            f"candidate={new10['matches']}/{new10['rms_px']} pass={assessment['passed']}"
        )
        for old_row, new_row in zip(old10["per_pose"], new10["per_pose"]):
            print(
                f"  {new_row['pose']}: source {old_row['matches']}/{old_row['rms_px']} -> "
                f"candidate {new_row['matches']}/{new_row['rms_px']}"
            )
        for note in assessment["notes"]:
            print(f"  NOTE: {note}")
        for reason in assessment["reasons"]:
            print(f"  FAIL: {reason}")

    result = {
        "format": "psvr2-tracking-native-heldout-validation-v1",
        "runtime_usable": False,
        "status": "passed" if overall_pass else "failed",
        "source_calibration": {
            "path": str(args.source_calibration),
            "sha256": sha256_file(args.source_calibration),
        },
        "candidate_calibration": {
            "path": str(args.candidate_calibration),
            "sha256": sha256_file(args.candidate_calibration),
        },
        "validation_root": str(args.validation_root),
        "hand": args.hand,
        "poses": anchors,
        "cameras": cameras,
        "criteria": {
            "aggregate_10px_match_fraction": 0.75,
            "aggregate_rms_px": NORMAL_RMS_LIMIT_PX,
            "per_pose_minimum_matches": 2,
            "per_pose_rms_px": NORMAL_RMS_LIMIT_PX,
            "weak_anchor_per_pose_rms_px": WEAK_ANCHOR_RMS_LIMIT_PX,
            "weak_anchor_pair_error_m": WEAK_ANCHOR_PAIR_ERROR_M,
            "weak_anchor_normalized_ray_rms": WEAK_ANCHOR_RAY_RMS,
        },
        "note": (
            "Strictly held-out comparison: no model parameter is fitted from V* data. Cameras 0/1 must be identical "
            "between source and candidate and are used only to reconstruct the validation point clouds."
        ),
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"{result['status'].upper()}: wrote {args.output}")
    return 0 if overall_pass else 2


if __name__ == "__main__":
    raise SystemExit(main())

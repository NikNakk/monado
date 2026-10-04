#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Independently score an aligned direct ChArUco rig on held-out Sense poses.

The native camera0/1 stereo pair reconstructs an unlabeled 3-D cloud using
only its own observations and the published Sense LED pair-distance spectrum.
No camera parameter or cloud is fitted against cameras 2/3.  Sparse three-LED
anchors are retained and identified separately in the diagnostics.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import itertools
import json
from pathlib import Path

import numpy as np

from psvr2_tracking_geometry import sha256_file
from psvr2_tracking_joint_rig_refine import (
    load_cameras, load_led_positions, merged_blinks, score_camera, stereo_cloud,
)
from psvr2_tracking_native_heldout_validate import validation_dirs

LIMITS = (5, 10, 20)


def capture_digest(root: Path) -> tuple[str, int]:
    digest = hashlib.sha256()
    files = sorted(root.rglob("mode-04-size-*-set-*-example-*-plane*.pgm"))
    for path in files:
        digest.update(path.relative_to(root).as_posix().encode() + b"\0")
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    return digest.hexdigest(), len(files)


def stereo_anchor(cam0, cam1, obs0, obs1, model_distances):
    maximum = min(len(obs0), len(obs1))
    if maximum < 3:
        raise RuntimeError("fewer than three lower-camera blobs")
    minimum = max(3, maximum - 2)
    for count in range(maximum, minimum - 1, -1):
        options = []
        for ids0 in itertools.combinations(range(len(obs0)), count):
            for ids1 in itertools.combinations(range(len(obs1)), count):
                try:
                    solution = stereo_cloud(cam0, cam1, obs0[list(ids0)], obs1[list(ids1)], model_distances)
                except RuntimeError:
                    continue
                options.append((float(solution[0]), ids0, ids1, solution))
        if options:
            objective, ids0, ids1, solution = min(options, key=lambda item: item[0])
            return solution, {"used_blob_count": count, "input_blob_counts": [len(obs0), len(obs1)],
                              "camera0_indices": list(ids0), "camera1_indices": list(ids1),
                              "lower_stereo_objective": objective,
                              "sparse_three_point_anchor": count == 3}
    raise RuntimeError("no physically plausible lower-camera stereo assignment")


def summarize(camera, clouds, observations, names):
    scores = {}
    for limit in LIMITS:
        count, _, matches = score_camera(camera, clouds, observations, limit=limit)
        rows = []
        errors = []
        for name, match_row in zip(names, matches):
            values = [float(item[0]) for item in match_row]
            errors.extend(values)
            rows.append({"pose": name, "matches": len(values),
                         "rms_px": float(np.sqrt(np.mean(np.square(values)))) if values else None,
                         "errors_px": values})
        scores[str(limit)] = {"matches": count,
                              "rms_px": float(np.sqrt(np.mean(np.square(errors)))) if errors else None,
                              "median_px": float(np.median(errors)) if errors else None,
                              "p95_px": float(np.percentile(errors, 95)) if errors else None,
                              "per_pose": rows}
    return scores


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("validation_root", type=Path)
    parser.add_argument("--source", type=Path, help="prior standard calibration for upper-camera comparison only")
    parser.add_argument("--hand", choices=("left", "right"), default="left")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--validated-output", type=Path,
                        help="write a standard opt-in live-test candidate only if validation passes")
    args = parser.parse_args()
    data = json.loads(args.candidate.read_text())
    cameras = load_cameras(data)
    source = load_cameras(json.loads(args.source.read_text())) if args.source else None
    leds = load_led_positions(Path(__file__).resolve().parents[1], args.hand)
    distances = np.array([np.linalg.norm(leds[i] - leds[j]) for i, j in itertools.combinations(range(len(leds)), 2)])
    directories = validation_dirs(args.validation_root)
    names, clouds, observations, anchors, skipped = [], [], [], [], []
    for directory in directories:
        observed = [merged_blinks(directory, i) for i in range(4)]
        try:
            solution, selection = stereo_anchor(cameras[0], cameras[1], observed[0], observed[1], distances)
        except RuntimeError as exc:
            skipped.append({"pose": directory.name, "reason": str(exc), "blob_counts": [len(x) for x in observed]})
            continue
        names.append(directory.name)
        clouds.append(solution[1])
        observations.append(observed)
        anchors.append({"pose": directory.name, "point_count": len(solution[1]),
                        "blob_counts": [len(x) for x in observed], "diameter_m": float(solution[2]),
                        "model_pair_error_m": float(solution[3]), "normalized_ray_rms": float(solution[4]),
                        "selection": selection})
    reports = {}
    reasons = []
    if len(names) != len(directories):
        reasons.append(f"only {len(names)}/{len(directories)} validation poses have lower stereo anchors")
    if len(names) < 4:
        reasons.append("fewer than four usable validation poses")
    for i in range(4):
        observed = [row[i] for row in observations]
        candidate = summarize(cameras[i], clouds, observed, names)
        possible = [min(len(cloud), len(obs)) for cloud, obs in zip(clouds, observed)]
        reports[str(i)] = {"possible_matches_per_pose": possible, "candidate": candidate}
        if source is not None and i >= 2:
            reports[str(i)]["source"] = summarize(source[i], clouds, observed, names)
        ten = candidate["10"]
        if i >= 2:
            if ten["matches"] < int(np.ceil(0.75 * sum(possible))):
                reasons.append(f"cam{i}: insufficient 10 px matches ({ten['matches']}/{sum(possible)})")
            if ten["rms_px"] is None or ten["rms_px"] > 5.0:
                reasons.append(f"cam{i}: 10 px RMS exceeds 5 px ({ten['rms_px']})")
            for row, max_count in zip(ten["per_pose"], possible):
                if row["matches"] < min(2, max_count):
                    reasons.append(f"cam{i} {row['pose']}: fewer than two 10 px matches")
                if row["rms_px"] is None or row["rms_px"] > 5.0:
                    reasons.append(f"cam{i} {row['pose']}: 10 px RMS exceeds 5 px ({row['rms_px']})")
    image_hash, image_count = capture_digest(args.validation_root)
    report = {"format": "psvr2-mode4-charuco-sense-heldout-validation-v1", "runtime_usable": False,
              "status": "passed" if not reasons else "failed", "reasons": reasons,
              "candidate": {"path": str(args.candidate), "sha256": sha256_file(args.candidate)},
              "source": None if args.source is None else {"path": str(args.source), "sha256": sha256_file(args.source)},
              "validation": {"root": str(args.validation_root), "hand": args.hand,
                             "image_count": image_count, "images_sha256": image_hash},
              "anchors": anchors, "skipped_poses": skipped, "cameras": reports,
              "criteria": {"upper_min_10px_match_fraction": 0.75, "upper_max_10px_rms_px": 5.0,
                           "upper_min_per_pose_10px_matches": 2, "upper_max_per_pose_10px_rms_px": 5.0},
              "note": "No candidate intrinsic, distortion, or camera extrinsic is fitted from Sense data. Native lower stereo alone reconstructs each cloud; sparse three-point anchors are explicitly flagged."}
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    if args.validated_output is not None and not reasons:
        validated = copy.deepcopy(data)
        validated["status"] = "validated_offline_opt_in_live_test_candidate"
        validated["runtime_usable"] = False
        validated["controller_models"][args.hand]["status"] = "independent_offline_validation_only"
        validated["candidate_provenance"]["independent_sense_validation"] = {
            "path": str(args.output), "sha256": sha256_file(args.output),
            "status": "passed", "poses": names,
        }
        validated["note"] = (
            "Passed six held-out left-Sense offline poses with direct native ChArUco K/D and a single "
            "lower-stereo-to-reviewed-frame alignment. Opt-in monado-cli live test candidate only; "
            "absolute HMD-frame accuracy and right-controller performance remain unverified."
        )
        args.validated_output.write_text(json.dumps(validated, indent=2) + "\n")
        print(f"wrote opt-in live-test candidate {args.validated_output}; runtime_usable=false")
    for i in range(4):
        print(f"cam{i}:")
        for limit in LIMITS:
            row = reports[str(i)]["candidate"][str(limit)]
            print(f"  {limit}px {row['matches']} matches RMS {row['rms_px']}")
        for row in reports[str(i)]["candidate"]["10"]["per_pose"]:
            print(f"  {row['pose']}: {row['matches']} matches RMS {row['rms_px']}")
    print(f"{report['status'].upper()}: wrote {args.output}")
    for reason in reasons:
        print(f"  {reason}")
    return 0 if not reasons else 2


if __name__ == "__main__":
    raise SystemExit(main())

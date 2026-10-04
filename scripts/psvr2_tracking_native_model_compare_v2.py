#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Corrected runner for psvr2_tracking_native_model_compare.

The comparison model itself uses the compact mode-4 Camera dataclass, while the
KB2 bootstrap inherited from psvr2_tracking_native_readout_validate requires the
reviewed visible/bridge CameraModel (including H_mode3_to_mode4).  This runner
keeps those roles separate: lower stereo, KB4 fitting and held-out scoring use
the supplied provisional mode-4 calibration; only the KB2 bootstrap uses the
reviewed base calibration recorded by that artifact (or --base-calibration).
"""
from __future__ import annotations

import argparse
import itertools
import json
from pathlib import Path

import numpy as np

import psvr2_tracking_native_model_compare as compare
from psvr2_tracking_geometry import load_camera_models, sha256_file


def resolve_base_calibration(args, calibration_data: dict) -> Path:
    if args.base_calibration is not None:
        path = args.base_calibration
    else:
        recorded = calibration_data.get("base_calibration", {}).get("path")
        if not recorded:
            raise SystemExit(
                "mode-4 calibration does not record base_calibration.path; "
                "pass --base-calibration /path/to/reviewed-visible-calibration.json"
            )
        path = Path(recorded)
    if not path.exists():
        raise SystemExit(
            f"reviewed base calibration not found at {path}; "
            "pass --base-calibration explicitly"
        )
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("calibration", type=Path, help="provisional mode-4 calibration")
    parser.add_argument("training_root", type=Path)
    parser.add_argument("validation_root", type=Path)
    parser.add_argument("--base-calibration", type=Path, default=None)
    parser.add_argument("--hand", choices=("left", "right"), default="left")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    calibration_data = json.loads(args.calibration.read_text())
    cameras = compare.load_cameras(calibration_data)

    base_path = resolve_base_calibration(args, calibration_data)
    bridge_cameras = load_camera_models(json.loads(base_path.read_text()))
    if len(bridge_cameras) != 4:
        raise SystemExit("reviewed base calibration did not yield four cameras")

    # The native bootstrap and the model-family fits must refer to the same
    # physical rig. The bridge CameraModel may have different image geometry,
    # but its physical T_rig_camera should equal the provisional mode-4 T.
    for index, (camera, bridge) in enumerate(zip(cameras, bridge_cameras)):
        if not np.allclose(camera.T, bridge.T_rig_camera, rtol=0.0, atol=1e-8):
            delta_mm = 1000.0 * float(np.linalg.norm(camera.T[:3, 3] - bridge.T_rig_camera[:3, 3]))
            raise SystemExit(
                f"camera {index} physical transform differs between provisional and reviewed base "
                f"calibrations (centre delta {delta_mm:.3f} mm)"
            )

    positions = compare.load_led_positions(Path(__file__).resolve().parents[1], args.hand)
    model_distances = np.asarray(
        [np.linalg.norm(positions[i] - positions[j]) for i, j in itertools.combinations(range(len(positions)), 2)]
    )

    train_clouds, train_rows, train_anchors = compare.training_data(args.training_root, cameras, model_distances)
    val_names, val_clouds, val_rows, val_anchors, skipped = compare.heldout_data(
        args.validation_root, cameras, model_distances
    )
    if len(val_clouds) < 4:
        raise SystemExit(f"only {len(val_clouds)} usable held-out poses; expected at least 4")

    output_cameras = {}
    for camera_index in (2, 3):
        camera = cameras[camera_index]
        bridge_camera = bridge_cameras[camera_index]
        train_obs = [row[camera_index] for row in train_rows]
        val_obs = [row[camera_index] for row in val_rows]

        # fit_native needs H_mode3_to_mode4 and T_rig_camera, so use the
        # reviewed bridge CameraModel here. The returned seven parameters are
        # ordinary native-readout parameters and are then consumed by the
        # compact mode-4 comparison model below.
        kb2, kb2_history = compare.fit_native(bridge_camera, train_clouds, train_obs)
        starts = {family: compare.extend_parameters(kb2, family) for family in compare.FAMILIES}
        fits = {"kb2_fixed": (kb2, kb2_history)}
        for family in compare.FAMILIES[1:]:
            fits[family] = compare.fit_family(camera, starts[family], family, train_clouds, train_obs)

        family_results = {}
        possible = [min(len(cloud), len(observed)) for cloud, observed in zip(val_clouds, val_obs)]
        for family in compare.FAMILIES:
            parameters, history = fits[family]
            training_scores = {
                str(int(limit)): compare.score_model(camera, parameters, family, train_clouds, train_obs, limit)
                for limit in compare.SCORE_LIMITS
            }
            heldout_scores = {
                str(int(limit)): compare.named_score(
                    compare.score_model(camera, parameters, family, val_clouds, val_obs, limit), val_names
                )
                for limit in compare.SCORE_LIMITS
            }
            assessment = compare.candidate_assessment(heldout_scores, val_anchors, possible)
            family_results[family] = {
                "parameters": compare.parameters_json(parameters, family),
                "optimization": history,
                "training": training_scores,
                "heldout": heldout_scores,
                "assessment": assessment,
            }

            train10 = training_scores["10"]
            held10 = heldout_scores["10"]
            print(
                f"camera {camera_index} {family}: "
                f"training={train10['matches']}/{train10['rms_px']} "
                f"heldout={held10['matches']}/{held10['rms_px']} "
                f"pass={assessment['passed']}"
            )
            params = family_results[family]["parameters"]
            if family in ("kb4_tilt", "kb4_pose"):
                print(
                    f"  tilt={params['optical_axis_tilt_deg']} deg "
                    f"translation={params['camera_local_translation_mm']} mm"
                )

        output_cameras[str(camera_index)] = {"camera": camera_index, "families": family_results}

    result = {
        "format": "psvr2-tracking-native-model-comparison-v2",
        "runtime_usable": False,
        "calibration": {"path": str(args.calibration), "sha256": sha256_file(args.calibration)},
        "base_calibration": {"path": str(base_path), "sha256": sha256_file(base_path)},
        "training_root": str(args.training_root),
        "validation_root": str(args.validation_root),
        "hand": args.hand,
        "training_anchors": train_anchors,
        "heldout_anchors": val_anchors,
        "skipped_heldout": skipped,
        "cameras": output_cameras,
        "note": (
            "F* poses only are used for fitting. V* poses are reconstructed from unchanged cameras 0/1 and are used "
            "only for held-out scoring. The reviewed visible/bridge calibration is used only to initialise the KB2 "
            "native-readout fit; all family fitting/scoring uses the provisional mode-4 physical rig. kb4_tilt changes "
            "optical-axis direction but not camera centre; kb4_pose permits at most 20 mm camera-local translation and "
            "12 degree x/y tilt. None of these diagnostic models is runtime usable."
        ),
    }
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"wrote {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

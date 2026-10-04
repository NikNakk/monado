#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Turn a direct native mode-4 ChArUco solve into a psvr2-constellation calibration, origin native camera0.

psvr2_tracking_charuco_align.py places the direct rig in the reviewed visible-camera0 frame, which needs the
reviewed visible calibration. This variant keeps the direct solve's own origin (native mode-4 camera0). Every
camera-to-camera transform, and so every controller pose relative to the cameras, is identical to the aligned
candidate; only the tracking origin's placement relative to the headset differs. That placement was untrusted
anyway (the reviewed SLAM hand-eye translation was never trusted).

The provisional calibration is used only as a JSON template for the fields the CLI and tools expect
(format, controller_models); its camera entries are replaced entirely.

    psvr2_tracking_charuco_native_origin.py DIRECT.json PROVISIONAL.json --output CANDIDATE.json
"""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path

import numpy as np

from psvr2_tracking_calibration_assemble import xrt_pose_from_opencv_transform
from psvr2_tracking_charuco_align import matrix
from psvr2_tracking_geometry import sha256_file


def build(direct: dict, provisional: dict, paths: dict) -> dict:
    if direct.get("format") != "psvr2-mode4-charuco-direct-calibration-v1":
        raise ValueError("expected direct ChArUco calibration v1")
    if provisional.get("format") != "psvr2-mode4-constellation-calibration-v1":
        raise ValueError("expected provisional constellation calibration v1 as a template")

    result = copy.deepcopy(provisional)
    result.pop("base_calibration", None)
    result["runtime_usable"] = False
    result["status"] = "candidate_direct_charuco_native_origin_pending_live_validation"
    result["tracking_origin"] = "native_mode4_camera0_opencv_converted_to_xrt"
    if "controller_models" in result:
        result["controller_models"].setdefault("left", {})["status"] = "available_for_independent_validation_only"
        result["controller_models"].setdefault("right", {})["status"] = "not_used_or_validated"

    cameras = []
    for i in range(4):
        native = matrix(direct["native_relative_rig"]["transforms_T_camera0_camera"][str(i)], f"native camera{i}")
        calibration = copy.deepcopy(direct["cameras"][str(i)]["calibration"])
        # The runtime frame has a 512-byte row stride; columns 508..511 are padding. K/D keep the direct fit's
        # native 508x508 pixel coordinates.
        calibration["resolution"] = {"width": 512, "height": 508}
        cameras.append(
            {
                "camera": i,
                "calibration": calibration,
                "transform_to_rig_T_rig_camera_opencv": native.tolist(),
                "pose_in_tracking_origin_xrt": xrt_pose_from_opencv_transform(native),
                "direct_charuco_fit": copy.deepcopy(direct["cameras"][str(i)]["fit"]),
                "direct_charuco_leave_one_pose_out": copy.deepcopy(
                    direct["cameras"][str(i)].get("leave_one_pose_out")
                ),
                "native_active_image": {"width": 508, "height": 508, "padding_columns_ignored": [508, 509, 510, 511]},
            }
        )
    result["cameras"] = cameras

    baseline_mm = {
        f"{a}-{b}": float(1000 * np.linalg.norm(np.asarray(cameras[a]["transform_to_rig_T_rig_camera_opencv"])[:3, 3]
                                                  - np.asarray(cameras[b]["transform_to_rig_T_rig_camera_opencv"])[:3, 3]))
        for a, b in ((0, 1), (0, 2), (1, 3), (2, 3), (0, 3), (1, 2))
    }
    result["candidate_provenance"] = {
        "direct_charuco": {
            "path": str(paths["direct"]),
            "sha256": sha256_file(paths["direct"]),
            "capture_root": direct["capture_root"],
            "capture_images": direct.get("capture_images"),
            "board": direct["board"],
            "native_rig_fit": {k: direct["native_relative_rig"]["fit"][k] for k in ("rms_px", "median_px", "p95_px")},
        },
        "template": {"path": str(paths["provisional"]), "sha256": sha256_file(paths["provisional"])},
        "camera_centre_distances_mm": baseline_mm,
        "coordinate_convention": "T_rig_camera maps OpenCV camera (+x right,+y down,+z forward) to native camera0; "
        "XRT uses (+x right,+y up,-z forward) via C*T*C where C=diag(1,-1,-1,1)",
        "alignment": "none: native camera0 is the tracking origin; headset-frame placement not established",
        "intrinsics_source": "direct native mode-4 ChArUco only",
    }
    result["note"] = (
        "Direct ChArUco mode-4 rig with native camera0 origin. Relative camera geometry equals the 13 Sep aligned "
        "candidate; Sense held-out validation must be repeated (the original sense-validation captures were lost)."
    )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("direct", type=Path)
    parser.add_argument("provisional", type=Path, help="provisional constellation calibration, used as a template")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    paths = {"direct": args.direct, "provisional": args.provisional}
    result = build(json.loads(args.direct.read_text()), json.loads(args.provisional.read_text()), paths)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["candidate_provenance"]["camera_centre_distances_mm"], indent=2))
    print(f"wrote {args.output}; runtime_usable=false")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

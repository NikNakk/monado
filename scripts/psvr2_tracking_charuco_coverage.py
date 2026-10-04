#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Assess native PSVR2 mode-4 ChArUco capture quality and image coverage.

This is a capture-planning diagnostic, not a calibration solver. It scans static
capture directories containing the mode-4 tracking readout, averages repeated
frames, searches several monotonic greyscale enhancements for robust ChArUco
detection, and reports how well cameras 2/3 are covered.

Expected board defaults:
  * 7 x 5 squares
  * 40 mm square size
  * 30 mm marker size
  * DICT_4X4_50

Mode-4 mapping:
  camera 2 = set 5 plane 0
  camera 3 = set 5 plane 1

The final four columns of the 512-wide transport image are ignored; the active
tracking image is 508 x 508.

Greyscale enhancement is used only to detect board geometry. It does not alter
the image coordinate system and therefore does not itself create calibration
measurements.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

import cv2
import numpy as np

ACTIVE_W = 508
ACTIVE_H = 508
MODE4 = {2: (5, 0), 3: (5, 1)}
GRID_LABELS = (
    ("upper-left", "upper-centre", "upper-right"),
    ("middle-left", "centre", "middle-right"),
    ("lower-left", "lower-centre", "lower-right"),
)


def make_board(squares_x: int, squares_y: int, square_m: float, marker_m: float, dictionary_id: int):
    dictionary = cv2.aruco.getPredefinedDictionary(dictionary_id)
    return cv2.aruco.CharucoBoard((squares_x, squares_y), square_m, marker_m, dictionary), dictionary


def make_detector(board):
    params = cv2.aruco.DetectorParameters()
    params.cornerRefinementMethod = cv2.aruco.CORNER_REFINE_SUBPIX
    params.adaptiveThreshWinSizeMin = 3
    params.adaptiveThreshWinSizeMax = 51
    params.adaptiveThreshWinSizeStep = 4
    params.minMarkerPerimeterRate = 0.01
    params.maxMarkerPerimeterRate = 4.0
    if hasattr(cv2.aruco, "CharucoDetector"):
        charuco = cv2.aruco.CharucoParameters()
        charuco.minMarkers = 2
        if hasattr(charuco, "tryRefineMarkers"):
            charuco.tryRefineMarkers = True
        return cv2.aruco.CharucoDetector(board, charuco, params), params
    return None, params


def capture_directories(root: Path) -> list[Path]:
    found = set()
    for path in root.rglob("mode-04-size-*-set-5-example-*-plane0.pgm"):
        if "__MACOSX" in path.parts:
            continue
        found.add(path.parent)
    if not found:
        raise ValueError(f"{root}: no mode-4 set-5 tracking PGM files found")
    return sorted(found, key=lambda p: str(p.relative_to(root)))


def frame_paths(directory: Path, camera: int) -> list[Path]:
    camera_set, plane = MODE4[camera]
    return sorted(directory.glob(f"mode-04-size-*-set-{camera_set}-example-*-plane{plane}.pgm"))


def load_stack(directory: Path, camera: int) -> np.ndarray:
    paths = frame_paths(directory, camera)
    if len(paths) < 2:
        raise ValueError(f"{directory}: camera {camera} has only {len(paths)} tracking frames")
    images = []
    for path in paths:
        image = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
        if image is None or image.shape != (ACTIVE_H, 512):
            raise ValueError(f"{path}: expected 512x508 8-bit PGM, got {None if image is None else image.shape}")
        image = image.copy()
        image[:, ACTIVE_W:] = 0
        images.append(image)
    return np.stack(images)


def linear_stretch(image: np.ndarray, lo: float, hi: float) -> np.ndarray:
    if hi <= lo + 1e-6:
        return np.zeros_like(image, dtype=np.uint8)
    return np.clip((image - lo) * (255.0 / (hi - lo)), 0.0, 255.0).astype(np.uint8)


def enhancement_variants(stack: np.ndarray):
    mean = stack.astype(np.float32).mean(axis=0)
    median = np.median(stack, axis=0).astype(np.float32)
    clahe = cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8))

    # Keep the search deliberately small enough for interactive capture
    # checking. In PSVR2 mode-4 images the useful scene signal is commonly in
    # grey levels 0..8 while lights saturate at 255, so fixed low-range
    # stretches are especially productive.
    for source_name, source in (("mean", mean), ("median", median)):
        for hi in (4.0, 8.0):
            yield f"{source_name}_hi{int(hi)}", linear_stretch(source, 0.0, hi)

        values = source[:, :ACTIVE_W].reshape(-1)
        lo = float(np.percentile(values, 0.5))
        for percentile in (95.0, 98.0, 99.0):
            hi = float(np.percentile(values, percentile))
            if hi <= lo + 0.2:
                continue
            stretched = linear_stretch(source, lo, hi)
            tag = str(percentile).rstrip("0").rstrip(".")
            yield f"{source_name}_p{tag}", stretched
            if percentile == 98.0:
                yield f"{source_name}_p{tag}_clahe", clahe.apply(stretched)


def detect_charuco(image: np.ndarray, board, dictionary, detector, detector_params):
    if detector is not None:
        charuco_corners, charuco_ids, marker_corners, marker_ids = detector.detectBoard(image)
    else:
        marker_corners, marker_ids, _ = cv2.aruco.detectMarkers(
            image, dictionary, parameters=detector_params
        )
        charuco_corners = None
        charuco_ids = None
        if marker_ids is not None and len(marker_ids) >= 2:
            _, charuco_corners, charuco_ids = cv2.aruco.interpolateCornersCharuco(
                marker_corners, marker_ids, image, board
            )

    marker_count = 0 if marker_ids is None else int(len(marker_ids))
    if charuco_ids is None or charuco_corners is None:
        return np.empty((0, 2), dtype=np.float32), np.empty((0,), dtype=np.int32), marker_count

    points = np.asarray(charuco_corners, dtype=np.float32).reshape(-1, 2)
    ids = np.asarray(charuco_ids, dtype=np.int32).reshape(-1)
    return points, ids, marker_count


def best_detection(stack, board, dictionary, detector, detector_params):
    best = None
    for name, enhanced in enhancement_variants(stack):
        points, ids, marker_count = detect_charuco(
            enhanced, board, dictionary, detector, detector_params
        )
        # Primary score: identified ChArUco corners. Marker count breaks ties.
        score = (int(len(ids)), marker_count)
        if best is None or score > best["score"]:
            best = {
                "score": score,
                "enhancement": name,
                "image": enhanced,
                "points": points,
                "ids": ids,
                "marker_count": marker_count,
            }
    return best


def perspective_ratio(board, points: np.ndarray, ids: np.ndarray, squares_x: int, squares_y: int, square_m: float):
    if len(ids) < 4:
        return None
    object_xy = np.asarray(board.getChessboardCorners(), dtype=np.float64)[ids, :2]
    H, _ = cv2.findHomography(object_xy, points.astype(np.float64), 0)
    if H is None or abs(float(H[2, 2])) < 1e-12:
        return None
    H = H / H[2, 2]
    width = squares_x * square_m
    height = squares_y * square_m
    outer = np.asarray([[0.0, 0.0], [width, 0.0], [width, height], [0.0, height]])
    w = outer @ H[2, :2] + H[2, 2]
    if np.any(np.abs(w) < 1e-9):
        return None
    values = np.abs(w)
    return float(values.max() / values.min())


def detection_metrics(board, result, squares_x: int, squares_y: int, square_m: float) -> dict:
    points = result["points"]
    ids = result["ids"]
    if len(points) == 0:
        return {
            "charuco_corners": 0,
            "marker_count": result["marker_count"],
            "ids": [],
            "centroid_px": None,
            "bbox_px": None,
            "hull_area_fraction": 0.0,
            "perspective_ratio": None,
            "enhancement": result["enhancement"],
        }

    minimum = points.min(axis=0)
    maximum = points.max(axis=0)
    hull_area = 0.0
    if len(points) >= 3:
        hull = cv2.convexHull(points.astype(np.float32))
        hull_area = float(cv2.contourArea(hull) / (ACTIVE_W * ACTIVE_H))

    return {
        "charuco_corners": int(len(ids)),
        "marker_count": int(result["marker_count"]),
        "ids": [int(value) for value in ids],
        "centroid_px": [float(v) for v in points.mean(axis=0)],
        "bbox_px": [float(minimum[0]), float(minimum[1]), float(maximum[0]), float(maximum[1])],
        "hull_area_fraction": hull_area,
        "perspective_ratio": perspective_ratio(
            board, points, ids, squares_x, squares_y, square_m
        ),
        "enhancement": result["enhancement"],
    }


def quality_label(corners: int) -> str:
    if corners >= 18:
        return "EXCELLENT"
    if corners >= 10:
        return "GOOD"
    if corners >= 6:
        return "USABLE"
    if corners > 0:
        return "LOW"
    return "NO BOARD"


def save_preview(path: Path, result, metrics: dict, title: str):
    image = cv2.cvtColor(result["image"], cv2.COLOR_GRAY2BGR)
    if len(result["ids"]):
        cv2.aruco.drawDetectedCornersCharuco(
            image,
            result["points"].reshape(-1, 1, 2),
            result["ids"].reshape(-1, 1),
            (0, 255, 0),
        )
    cv2.putText(
        image,
        f"{title}: {metrics['charuco_corners']} corners ({quality_label(metrics['charuco_corners'])})",
        (8, 22),
        cv2.FONT_HERSHEY_SIMPLEX,
        0.48,
        (255, 255, 255),
        1,
        cv2.LINE_AA,
    )
    cv2.imwrite(str(path), image)


def summarize_camera(camera: int, captures: list[dict], min_corners: int, target_poses: int) -> dict:
    usable = [item for item in captures if item["metrics"]["charuco_corners"] >= min_corners]
    all_points = [item["points"] for item in usable if len(item["points"])]
    cell_poses = [[set() for _ in range(3)] for _ in range(3)]
    unique_ids = set()
    perspective = []
    scale_areas = []

    for item in usable:
        metrics = item["metrics"]
        unique_ids.update(metrics["ids"])
        if metrics["perspective_ratio"] is not None:
            perspective.append(metrics["perspective_ratio"])
        if metrics["charuco_corners"] >= 10:
            scale_areas.append(metrics["hull_area_fraction"])
        for x, y in item["points"]:
            col = min(2, max(0, int(float(x) / (ACTIVE_W / 3.0))))
            row = min(2, max(0, int(float(y) / (ACTIVE_H / 3.0))))
            cell_poses[row][col].add(item["capture"])

    grid = {
        GRID_LABELS[row][col]: len(cell_poses[row][col])
        for row in range(3)
        for col in range(3)
    }
    undercovered = [name for name, count in grid.items() if count < 2]

    if all_points:
        points = np.vstack(all_points)
        span = {
            "x_min": float(points[:, 0].min()),
            "x_max": float(points[:, 0].max()),
            "y_min": float(points[:, 1].min()),
            "y_max": float(points[:, 1].max()),
        }
    else:
        span = None

    oblique_count = sum(value >= 1.25 for value in perspective)
    recommendations = []
    if len(usable) < target_poses:
        recommendations.append(
            f"collect at least {target_poses - len(usable)} more usable poses (target >= {target_poses})"
        )
    if undercovered:
        recommendations.append(
            "prioritise board/corners toward: " + ", ".join(undercovered)
        )
    if oblique_count < 3:
        recommendations.append(
            f"add {3 - oblique_count} more clearly oblique board views (yaw/pitch)"
        )
    if len(scale_areas) >= 2:
        low = min(scale_areas)
        high = max(scale_areas)
        if low > 0 and high / low < 1.8:
            recommendations.append("add more near/far scale variation")
    elif len(scale_areas) < 2:
        recommendations.append("add near/far scale variation with >=10 detected corners")

    return {
        "camera": camera,
        "usable_capture_count": len(usable),
        "target_capture_count": target_poses,
        "unique_charuco_ids": sorted(unique_ids),
        "unique_charuco_id_count": len(unique_ids),
        "corner_span_px": span,
        "grid_pose_counts": grid,
        "undercovered_grid_cells": undercovered,
        "oblique_pose_count": int(oblique_count),
        "perspective_ratio_range": None
        if not perspective
        else [float(min(perspective)), float(max(perspective))],
        "high_quality_hull_area_fraction_range": None
        if not scale_areas
        else [float(min(scale_areas)), float(max(scale_areas))],
        "recommendations": recommendations,
        "capture_ready": len(usable) >= target_poses and not undercovered and oblique_count >= 3,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_root", type=Path)
    parser.add_argument("--cameras", type=int, nargs="+", default=[2, 3], choices=[2, 3])
    parser.add_argument("--squares-x", type=int, default=7)
    parser.add_argument("--squares-y", type=int, default=5)
    parser.add_argument("--square-mm", type=float, default=40.0)
    parser.add_argument("--marker-mm", type=float, default=30.0)
    parser.add_argument("--dictionary", default="DICT_4X4_50")
    parser.add_argument("--min-corners", type=int, default=6)
    parser.add_argument("--target-poses", type=int, default=10)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--preview-dir", type=Path)
    args = parser.parse_args()

    if not hasattr(cv2, "aruco"):
        raise SystemExit("OpenCV aruco module is required (opencv-contrib-python / OpenCV with aruco)")
    if not hasattr(cv2.aruco, args.dictionary):
        raise SystemExit(f"unknown ArUco dictionary: {args.dictionary}")
    dictionary_id = getattr(cv2.aruco, args.dictionary)

    square_m = args.square_mm / 1000.0
    marker_m = args.marker_mm / 1000.0
    board, dictionary = make_board(
        args.squares_x, args.squares_y, square_m, marker_m, dictionary_id
    )
    detector, detector_params = make_detector(board)
    directories = capture_directories(args.capture_root)

    if args.preview_dir is not None:
        args.preview_dir.mkdir(parents=True, exist_ok=True)

    per_camera = {camera: [] for camera in args.cameras}
    output_captures = []

    for directory in directories:
        capture_name = str(directory.relative_to(args.capture_root))
        if capture_name == ".":
            capture_name = directory.name
        row = {"capture": capture_name, "cameras": {}}
        for camera in args.cameras:
            stack = load_stack(directory, camera)
            best = best_detection(stack, board, dictionary, detector, detector_params)
            metrics = detection_metrics(
                board, best, args.squares_x, args.squares_y, square_m
            )
            row["cameras"][str(camera)] = metrics
            per_camera[camera].append(
                {
                    "capture": capture_name,
                    "metrics": metrics,
                    "points": best["points"],
                }
            )

            centroid = metrics["centroid_px"]
            centroid_text = "-" if centroid is None else f"({centroid[0]:.0f},{centroid[1]:.0f})"
            perspective = metrics["perspective_ratio"]
            perspective_text = "-" if perspective is None else f"{perspective:.2f}"
            print(
                f"{capture_name} cam{camera}: {metrics['charuco_corners']:2d} corners "
                f"{quality_label(metrics['charuco_corners']):9s} "
                f"centroid={centroid_text:>11s} perspective={perspective_text:>4s} "
                f"enhancement={metrics['enhancement']}"
            )

            if args.preview_dir is not None:
                safe = capture_name.replace("/", "_").replace(" ", "_")
                save_preview(
                    args.preview_dir / f"{safe}-cam{camera}.png",
                    best,
                    metrics,
                    f"{capture_name} cam{camera}",
                )
        output_captures.append(row)

    summaries = {}
    print("\ncoverage summary:")
    for camera in args.cameras:
        summary = summarize_camera(camera, per_camera[camera], args.min_corners, args.target_poses)
        summaries[str(camera)] = summary
        span = summary["corner_span_px"]
        span_text = "-"
        if span is not None:
            span_text = (
                f"x={span['x_min']:.0f}..{span['x_max']:.0f}, "
                f"y={span['y_min']:.0f}..{span['y_max']:.0f}"
            )
        print(
            f"camera {camera}: {summary['usable_capture_count']} usable poses; "
            f"{summary['unique_charuco_id_count']} unique IDs; span {span_text}"
        )
        print("  3x3 pose coverage:")
        for row_index in range(3):
            print(
                "    "
                + " | ".join(
                    f"{GRID_LABELS[row_index][col]}={summary['grid_pose_counts'][GRID_LABELS[row_index][col]]}"
                    for col in range(3)
                )
            )
        if summary["recommendations"]:
            for recommendation in summary["recommendations"]:
                print(f"  NEED: {recommendation}")
        else:
            print("  COVERAGE READY for calibration fitting")

    result = {
        "format": "psvr2-tracking-charuco-coverage-v1",
        "runtime_usable": False,
        "capture_root": str(args.capture_root),
        "board": {
            "squares_x": args.squares_x,
            "squares_y": args.squares_y,
            "square_mm": args.square_mm,
            "marker_mm": args.marker_mm,
            "dictionary": args.dictionary,
            "charuco_corner_count": int(len(board.getChessboardCorners())),
        },
        "tracking_image": {
            "width": ACTIVE_W,
            "height": ACTIVE_H,
            "transport_width": 512,
            "ignored_padding_columns": [508, 509, 510, 511],
        },
        "min_corners_for_usable_capture": args.min_corners,
        "target_poses_per_camera": args.target_poses,
        "captures": output_captures,
        "cameras": summaries,
        "note": (
            "Capture-planning diagnostic only. Greyscale enhancement is used for ChArUco detection, "
            "not as a geometric transform. Native calibration should subsequently refine corner "
            "locations and fit intrinsics/distortion from the tracking readout itself."
        ),
    }

    if args.output is not None:
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(f"\nwrote {args.output}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

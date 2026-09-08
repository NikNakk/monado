#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""PSVR2 four-camera ChArUco calibration helper.

Initial scope:
- Generate the agreed A3 7x5 / 40 mm / 30 mm / DICT_4X4_50 target.
- Validate four-camera datasets produced by the visible PyUSB recorder or the
  earlier Monado mode-4 recorder.
- Detect ChArUco corners in all four synchronized camera streams.
- Write repeatable per-corner observations for the later calibration stages.

The fisheye intrinsics, multi-camera extrinsics and hand-eye solve come next,
after a real target dataset has validated detection quality.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

try:
    import cv2
    import numpy as np
except ImportError as exc:  # pragma: no cover - environment dependent
    raise SystemExit(
        "This tool requires OpenCV with the aruco module and NumPy. "
        "Install an OpenCV build that includes cv2.aruco."
    ) from exc

SQUARES_X = 7
SQUARES_Y = 5
SQUARE_LENGTH_MM = 40.0
MARKER_LENGTH_MM = 30.0
DICTIONARY_NAME = "DICT_4X4_50"
A3_WIDTH_MM = 420.0
A3_HEIGHT_MM = 297.0
CAMERA_COUNT = 4


@dataclass
class Detection:
    set_index: int
    sequence_id: int
    camera: int
    corner_id: int
    x: float
    y: float


def get_dictionary():
    aruco = cv2.aruco
    dictionary_id = getattr(aruco, DICTIONARY_NAME)
    return aruco.getPredefinedDictionary(dictionary_id)


def create_board(square_length: float = SQUARE_LENGTH_MM, marker_length: float = MARKER_LENGTH_MM):
    dictionary = get_dictionary()
    size = (SQUARES_X, SQUARES_Y)
    try:
        return cv2.aruco.CharucoBoard(size, square_length, marker_length, dictionary)
    except (AttributeError, TypeError):
        return cv2.aruco.CharucoBoard_create(SQUARES_X, SQUARES_Y, square_length, marker_length, dictionary)


def render_board(board, size: tuple[int, int]):
    if hasattr(board, "generateImage"):
        return board.generateImage(size, marginSize=0, borderBits=1)
    return board.draw(size, marginSize=0, borderBits=1)


def mm_to_px(mm: float, dpi: float) -> int:
    return int(round(mm / 25.4 * dpi))


def command_board(args: argparse.Namespace) -> int:
    if args.dpi <= 0:
        raise SystemExit("--dpi must be positive")

    page_w = mm_to_px(A3_WIDTH_MM, args.dpi)
    page_h = mm_to_px(A3_HEIGHT_MM, args.dpi)
    board_w_mm = SQUARES_X * SQUARE_LENGTH_MM
    board_h_mm = SQUARES_Y * SQUARE_LENGTH_MM
    board_w = mm_to_px(board_w_mm, args.dpi)
    board_h = mm_to_px(board_h_mm, args.dpi)

    board = create_board()
    board_image = render_board(board, (board_w, board_h))
    page = np.full((page_h, page_w), 255, dtype=np.uint8)
    x0 = (page_w - board_w) // 2
    y0 = (page_h - board_h) // 2
    page[y0 : y0 + board_h, x0 : x0 + board_w] = board_image

    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    if not cv2.imwrite(str(output), page):
        raise SystemExit(f"Failed to write {output}")

    sidecar = output.with_suffix(output.suffix + ".json")
    sidecar.write_text(
        json.dumps(
            {
                "paper": "A3 landscape",
                "paper_width_mm": A3_WIDTH_MM,
                "paper_height_mm": A3_HEIGHT_MM,
                "dpi": args.dpi,
                "squares_x": SQUARES_X,
                "squares_y": SQUARES_Y,
                "square_length_mm_nominal": SQUARE_LENGTH_MM,
                "marker_length_mm_nominal": MARKER_LENGTH_MM,
                "dictionary": DICTIONARY_NAME,
                "board_width_mm": board_w_mm,
                "board_height_mm": board_h_mm,
                "print_instruction": "Print as A3 landscape at 100% / actual size; disable fit-to-page.",
            },
            indent=2,
        )
        + "\n"
    )

    print(f"Wrote {output} ({page_w}x{page_h} px at {args.dpi:g} dpi design resolution)")
    print(f"Board area: {board_w_mm:.1f} x {board_h_mm:.1f} mm, centred on A3 landscape")
    print("Print at 100% / actual size with fit-to-page disabled.")
    print("After mounting flat, measure the actual printed square size accurately.")
    return 0


def make_detector(board):
    if hasattr(cv2.aruco, "CharucoDetector"):
        return cv2.aruco.CharucoDetector(board)
    return None


def detect_charuco(image, board, detector):
    if detector is not None:
        corners, ids, _marker_corners, _marker_ids = detector.detectBoard(image)
        return corners, ids

    marker_corners, marker_ids, _rejected = cv2.aruco.detectMarkers(image, get_dictionary())
    if marker_ids is None or len(marker_ids) == 0:
        return None, None
    _count, corners, ids = cv2.aruco.interpolateCornersCharuco(marker_corners, marker_ids, image, board)
    return corners, ids


def load_manifest(dataset_dir: Path) -> list[dict[str, str]]:
    manifest_path = dataset_dir / "manifest.csv"
    if not manifest_path.exists():
        raise SystemExit(f"Missing {manifest_path}")
    with manifest_path.open(newline="") as file:
        return list(csv.DictReader(file))


def iter_camera_paths(dataset_dir: Path, row: dict[str, str]) -> Iterable[tuple[int, Path]]:
    for camera in range(CAMERA_COUNT):
        key = f"camera{camera}_file"
        relative = row.get(key, "")
        if not relative:
            continue
        yield camera, dataset_dir / relative


def command_inspect(args: argparse.Namespace) -> int:
    dataset_dir = Path(args.dataset)
    metadata_path = dataset_dir / "dataset.json"
    if not metadata_path.exists():
        raise SystemExit(f"Missing {metadata_path}")
    metadata = json.loads(metadata_path.read_text())
    camera_mode = int(metadata.get("camera_mode", -1))
    camera_count = int(metadata.get("camera_count", -1))
    if camera_count != 4 or camera_mode not in (3, 4):
        raise SystemExit(
            f"Dataset is not a supported four-camera PSVR2 calibration dataset "
            f"(camera_mode={camera_mode}, camera_count={camera_count})"
        )

    rows = load_manifest(dataset_dir)
    if not rows:
        raise SystemExit("Manifest contains no synchronized frame sets")

    board = create_board()
    detector = make_detector(board)
    output_path = Path(args.output) if args.output else dataset_dir / "charuco-detections.csv"
    output_path.parent.mkdir(parents=True, exist_ok=True)

    camera_frames = [0] * CAMERA_COUNT
    camera_detected = [0] * CAMERA_COUNT
    camera_corner_total = [0] * CAMERA_COUNT
    missing_images = 0
    strong_sets = 0
    detections: list[Detection] = []

    for row in rows:
        set_index = int(row["set_index"])
        sequence_id = int(row["sequence_id"])
        cameras_strong = 0
        for camera, image_path in iter_camera_paths(dataset_dir, row):
            camera_frames[camera] += 1
            image = cv2.imread(str(image_path), cv2.IMREAD_GRAYSCALE)
            if image is None:
                print(f"warning: could not read {image_path}", file=sys.stderr)
                missing_images += 1
                continue
            corners, ids = detect_charuco(image, board, detector)
            if corners is None or ids is None:
                continue

            ids_flat = np.asarray(ids).reshape(-1)
            corners_flat = np.asarray(corners, dtype=np.float64).reshape(-1, 2)
            count = min(len(ids_flat), len(corners_flat))
            if count == 0:
                continue

            camera_detected[camera] += 1
            camera_corner_total[camera] += count
            if count >= args.min_corners:
                cameras_strong += 1

            for corner_id, point in zip(ids_flat[:count], corners_flat[:count]):
                detections.append(
                    Detection(
                        set_index=set_index,
                        sequence_id=sequence_id,
                        camera=camera,
                        corner_id=int(corner_id),
                        x=float(point[0]),
                        y=float(point[1]),
                    )
                )

        if cameras_strong >= args.min_cameras:
            strong_sets += 1

    with output_path.open("w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["set_index", "sequence_id", "camera", "corner_id", "x", "y"])
        for detection in detections:
            writer.writerow(
                [
                    detection.set_index,
                    detection.sequence_id,
                    detection.camera,
                    detection.corner_id,
                    f"{detection.x:.6f}",
                    f"{detection.y:.6f}",
                ]
            )

    print(f"Dataset mode: {camera_mode}; sets: {len(rows)}")
    for camera in range(CAMERA_COUNT):
        detected = camera_detected[camera]
        mean_corners = camera_corner_total[camera] / detected if detected else 0.0
        print(
            f"camera {camera}: {detected}/{camera_frames[camera]} frames with ChArUco corners; "
            f"mean {mean_corners:.1f} corners when detected"
        )
    print(
        f"Strong synchronized sets: {strong_sets}/{len(rows)} "
        f"(>= {args.min_corners} corners in >= {args.min_cameras} cameras)"
    )
    if missing_images:
        print(f"Missing/unreadable images: {missing_images}")
    print(f"Wrote {len(detections)} corner observations to {output_path}")

    if strong_sets < args.min_strong_sets:
        print(
            f"Dataset is weak for calibration: only {strong_sets} strong sets; "
            f"target at least {args.min_strong_sets} with broad position/angle coverage.",
            file=sys.stderr,
        )
        return 2
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    board = subparsers.add_parser("board", help="Generate the agreed A3 ChArUco target")
    board.add_argument("output", help="Output PNG path")
    board.add_argument("--dpi", type=float, default=300.0, help="Design resolution for the A3 page (default: 300)")
    board.set_defaults(func=command_board)

    inspect = subparsers.add_parser("inspect", help="Detect ChArUco corners in a recorded four-camera dataset")
    inspect.add_argument("dataset", help="Dataset directory produced by a PSVR2 calibration recorder")
    inspect.add_argument("--output", help="Corner-observation CSV path (default: DATASET/charuco-detections.csv)")
    inspect.add_argument("--min-corners", type=int, default=8, help="Corners required for a camera observation to count as strong")
    inspect.add_argument("--min-cameras", type=int, default=2, help="Strong camera observations required in a synchronized set")
    inspect.add_argument("--min-strong-sets", type=int, default=20, help="Minimum strong synchronized sets before returning success")
    inspect.set_defaults(func=command_inspect)
    return parser


def main() -> int:
    if not hasattr(cv2, "aruco"):
        raise SystemExit("This OpenCV build does not include cv2.aruco")
    args = build_parser().parse_args()
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())

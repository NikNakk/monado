#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Capture one static mode-4 ChArUco pose for the direct calibration and report session coverage.

    .venv/bin/python scripts/psvr2_charuco_capture_pose.py SESSION_DIR LABEL
    .venv/bin/python scripts/psvr2_charuco_capture_pose.py SESSION_DIR --report     # coverage only

Each call captures SESSION_DIR/Pnn-LABEL with the same survey settings as the 13 September char-mode-12 capture
(mode 4, 0.5 s settle, 1 s sample, 8 frames per camera), then detects the board in each camera with the direct
solver's own detector and prints the whole session's coverage:

- corners per camera for this pose (>= 6 makes the pose usable for that camera);
- usable poses per camera (the solver needs >= 6; aim for >= 15);
- which cells of a 3x3 image grid each camera has seen the board centre in (aim for >= 7 of 9, edges matter);
- poses seen together by each camera pair (they carry the rig extrinsics; aim for >= 8 per overlapping pair).

Monado must not be running: the survey claims the camera interface over libusb. Never use /tmp.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import psvr2_tracking_charuco_direct as direct  # noqa: E402

MIN_CORNERS = 6
TARGET_POSES = 15
TARGET_CELLS = 7
TARGET_PAIR = 8
SIZE = direct.W


def detect_pose(pose_dir: Path, detector) -> list[dict]:
    return [direct.detect(pose_dir, camera, detector) for camera in range(4)]


def cell_of(points: np.ndarray) -> tuple[int, int]:
    c = points.mean(axis=0)
    return min(2, int(c[0] * 3 / SIZE)), min(2, int(c[1] * 3 / SIZE))


def report(session: Path, detector) -> None:
    poses = sorted(p for p in session.iterdir() if p.is_dir() and re.match(r"P\d\d", p.name))
    usable = {c: 0 for c in range(4)}
    cells = {c: set() for c in range(4)}
    pairs = np.zeros((4, 4), int)
    print(f"\nsession {session} ({len(poses)} poses)")
    print("pose                                         cam0 cam1 cam2 cam3")
    for pose in poses:
        try:
            found = detect_pose(pose, detector)
        except ValueError as e:
            print(f"{pose.name:44s} unreadable: {e}")
            continue
        ok = [f["corners"] >= MIN_CORNERS for f in found]
        print(f"{pose.name[:44]:44s} " + " ".join(f"{f['corners']:4d}" for f in found))
        for c in range(4):
            if ok[c]:
                usable[c] += 1
                cells[c].add(cell_of(found[c]["points"]))
            for d in range(c + 1, 4):
                if ok[c] and ok[d]:
                    pairs[c, d] += 1

    print("\nusable poses per camera (>= %d corners):" % MIN_CORNERS)
    for c in range(4):
        grid = "".join("#" if (x, y) in cells[c] else "." for y in range(3) for x in range(3))
        grid = " ".join(grid[i : i + 3] for i in (0, 3, 6))
        flag = "" if usable[c] >= TARGET_POSES and len(cells[c]) >= TARGET_CELLS else "   <-- needs more"
        print(f"  cam{c}: {usable[c]:3d} poses, image cells {len(cells[c])}/9 [{grid}]{flag}")
    print("poses seen by both cameras of a pair:")
    for c in range(4):
        for d in range(c + 1, 4):
            flag = "" if pairs[c, d] >= TARGET_PAIR else "   <-- needs more (if these cameras overlap)"
            print(f"  cam{c}+cam{d}: {pairs[c, d]:3d}{flag}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("session", type=Path)
    parser.add_argument("label", nargs="?", help="short description of the pose, e.g. board-left-near")
    parser.add_argument("--report", action="store_true", help="only print the session's coverage")
    args = parser.parse_args()

    session = args.session.expanduser().resolve()
    if str(session).startswith(("/tmp", "/private/tmp")):
        print("refusing to write under /tmp: macOS purges it", file=sys.stderr)
        return 2
    session.mkdir(parents=True, exist_ok=True)
    _, detector = direct.board_detector()

    if not args.report:
        if not args.label:
            parser.error("LABEL is required unless --report")
        label = re.sub(r"[^A-Za-z0-9._-]+", "-", args.label).strip("-")
        existing = [int(p.name[1:3]) for p in session.iterdir() if p.is_dir() and re.match(r"P\d\d", p.name)]
        pose = session / f"P{(max(existing) + 1 if existing else 0):02d}-{label}"
        survey = Path(__file__).resolve().parent / "psvr2_camera_mode_survey.py"
        cmd = [sys.executable, str(survey), str(pose), "--modes", "4", "--settle", "0.5", "--sample", "1.0",
               "--examples", "8", "--save-every", "4", "--no-contact-sheet"]
        print("capturing", pose.name, "- hold still")
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stdout[-2000:], result.stderr[-2000:], file=sys.stderr)
            return result.returncode
        found = detect_pose(pose, detector)
        print("this pose, corners per camera: " + "  ".join(f"cam{c} {f['corners']}" for c, f in enumerate(found)))
        if not any(f["corners"] >= MIN_CORNERS for f in found):
            print("  no camera sees enough of the board: check lighting and placement (pose kept; delete it if useless)")

    report(session, detector)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

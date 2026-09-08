#!/usr/bin/env python3
import argparse
import json
import re
from pathlib import Path

import cv2
import numpy as np

PGM_RE = re.compile(r"mode-([0-9a-f]{2})-size-(\d+)-set-(-?\d+)-example-(\d+)-plane(\d+)\.pgm$")


def read_gray(path):
    im = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if im is None:
        raise RuntimeError(f"Could not read {path}")
    return im


def zncc(a, b):
    a = a.astype(np.float32)
    b = b.astype(np.float32)
    am = a.mean()
    bm = b.mean()
    asd = a.std()
    bsd = b.std()
    if asd < 1e-6 or bsd < 1e-6:
        return -1.0
    return float(np.mean((a - am) * (b - bm)) / (asd * bsd))


def orient(im, name):
    if name == "identity":
        return im
    if name == "flip_x":
        return cv2.flip(im, 1)
    if name == "flip_y":
        return cv2.flip(im, 0)
    if name == "rot180":
        return cv2.flip(im, -1)
    if name == "rot90":
        return cv2.rotate(im, cv2.ROTATE_90_CLOCKWISE)
    if name == "rot270":
        return cv2.rotate(im, cv2.ROTATE_90_COUNTERCLOCKWISE)
    raise ValueError(name)


def best_small_shift(a, b, max_shift=12):
    """Estimate a translation to apply to b so that it aligns to a."""
    af = a.astype(np.float32)
    bf = b.astype(np.float32)
    shift, _ = cv2.phaseCorrelate(bf, af)
    dx = int(round(max(-max_shift, min(max_shift, shift[0]))))
    dy = int(round(max(-max_shift, min(max_shift, shift[1]))))
    h, w = a.shape
    y0 = max(0, dy)
    y1 = min(h, h + dy)
    x0 = max(0, dx)
    x1 = min(w, w + dx)
    aa = a[y0:y1, x0:x1]
    bb = b[y0 - dy:y1 - dy, x0 - dx:x1 - dx]
    return {"zncc": zncc(aa, bb), "dx": dx, "dy": dy}


def compare_scaled(src, dst, orientations=("identity", "flip_x", "flip_y", "rot180"), max_shift=12):
    candidates = []
    for orientation in orientations:
        oriented = orient(src, orientation)
        resized = cv2.resize(oriented, (dst.shape[1], dst.shape[0]), interpolation=cv2.INTER_AREA)
        result = best_small_shift(dst, resized, max_shift=max_shift)
        result.update(
            {
                "orientation": orientation,
                "scale_x": dst.shape[1] / oriented.shape[1],
                "scale_y": dst.shape[0] / oriented.shape[0],
            }
        )
        candidates.append(result)
    return max(candidates, key=lambda x: x["zncc"])


def find_images(root, mode=None, camera_set=None, example=0):
    found = []
    for path in Path(root).glob("*.pgm"):
        match = PGM_RE.match(path.name)
        if not match:
            continue
        parsed_mode = int(match.group(1), 16)
        parsed_set = int(match.group(3))
        parsed_example = int(match.group(4))
        plane = int(match.group(5))
        if mode is not None and parsed_mode != mode:
            continue
        if camera_set is not None and parsed_set != camera_set:
            continue
        if parsed_example != example:
            continue
        found.append((parsed_set, plane, path))
    return sorted(found, key=lambda x: (x[0], x[1]))


def mode4_cameras(root, example=0):
    """Mode 4 set 4 => cameras 0/1, set 5 => cameras 2/3."""
    out = {}
    for camera_set, plane, path in find_images(root, mode=4, example=example):
        if camera_set in (4, 5) and plane in (0, 1):
            out[(camera_set - 4) * 2 + plane] = path
    return out


def mode3_cameras(root, example=0):
    """Expose mode 3 candidates by set/plane without assuming physical ordering."""
    return {
        (camera_set, plane): path
        for camera_set, plane, path in find_images(root, mode=3, example=example)
        if camera_set in (0, 3)
    }


def mode12(root, example=0):
    visible = {
        plane: path for camera_set, plane, path in find_images(root, mode=12, camera_set=8, example=example)
    }
    tracking = {
        plane: path for camera_set, plane, path in find_images(root, mode=12, camera_set=9, example=example)
    }
    return visible, tracking


def matrix_assignment(scores):
    """Brute-force a unique assignment; this is only 4x4 for the current PSVR2 use."""
    import itertools

    rows = sorted(scores)
    cols = sorted(next(iter(scores.values())))
    best = None
    for permutation in itertools.permutations(cols, len(rows)):
        total = sum(scores[row][col]["zncc"] for row, col in zip(rows, permutation))
        if best is None or total > best[0]:
            best = (total, list(zip(rows, permutation)))
    return best


def main():
    parser = argparse.ArgumentParser(
        description="Register PSVR2 visible and controller-tracking camera readout modes."
    )
    parser.add_argument("sweep_dir", help="Full mode sweep directory containing mode 3/4/12 PGMs")
    parser.add_argument("--mode12-dir", help="Optional dedicated mode-12 survey directory")
    parser.add_argument("--output", default="psvr2-cross-mode-registration.json")
    args = parser.parse_args()

    sweep = Path(args.sweep_dir)
    mode12_root = Path(args.mode12_dir) if args.mode12_dir else sweep
    visible12, tracking12 = mode12(mode12_root, 0)
    mode4 = mode4_cameras(sweep, 0)
    mode3 = mode3_cameras(sweep, 0)

    if len(visible12) != 4 or len(tracking12) != 4 or len(mode4) != 4:
        raise SystemExit(
            "Need 4 mode12 visible, 4 mode12 tracking and 4 mode4 cameras; "
            f"got {len(visible12)}, {len(tracking12)}, {len(mode4)}"
        )

    report = {
        "format": "psvr2-cross-mode-registration-v1",
        "mode12_dir": str(mode12_root),
        "sweep_dir": str(sweep),
    }

    tracking_scores = {i: {} for i in range(4)}
    for mode12_plane, mode12_path in tracking12.items():
        target = read_gray(mode12_path)
        for mode4_camera, mode4_path in mode4.items():
            source = read_gray(mode4_path)
            tracking_scores[mode12_plane][mode4_camera] = compare_scaled(source, target, max_shift=8)

    total, pairs = matrix_assignment(tracking_scores)
    report["tracking"] = {"assignment_total_zncc": total, "pairs": []}
    for mode12_plane, mode4_camera in pairs:
        result = dict(tracking_scores[mode12_plane][mode4_camera])
        result.update(
            {
                "mode12_plane": mode12_plane,
                "mode4_camera": mode4_camera,
                "mode12_file": tracking12[mode12_plane].name,
                "mode4_file": mode4[mode4_camera].name,
            }
        )
        result["confidence"] = (
            "strong"
            if (
                result["orientation"] == "identity"
                and result["dx"] == 0
                and result["dy"] == 0
                and result["zncc"] >= 0.30
            )
            else ("moderate" if result["zncc"] >= 0.20 else "weak")
        )
        report["tracking"]["pairs"].append(result)

    if len(mode3) >= 4:
        visible_scores = {i: {} for i in range(4)}
        for mode12_plane, mode12_path in visible12.items():
            target = read_gray(mode12_path)
            for key, mode3_path in mode3.items():
                source = read_gray(mode3_path)
                visible_scores[mode12_plane][f"{key[0]}:{key[1]}"] = compare_scaled(
                    source, target, max_shift=20
                )

        total, pairs = matrix_assignment(visible_scores)
        report["visible"] = {"assignment_total_zncc": total, "pairs": []}
        for mode12_plane, key in pairs:
            camera_set, plane = map(int, key.split(":"))
            result = dict(visible_scores[mode12_plane][key])
            result.update(
                {
                    "mode12_plane": mode12_plane,
                    "mode3_set": camera_set,
                    "mode3_plane": plane,
                    "mode12_file": visible12[mode12_plane].name,
                    "mode3_file": mode3[(camera_set, plane)].name,
                }
            )
            result["confidence"] = (
                "strong" if result["zncc"] >= 0.60 else ("moderate" if result["zncc"] >= 0.40 else "weak")
            )
            report["visible"]["pairs"].append(result)

    tracking_pairs = report["tracking"]["pairs"]
    exact_half = all(
        pair["orientation"] == "identity"
        and pair["dx"] == 0
        and pair["dy"] == 0
        and abs(pair["scale_x"] - 0.5) < 1e-9
        and abs(pair["scale_y"] - 0.5) < 1e-9
        for pair in tracking_pairs
    )
    identity_order = all(pair["mode12_plane"] == pair["mode4_camera"] for pair in tracking_pairs)
    report["tracking"]["simple_model"] = {
        "supported": bool(exact_half and identity_order),
        "model": "mode12_tracking_is_half_resolution_mode4_same_order" if exact_half and identity_order else None,
        "note": (
            "Image correlation supports camera identity/readout mapping, not yet the exact sub-pixel "
            "intrinsic-coordinate transform."
        ),
    }

    if "visible" in report:
        strong = sum(1 for pair in report["visible"]["pairs"] if pair["confidence"] == "strong")
        report["visible"]["dataset_sufficient_for_mapping"] = strong >= 3
        report["visible"]["note"] = (
            "Current samples are sufficient for a tentative mapping."
            if strong >= 3
            else (
                "Current sweep samples are not sufficient to assert the full mode3-to-mode12 visible mapping; "
                "capture near-simultaneous stationary samples."
            )
        )

    report["dimensions"] = {
        "mode12_visible": list(read_gray(next(iter(visible12.values()))).shape[::-1]),
        "mode12_tracking": list(read_gray(next(iter(tracking12.values()))).shape[::-1]),
        "mode4_tracking": list(read_gray(next(iter(mode4.values()))).shape[::-1]),
    }

    with open(args.output, "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Register PSVR2 visible and controller-tracking camera readout modes.

The preferred input is a stationary --sequence 3,12,3 --repeat N capture from
psvr2_camera_mode_survey.py. Mode-3 819456-byte packets are decoded from their
raw .bin files as 1280x640 side-by-side L8: the older contiguous-plane decode
is intentionally not used.
"""

from __future__ import annotations

import argparse
import itertools
import json
import re
from collections import Counter, defaultdict
from pathlib import Path

import cv2
import numpy as np

VISIT_PGM_RE = re.compile(
    r"visit-(\d+)-mode-([0-9a-f]{2})-size-(\d+)-set-(-?\d+)-example-(\d+)-plane(\d+)\.pgm$"
)
LEGACY_PGM_RE = re.compile(r"mode-([0-9a-f]{2})-size-(\d+)-set-(-?\d+)-example-(\d+)-plane(\d+)\.pgm$")


def read_gray(path: Path) -> np.ndarray:
    image = cv2.imread(str(path), cv2.IMREAD_GRAYSCALE)
    if image is None:
        raise RuntimeError(f"Could not read {path}")
    return image


def zncc(a: np.ndarray, b: np.ndarray) -> float:
    af = a.astype(np.float32)
    bf = b.astype(np.float32)
    af -= af.mean()
    bf -= bf.mean()
    sa = af.std()
    sb = bf.std()
    if sa < 1e-6 or sb < 1e-6:
        return -1.0
    return float(np.mean(af * bf) / (sa * sb))


def orient(image: np.ndarray, name: str) -> np.ndarray:
    if name == "identity":
        return image
    if name == "flip_x":
        return cv2.flip(image, 1)
    if name == "flip_y":
        return cv2.flip(image, 0)
    if name == "rot180":
        return cv2.flip(image, -1)
    raise ValueError(name)


def score_shift(reference: np.ndarray, candidate: np.ndarray, dx: int, dy: int) -> float:
    h, w = reference.shape
    x0 = max(0, dx)
    x1 = min(w, w + dx)
    y0 = max(0, dy)
    y1 = min(h, h + dy)
    if x1 - x0 < 32 or y1 - y0 < 32:
        return -1.0
    a = reference[y0:y1, x0:x1]
    b = candidate[y0 - dy : y1 - dy, x0 - dx : x1 - dx]
    return zncc(a, b)


def best_small_shift(reference: np.ndarray, candidate: np.ndarray, max_shift: int = 12) -> dict:
    af = reference.astype(np.float32)
    bf = candidate.astype(np.float32)
    phase_shift, _ = cv2.phaseCorrelate(bf, af)
    cx = int(round(phase_shift[0]))
    cy = int(round(phase_shift[1]))
    candidates = {(0, 0)}
    for dx in range(max(-max_shift, cx - 3), min(max_shift, cx + 3) + 1):
        for dy in range(max(-max_shift, cy - 3), min(max_shift, cy + 3) + 1):
            candidates.add((dx, dy))
    best = max(((score_shift(reference, candidate, dx, dy), dx, dy) for dx, dy in candidates), key=lambda x: x[0])
    return {"zncc": best[0], "dx": best[1], "dy": best[2]}


def compare_scaled(src: np.ndarray, dst: np.ndarray, max_shift: int = 12) -> dict:
    results = []
    for orientation in ("identity", "flip_x", "flip_y", "rot180"):
        transformed = orient(src, orientation)
        resized = cv2.resize(transformed, (dst.shape[1], dst.shape[0]), interpolation=cv2.INTER_AREA)
        result = best_small_shift(dst, resized, max_shift=max_shift)
        result.update(
            orientation=orientation,
            scale_x=dst.shape[1] / transformed.shape[1],
            scale_y=dst.shape[0] / transformed.shape[0],
        )
        results.append(result)
    return max(results, key=lambda item: item["zncc"])


def unique_assignment(scores: dict, value_key: str = "zncc") -> tuple[float, list[tuple]]:
    rows = sorted(scores)
    cols = sorted(next(iter(scores.values())))
    best = None
    for permutation in itertools.permutations(cols, len(rows)):
        total = sum(scores[row][col][value_key] for row, col in zip(rows, permutation))
        if best is None or total > best[0]:
            best = (total, list(zip(rows, permutation)))
    assert best is not None
    return best


def decode_mode3_sbs(path: Path) -> tuple[np.ndarray, np.ndarray]:
    packet = path.read_bytes()
    if len(packet) != 819456:
        raise RuntimeError(f"Expected 819456-byte mode-3 packet, got {len(packet)}: {path}")
    payload = np.frombuffer(packet, dtype=np.uint8, offset=256)
    if payload.size != 1280 * 640:
        raise RuntimeError(f"Unexpected mode-3 payload size in {path}")
    raster = payload.reshape(640, 1280)
    return raster[:, :640].copy(), raster[:, 640:].copy()


def mode3_visit(root: Path, visit: int, example: int) -> dict[tuple[int, int], np.ndarray]:
    result: dict[tuple[int, int], np.ndarray] = {}
    for camera_set in (0, 3):
        path = root / f"visit-{visit:02d}-mode-03-size-819456-set-{camera_set}-example-{example}.bin"
        if not path.exists():
            continue
        left, right = decode_mode3_sbs(path)
        result[(camera_set, 0)] = left
        result[(camera_set, 1)] = right
    return result


def mode12_visit(root: Path, visit: int, camera_set: int, example: int) -> dict[int, np.ndarray]:
    size = 409856 if camera_set == 8 else 260352
    result = {}
    for plane in range(4):
        path = root / f"visit-{visit:02d}-mode-0c-size-{size}-set-{camera_set}-example-{example}-plane{plane}.pgm"
        if path.exists():
            result[plane] = read_gray(path)
    return result


def available_examples(root: Path, visit: int, mode: int, camera_set: int | None = None) -> list[int]:
    examples = set()
    for path in root.glob(f"visit-{visit:02d}-mode-{mode:02x}-*"):
        match = VISIT_PGM_RE.match(path.name)
        if match is None:
            if mode == 3 and path.suffix == ".bin":
                match2 = re.search(r"-example-(\d+)\.bin$", path.name)
                if match2:
                    examples.add(int(match2.group(1)))
            continue
        parsed_set = int(match.group(4))
        if camera_set is None or parsed_set == camera_set:
            examples.add(int(match.group(5)))
    return sorted(examples)


def analyze_visible_burst(root: Path, survey: dict) -> dict:
    plan = survey.get("capture_plan", [])
    mode12_visits = [i for i, mode in enumerate(plan) if mode == 12]
    observations = []

    for visit in mode12_visits:
        neighbours = [v for v in (visit - 1, visit + 1) if 0 <= v < len(plan) and plan[v] == 3]
        for ex12 in available_examples(root, visit, 12, 8):
            visible12 = mode12_visit(root, visit, 8, ex12)
            if len(visible12) != 4:
                continue
            for mode3_visit_index in neighbours:
                for ex3 in available_examples(root, mode3_visit_index, 3):
                    mode3 = mode3_visit(root, mode3_visit_index, ex3)
                    if len(mode3) != 4:
                        continue
                    scores = {plane: {} for plane in range(4)}
                    for plane, target in visible12.items():
                        for key, source in mode3.items():
                            scores[plane][key] = compare_scaled(source, target, max_shift=8)
                    total, pairs = unique_assignment(scores)
                    observations.append(
                        {
                            "mode12_visit": visit,
                            "mode12_example": ex12,
                            "mode3_visit": mode3_visit_index,
                            "mode3_example": ex3,
                            "assignment_total_zncc": total,
                            "pairs": [
                                {"mode12_plane": plane, "mode3_set": key[0], "mode3_plane": key[1], **scores[plane][key]}
                                for plane, key in pairs
                            ],
                        }
                    )

    by_plane = defaultdict(list)
    for observation in observations:
        for pair in observation["pairs"]:
            by_plane[pair["mode12_plane"]].append(pair)

    aggregate = []
    supported = len(by_plane) == 4 and bool(observations)
    for plane in range(4):
        pairs = by_plane.get(plane, [])
        if not pairs:
            supported = False
            continue
        votes = Counter((p["mode3_set"], p["mode3_plane"]) for p in pairs)
        mapping, vote_count = votes.most_common(1)[0]
        selected = [p for p in pairs if (p["mode3_set"], p["mode3_plane"]) == mapping]
        znccs = [p["zncc"] for p in selected]
        exact = all(
            p["orientation"] == "identity"
            and p["dx"] == 0
            and p["dy"] == 0
            and abs(p["scale_x"] - 0.5) < 1e-9
            and abs(p["scale_y"] - 0.5) < 1e-9
            for p in selected
        )
        if vote_count != len(pairs) or not exact or min(znccs) < 0.98:
            supported = False
        aggregate.append(
            {
                "mode12_plane": plane,
                "mode3_set": mapping[0],
                "mode3_plane": mapping[1],
                "votes": vote_count,
                "observations": len(pairs),
                "zncc_min": min(znccs),
                "zncc_mean": float(np.mean(znccs)),
                "zncc_max": max(znccs),
                "exact_half_scale_identity": exact,
            }
        )

    return {
        "observation_count": len(observations),
        "pairs": aggregate,
        "simple_model": {
            "supported": supported,
            "model": "mode3_visible_to_mode12_visible_half_resolution_same_order" if supported else None,
        },
    }


def prep_visible(image: np.ndarray) -> np.ndarray:
    return cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(image)


def prep_tracking(image: np.ndarray) -> np.ndarray:
    f = np.log1p(image.astype(np.float32))
    f = (255.0 * (f - f.min()) / (f.max() - f.min() + 1e-6)).astype(np.uint8)
    return cv2.createCLAHE(clipLimit=2.0, tileGridSize=(8, 8)).apply(f)


def sift_matches(src: np.ndarray, dst: np.ndarray, ratio: float = 0.72) -> tuple[np.ndarray, np.ndarray]:
    sift = cv2.SIFT_create(nfeatures=5000, contrastThreshold=0.01, edgeThreshold=10)
    a = prep_visible(src)
    b = prep_tracking(dst)
    kp1, des1 = sift.detectAndCompute(a, None)
    kp2, des2 = sift.detectAndCompute(b, None)
    if des1 is None or des2 is None:
        return np.empty((0, 2), np.float32), np.empty((0, 2), np.float32)
    matches = cv2.BFMatcher().knnMatch(des1, des2, k=2)
    good = [m for m, n in matches if m.distance < ratio * n.distance]
    return (
        np.float32([kp1[m.queryIdx].pt for m in good]),
        np.float32([kp2[m.trainIdx].pt for m in good]),
    )


def affine_inliers(src_points: np.ndarray, dst_points: np.ndarray) -> tuple[int, np.ndarray | None, np.ndarray | None]:
    if len(src_points) < 4:
        return 0, None, None
    matrix, mask = cv2.estimateAffine2D(
        src_points,
        dst_points,
        method=cv2.RANSAC,
        ransacReprojThreshold=1.5,
        maxIters=20000,
        confidence=0.999,
        refineIters=20,
    )
    if matrix is None or mask is None:
        return 0, None, None
    return int(mask.sum()), matrix, mask


def analyze_mode12_bridge(root: Path, survey: dict) -> dict:
    plan = survey.get("capture_plan", [])
    visits = [i for i, mode in enumerate(plan) if mode == 12]
    visit_assignments = []
    matched_points: dict[tuple[int, int], list[tuple[np.ndarray, np.ndarray]]] = defaultdict(list)

    for visit in visits:
        visible = mode12_visit(root, visit, 8, 0)
        tracking = mode12_visit(root, visit, 9, 0)
        if len(visible) != 4 or len(tracking) != 4:
            continue
        scores = {i: {} for i in range(4)}
        point_cache = {}
        for vis_plane in range(4):
            for track_plane in range(4):
                p1, p2 = sift_matches(visible[vis_plane], tracking[track_plane])
                inliers, _, _ = affine_inliers(p1, p2)
                scores[vis_plane][track_plane] = {"inliers": inliers}
                point_cache[(vis_plane, track_plane)] = (p1, p2)
        total, pairs = unique_assignment(scores, value_key="inliers")
        visit_assignments.append(
            {"visit": visit, "total_inliers": total, "pairs": [{"visible_plane": a, "tracking_plane": b, "inliers": scores[a][b]["inliers"]} for a, b in pairs]}
        )
        for pair in pairs:
            matched_points[pair].append(point_cache[pair])

    consensus = {}
    for vis_plane in range(4):
        votes = Counter()
        for assignment in visit_assignments:
            for pair in assignment["pairs"]:
                if pair["visible_plane"] == vis_plane:
                    votes[pair["tracking_plane"]] += 1
        if votes:
            consensus[vis_plane] = votes.most_common(1)[0][0]

    affine_models = []
    for vis_plane, track_plane in sorted(consensus.items()):
        all_src = []
        all_dst = []
        for visit in visits:
            visible = mode12_visit(root, visit, 8, 0)
            tracking = mode12_visit(root, visit, 9, 0)
            if vis_plane not in visible or track_plane not in tracking:
                continue
            p1, p2 = sift_matches(visible[vis_plane], tracking[track_plane])
            all_src.append(p1)
            all_dst.append(p2)
        if not all_src:
            continue
        src = np.vstack(all_src)
        dst = np.vstack(all_dst)
        inlier_count, matrix, mask = affine_inliers(src, dst)
        if matrix is None or mask is None:
            continue
        predicted = cv2.transform(src.reshape(-1, 1, 2), matrix).reshape(-1, 2)
        errors = np.linalg.norm(predicted - dst, axis=1)
        inlier_mask = mask.ravel().astype(bool)
        affine_models.append(
            {
                "visible_plane": vis_plane,
                "tracking_plane": track_plane,
                "matches": int(len(src)),
                "inliers": inlier_count,
                "matrix_visible_to_tracking": matrix.tolist(),
                "inlier_error_median_px": float(np.median(errors[inlier_mask])),
                "inlier_error_p95_px": float(np.percentile(errors[inlier_mask], 95)),
            }
        )

    identity_order = len(consensus) == 4 and all(consensus.get(i) == i for i in range(4))
    return {
        "visit_assignments": visit_assignments,
        "identity_order_supported": identity_order,
        "affine_bootstrap_models": affine_models,
        "note": (
            "These cross-spectral affine fits are a bootstrap relationship only. Residual lens/readout non-linearity must be "
            "measured during calibration before treating them as an exact intrinsic-coordinate transform."
        ),
    }


def analyze_burst(root: Path, survey: dict) -> dict:
    return {
        "format": "psvr2-cross-mode-registration-v2",
        "source": str(root),
        "visible_mode3_to_mode12": analyze_visible_burst(root, survey),
        "mode12_visible_to_tracking": analyze_mode12_bridge(root, survey),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_dir", help="Stationary camera survey/burst directory")
    parser.add_argument("--output", default="psvr2-cross-mode-registration.json")
    args = parser.parse_args()

    root = Path(args.capture_dir)
    survey_path = root / "survey.json"
    if not survey_path.exists():
        raise SystemExit(f"Missing {survey_path}")
    survey = json.loads(survey_path.read_text())
    if not survey.get("sequence_capture"):
        raise SystemExit("This version expects a v2/v3 stationary --sequence capture (recommended: 3,12,3 --repeat 3).")

    report = analyze_burst(root, survey)
    Path(args.output).write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

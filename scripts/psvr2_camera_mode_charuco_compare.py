#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Offline native-pixel correspondence for stationary mode-survey captures.

Each root is one fixed board pose, with repeated mode-4/16 visits. Reuse the
existing public ChArUco target/detector. No runtime calibration is modified.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np
from psvr2_tracking_charuco_direct import board_detector


def fit_affine(x, y):
    design = np.column_stack([x, np.ones(len(x))])
    if len(x) < 6 or np.linalg.matrix_rank(design) < 3:
        raise ValueError('insufficient non-collinear correspondences')
    matrix = np.linalg.lstsq(design, y, rcond=None)[0]
    return matrix, np.linalg.norm(design @ matrix - y, axis=1)


def stats(errors):
    return dict(rms_px=float(np.sqrt(np.mean(errors ** 2))),
                median_px=float(np.median(errors)), p95_px=float(np.percentile(errors, 95)))


def detect_best(active, detector):
    best = (0, None, None, 'native')
    for treatment, gain in [('native', 1), ('gain-8', 8), ('gain-32', 32), ('gain-64', 64)]:
        variant = np.clip(active.astype(float)*gain, 0, 255).astype('uint8')
        corners, ids, _, _ = detector.detectBoard(variant)
        count = 0 if ids is None else len(ids)
        if count > best[0]:
            best = count, corners, ids, treatment
    return best


def collect(root, detector, mean_frames=False):
    points, records, accumulated, counts = {}, [], {}, {}
    for manifest in sorted(root.glob('*packets.csv')):
        for row in csv.DictReader(manifest.open()):
            for filename in row['decoded_files'].split(';'):
                if not filename:
                    continue
                mode = 16 if 'mode-10-' in filename else 4 if 'mode-04-' in filename else None
                if mode is None:
                    continue
                plane = int(Path(filename).stem.rsplit('plane', 1)[1])
                camera = plane if mode == 16 else {(4, 0): 0, (4, 1): 1, (5, 0): 2, (5, 1): 3}[(int(row['camera_set']), plane)]
                path = root / filename
                image = cv2.imread(str(path), 0)
                if image is None:
                    raise ValueError(f'unreadable image: {path}')
                # Only documented mode-4 transport padding is excluded.
                active = image[:, :508] if mode == 4 else image
                # Prior native calibration needed contrast scaling for its dark
                # exposures. Detection variants only; saved images are untouched.
                count, corners, ids, treatment = detect_best(active, detector)
                if mean_frames:
                    key = (mode, camera)
                    if key not in accumulated:
                        accumulated[key] = np.zeros(active.shape, dtype=float)
                        counts[key] = 0
                    accumulated[key] += active
                    counts[key] += 1
                records.append(dict(root=str(root), file=filename, mode=mode, camera=camera,
                                    sequence_id=int(row['sequence_id']), vts_us=int(row['vts_us']),
                                    corners=count, detection_treatment=treatment,
                                    native_active_mean_dn=float(active.mean()),
                                    native_active_p99_dn=float(np.percentile(active, 99)),
                                    sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
                if count:
                    for ident, xy in zip(ids.ravel(), corners.reshape(-1, 2)):
                        points.setdefault((mode, camera, int(ident)), []).append(xy)
    medians = {key: np.median(value, axis=0) for key, value in points.items()}
    jitter = {str(key): stats(np.linalg.norm(np.asarray(value)-medians[key], axis=1)) for key, value in points.items()}
    averaged = []
    if mean_frames:
        medians = {}
        for (mode, camera), summed in accumulated.items():
            count, corners, ids, treatment = detect_best(summed/counts[(mode, camera)], detector)
            averaged.append(dict(mode=mode, camera=camera, frame_count=counts[(mode, camera)],
                                 corners=count, detection_treatment=treatment, accepted=count >= 8))
            if count >= 8:
                for ident, xy in zip(ids.ravel(), corners.reshape(-1, 2)):
                    medians[(mode, camera, int(ident))] = xy
    return medians, records, jitter, averaged


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('roots', nargs='+', type=Path)
    p.add_argument('--out', required=True, type=Path)
    p.add_argument('--mean-frames', action='store_true', help='separate stationary averaging treatment, like prior native calibration; require eight corners per view')
    args = p.parse_args()
    out = args.out.expanduser().resolve()
    if out.exists() or str(out).startswith(('/tmp/', '/private/tmp/')):
        p.error('use a new persistent output directory')
    _, detector = board_detector()
    poses, records, jitter, averaged = [], [], {}, {}
    for root in args.roots:
        root = root.expanduser().resolve()
        points, images, spread, means = collect(root, detector, args.mean_frames)
        poses.append(points)
        records.extend(images)
        jitter[str(root)] = spread
        averaged[str(root)] = means
    fits = []
    for source in range(4):
        for target in range(2):
            x, y, groups, matches = [], [], [], []
            for pose, points in enumerate(poses):
                ids = sorted({key[2] for key in points if key[:2] == (4, source)} &
                             {key[2] for key in points if key[:2] == (16, target)})
                for ident in ids:
                    a, b = points[(4, source, ident)], points[(16, target, ident)]
                    x.append(a); y.append(b); groups.append(pose)
                    matches.append(dict(pose=pose, corner_id=ident, mode4_xy=a.tolist(), mode16_xy=b.tolist()))
            if len(x) < 6:
                continue
            x, y, groups = np.asarray(x), np.asarray(y), np.asarray(groups)
            matrix, errors = fit_affine(x, y)
            held = []
            if len(set(groups)) >= 3:
                for pose in sorted(set(groups)):
                    mask = groups == pose
                    try:
                        train, _ = fit_affine(x[~mask], y[~mask])
                    except ValueError:
                        continue
                    held.append(dict(pose=int(pose), **stats(np.linalg.norm(np.column_stack([x[mask], np.ones(mask.sum())]) @ train-y[mask], axis=1))))
            fits.append(dict(mode4_camera=source, mode16_view=target, matches=matches,
                             affine_row_vector_matrix=matrix.tolist(), fit=stats(errors),
                             exact_double=stats(np.linalg.norm(2*x-y, axis=1)),
                             double_plus_half_pixel=stats(np.linalg.norm(2*x+.5-y, axis=1)),
                             held_pose_out=held))
    out.mkdir(parents=True)
    result = dict(roots=[str(p.resolve()) for p in args.roots], images=records,
                  corner_repeatability=jitter, candidate_mappings=fits,
                  coordinate_source='stationary_mean_images' if args.mean_frames else 'median_individual_frame_corners',
                  averaged_detections=averaged,
                  limitations=['Sequential modes: scene must stay fixed within each root.',
                               'An affine fit on one planar pose cannot establish camera identity or full calibration.',
                               'Inspect held-pose errors and spatial coverage before transferring intrinsics.',
                               'Contrast scaling is labelled detection-only; native saved images remain unamplified.'])
    (out/'comparison.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(dict(images=len(records), detected=sum(r['corners'] > 0 for r in records),
                         mappings=[{k: f[k] for k in ('mode4_camera','mode16_view','fit','exact_double','held_pose_out')} for f in fits])))


if __name__ == '__main__':
    main()

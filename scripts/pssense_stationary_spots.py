#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Offline fixed image-spot measurements, with no physical/semantic LED IDs.

Consume one stationary orientation's validated summary. Reuse the existing
mask diagnostic's component detector; compare each trial to its preceding
PRESCAN control. No temporal-slot or physical-selection conclusion is assumed.
"""
import argparse
import csv
import json
from pathlib import Path

import cv2
import numpy as np
from scipy.optimize import linear_sum_assignment
from psvr2_tracking_mask_analyze import compact_bright_centroids


def match_spots(reference, observed, radius=4.):
    matched = np.zeros(len(reference), dtype=bool)
    if len(reference) and len(observed):
        distance = np.linalg.norm(np.asarray(reference)[:, None]-np.asarray(observed)[None, :], axis=2)
        rows, cols = linear_sum_assignment(distance)
        matched[rows[distance[rows, cols] <= radius]] = True
    return matched


def images(run, stage):
    directory = Path(run['reports']).parent / f"{stage['index']:02d}-{stage['label']}"
    roi = run['arguments']['off_roi']
    x0, y0, x1, y1 = roi
    headers = {}
    for manifest in directory.glob('*packets.csv'):
        for row in csv.DictReader(manifest.open()):
            for name in row['decoded_files'].split(';'):
                headers[name] = row
    for frame in stage['camera0_roi_frames']:
        name = frame['file']
        image = cv2.imread(str(directory/name), 0)
        if image is None:
            raise ValueError(f'unreadable image: {directory/name}')
        yield name, image[y0:y1, x0:x1], headers[name]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('summary', type=Path)
    p.add_argument('--out', required=True, type=Path)
    args = p.parse_args()
    out = args.out.expanduser().resolve()
    if out.exists() or str(out).startswith(('/tmp/', '/private/tmp/')):
        p.error('use a new persistent output directory')
    source = json.loads(args.summary.read_text())
    selected = {}
    for run in source['runs']:
        for stage in run['stages']:
            if stage['accepted']:
                selected[stage['plan_index']] = run, stage
    anchor_index = next(i for i in sorted(selected) if i % 3 == 1)
    run, anchor = selected[anchor_index]
    candidates = [compact_bright_centroids(image, 100) for _, image, _ in images(run, anchor)]
    reference = sorted(max(candidates, key=len), key=lambda xy: (xy[0], xy[1]))
    if not reference:
        raise ValueError('no reference image spots in accepted PRESCAN')
    records, stages = [], {}
    for index, (run, stage) in sorted(selected.items()):
        occupancies, peaks, sums = [], [], []
        for name, image, header in images(run, stage):
            observed = compact_bright_centroids(image, 100)
            matched = match_spots(reference, observed)
            peak, integrated = [], []
            for x, y in reference:
                x, y = round(x), round(y)
                patch = image[max(0,y-3):y+4, max(0,x-3):x+4]
                peak.append(int(patch.max()))
                integrated.append(int(patch.sum()))
            occupancies.append(matched); peaks.append(peak); sums.append(integrated)
            records.append(dict(plan_index=index, label=stage['label'], file=name,
                                source=str(Path(run['reports']).parent), sequence_id=int(header['sequence_id']),
                                vts_us=int(header['vts_us']), host_monotonic_ns=int(header['host_monotonic_ns']),
                                observed_components=len(observed), matched=matched.tolist(), peak_dn=peak,
                                patch_sum_dn=integrated))
        stages[index] = dict(label=stage['label'], frames=len(peaks),
                             spot_occupancy=np.mean(occupancies,axis=0).tolist(),
                             median_peak_dn=np.median(peaks,axis=0).tolist(),
                             median_patch_sum_dn=np.median(sums,axis=0).tolist())
    trials = []
    for index, stage in sorted(stages.items()):
        if index % 3 != 2 or index-1 not in stages:
            continue
        anchor = stages[index-1]
        trials.append(dict(plan_index=index, **stage, preceding_prescan=anchor,
                           peak_ratio_to_prescan=(np.asarray(stage['median_peak_dn']) /
                               np.maximum(anchor['median_peak_dn'], 1)).tolist(),
                           patch_sum_ratio_to_prescan=(np.asarray(stage['median_patch_sum_dn']) /
                               np.maximum(anchor['median_patch_sum_dn'], 1)).tolist()))
    out.mkdir(parents=True)
    roi = run['arguments']['off_roi']
    result = dict(source_summary=str(args.summary.resolve()), reference_plan_index=anchor_index,
                  camera=0, roi=roi, reference_spots_roi_xy=reference, trials=trials,
                  limitations=['Spot indices label fixed image locations only, not physical LEDs or Sony semantic outputs.',
                               'Template comes from the first accepted PRESCAN; additional visible spots are not assigned identities.',
                               'DN thresholds and saturation can merge or hide components; inspect peak measurements too.',
                               'Mode-4 exposure and selected frames can alias temporal waveforms.',
                               'Phase calibration treatment differs across resumed first-orientation batches.',
                               'Compare repeated trials and orientations before attributing changes to waveform settings.'])
    (out/'spots.json').write_text(json.dumps(result,indent=2)+'\n')
    with (out/'frames.jsonl').open('x') as f:
        for row in records:
            f.write(json.dumps(row)+'\n')
    print(json.dumps(dict(out=str(out), image_spots=len(reference), trials=len(trials), frames=len(records))))


if __name__ == '__main__':
    main()

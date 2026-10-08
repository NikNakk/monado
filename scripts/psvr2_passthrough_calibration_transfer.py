#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Transfer mode-4 lower-camera geometry to an experimental BC4 passthrough file.

Requires numpy and scipy. Does not overwrite a candidate or edit the source.
This preserves the experimental status; it is not a camera/head calibration solve.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from scipy.spatial.transform import Rotation


def pose_matrix(pose):
    q = [pose['orientation'][k] for k in 'xyzw']
    t = [pose['position'][k] for k in 'xyz']
    if not np.all(np.isfinite(q + t)) or abs(np.linalg.norm(q) - 1) > 1e-4:
        raise ValueError('invalid source pose')
    matrix = np.eye(4)
    matrix[:3, :3] = Rotation.from_quat(q).as_matrix()
    matrix[:3, 3] = t
    return matrix


def transfer(source, source_bytes):
    if source['format'] != 'psvr2-mode4-constellation-calibration-v1':
        raise ValueError('expected mode-4 constellation calibration v1')
    if source['headset_serial'] is not None and (not isinstance(source['headset_serial'], str) or not source['headset_serial']):
        raise ValueError('invalid source headset serial')
    cameras = {c['camera']: c for c in source['cameras']}
    if len(cameras) != 4 or set(cameras) != {0, 1, 2, 3}:
        raise ValueError('expected four unique source cameras')
    head_from_rig = pose_matrix(source['head_from_camera0_xrt'])
    result = dict(format='psvr2-passthrough-calibration-v1', runtime_usable=False,
                  projection='rotation-only', headset_serial=source['headset_serial'],
                  status='experimental: camera-to-head alignment and latency unvalidated',
                  provenance=dict(source_sha256=hashlib.sha256(source_bytes).hexdigest(),
                                  source_runtime_usable=source.get('runtime_usable'),
                                  transfer='lower cameras 0/1; fx/fy *= 2; cx/cy = 2*cx/cy + 0.5'),
                  cameras=[])
    for view in range(2):
        camera = cameras[view]
        cal = camera['calibration']
        if cal['resolution'] != dict(width=512, height=508) or cal['model'] != 'fisheye_equidistant4':
            raise ValueError('expected 512x508 equidistant4 source')
        intrinsics = {k: 2 * cal['intrinsics'][k] + (0.5 if k in ('cx', 'cy') else 0)
                      for k in ('fx', 'fy', 'cx', 'cy')}
        distortion = {k: cal['distortion'][k] for k in ('k1', 'k2', 'k3', 'k4')}
        if not np.all(np.isfinite(list(intrinsics.values()) + list(distortion.values()))):
            raise ValueError('non-finite camera calibration')
        if intrinsics['fx'] <= 0 or intrinsics['fy'] <= 0:
            raise ValueError('invalid focal length')
        head_from_camera = head_from_rig @ pose_matrix(camera['pose_in_tracking_origin_xrt'])
        quaternion = Rotation.from_matrix(head_from_camera[:3, :3]).as_quat()
        pose = dict(orientation=dict(zip('xyzw', quaternion.tolist())),
                    position=dict(zip('xyz', head_from_camera[:3, 3].tolist())))
        result['cameras'].append(dict(view=view, width=1024, height=1016,
                                      model='fisheye_equidistant4', intrinsics=intrinsics,
                                      distortion=distortion, head_from_camera_xrt=pose))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    data = args.source.read_bytes()
    candidate = transfer(json.loads(data), data)
    with args.output.open('x') as stream:
        json.dump(candidate, stream, indent=2, allow_nan=False)
        stream.write('\n')
    print(f'Wrote experimental rotation-only candidate: {args.output}')


if __name__ == '__main__':
    main()

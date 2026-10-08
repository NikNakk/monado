# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
import copy
import sys
import unittest
from pathlib import Path

import numpy as np
from scipy.spatial.transform import Rotation

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from psvr2_passthrough_calibration_transfer import transfer, pose_matrix


def pose(angle, position):
    return dict(orientation=dict(zip('xyzw', Rotation.from_euler('y', angle).as_quat().tolist())),
                position=dict(zip('xyz', position)))


def source():
    cameras = [dict(camera=i, calibration=dict(resolution=dict(width=512, height=508),
                   model='fisheye_equidistant4', intrinsics=dict(fx=190, fy=191, cx=250, cy=251),
                   distortion=dict(k1=.03, k2=-.01, k3=.005, k4=-.001)),
                   pose_in_tracking_origin_xrt=pose(.2 * i, [.08 * i, 0, -.02 * i])) for i in range(4)]
    return dict(format='psvr2-mode4-constellation-calibration-v1', runtime_usable=False,
                headset_serial=None, head_from_camera0_xrt=pose(-.4, [-.04, -.02, -.1]), cameras=cameras)


class TransferTests(unittest.TestCase):
    def test_transfer_scale_pose_composition_and_experimental_status(self):
        original = source()
        before = copy.deepcopy(original)
        result = transfer(original, b'source')
        self.assertEqual(original, before)
        self.assertFalse(result['runtime_usable'])
        self.assertIsNone(result['headset_serial'])
        self.assertEqual(result['projection'], 'rotation-only')
        for view, camera in enumerate(result['cameras']):
            self.assertEqual(camera['intrinsics'], dict(fx=380, fy=382, cx=500.5, cy=502.5))
            self.assertEqual(camera['distortion'], original['cameras'][view]['calibration']['distortion'])
            expected = pose_matrix(original['head_from_camera0_xrt']) @ pose_matrix(original['cameras'][view]['pose_in_tracking_origin_xrt'])
            np.testing.assert_allclose(pose_matrix(camera['head_from_camera_xrt']), expected, atol=1e-12)

    def test_reject_wrong_mode_duplicate_camera_and_bad_pose(self):
        for mutation in ('format', 'resolution', 'duplicate', 'pose'):
            data = source()
            if mutation == 'format':
                data['format'] = 'unknown'
            elif mutation == 'resolution':
                data['cameras'][0]['calibration']['resolution']['width'] = 508
            elif mutation == 'duplicate':
                data['cameras'][1]['camera'] = 0
            else:
                data['head_from_camera0_xrt']['orientation']['w'] = 0
            with self.assertRaises(ValueError):
                transfer(data, b'source')


if __name__ == '__main__':
    unittest.main()

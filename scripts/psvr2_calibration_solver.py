#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Offline PSVR2 four-camera fisheye, rig, and SLAM calibration solver.

Transform names use ``T_A_B`` throughout: a homogeneous matrix which maps a
point expressed in frame B into frame A.  OpenCV ``solvePnP`` therefore returns
``T_camera_board``.  The rig frame is camera 0.
"""

from __future__ import annotations

import csv
import heapq
import json
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

import cv2
import numpy as np

CAMERA_COUNT = 4
SCHEMA_VERSION = 1


def fisheye_flag(name):
    """OpenCV 4 exposes fisheye flags in cv2.fisheye; OpenCV 5 moved them."""
    if hasattr(cv2.fisheye, name):
        return getattr(cv2.fisheye, name)
    return getattr(cv2, name)


def transform(R=None, t=None):
    T = np.eye(4, dtype=np.float64)
    if R is not None:
        T[:3, :3] = np.asarray(R, dtype=np.float64).reshape(3, 3)
    if t is not None:
        T[:3, 3] = np.asarray(t, dtype=np.float64).reshape(3)
    return T


def compose(T_A_B, T_B_C):
    """Return T_A_C = T_A_B @ T_B_C."""
    return np.asarray(T_A_B) @ np.asarray(T_B_C)


def invert(T_A_B):
    T_A_B = np.asarray(T_A_B, dtype=np.float64)
    R = T_A_B[:3, :3]
    return transform(R.T, -R.T @ T_A_B[:3, 3])


def quat_xyzw_to_matrix(q):
    q = np.asarray(q, dtype=np.float64).reshape(4)
    q /= np.linalg.norm(q)
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y*y + z*z), 2 * (x*y - z*w), 2 * (x*z + y*w)],
        [2 * (x*y + z*w), 1 - 2 * (x*x + z*z), 2 * (y*z - x*w)],
        [2 * (x*z - y*w), 2 * (y*z + x*w), 1 - 2 * (x*x + y*y)],
    ], dtype=np.float64)


def matrix_to_quat_xyzw(R):
    R = np.asarray(R, dtype=np.float64)
    # Eigenvector of Davenport's symmetric matrix, ordered x,y,z,w.
    K = np.array([
        [R[0,0]-R[1,1]-R[2,2], R[0,1]+R[1,0], R[0,2]+R[2,0], R[2,1]-R[1,2]],
        [R[0,1]+R[1,0], R[1,1]-R[0,0]-R[2,2], R[1,2]+R[2,1], R[0,2]-R[2,0]],
        [R[0,2]+R[2,0], R[1,2]+R[2,1], R[2,2]-R[0,0]-R[1,1], R[1,0]-R[0,1]],
        [R[2,1]-R[1,2], R[0,2]-R[2,0], R[1,0]-R[0,1], R.trace()],
    ]) / 3.0
    values, vectors = np.linalg.eigh(K)
    q = vectors[:, np.argmax(values)]
    if q[3] < 0:
        q = -q
    return q / np.linalg.norm(q)


def rotation_angle_deg(R):
    return math.degrees(math.acos(float(np.clip((np.trace(R) - 1.0) / 2.0, -1.0, 1.0))))


def rotation_mean(rotations):
    quats = [matrix_to_quat_xyzw(R) for R in rotations]
    anchor = quats[0]
    quats = [q if np.dot(q, anchor) >= 0 else -q for q in quats]
    return quat_xyzw_to_matrix(np.mean(quats, axis=0))


def matrix_json(T):
    return [[float(v) for v in row] for row in np.asarray(T)]


def stats(values):
    a = np.asarray(values, dtype=np.float64)
    if not len(a):
        return {"count": 0}
    return {
        "count": int(len(a)), "mean": float(np.mean(a)), "median": float(np.median(a)),
        "p95": float(np.percentile(a, 95)), "max": float(np.max(a)),
    }


@dataclass
class Observation:
    dataset: int
    set_index: int
    sequence_id: int
    camera: int
    ids: np.ndarray
    image_points: np.ndarray

    @property
    def key(self):
        return self.dataset, self.set_index


@dataclass
class Dataset:
    path: Path
    metadata: dict
    manifest: dict[int, dict]


def _csv(path):
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def load_inputs(paths: Iterable[Path], min_corners: int):
    datasets, observations = [], {}
    for dataset_index, path in enumerate(paths):
        metadata = json.loads((path / "dataset.json").read_text())
        if int(metadata.get("camera_mode", -1)) != 3 or int(metadata.get("camera_count", -1)) != 4:
            raise ValueError(f"{path}: expected a mode-3 four-camera dataset")
        manifest_rows = _csv(path / "manifest.csv")
        manifest = {int(r["set_index"]): r for r in manifest_rows}
        datasets.append(Dataset(path, metadata, manifest))
        grouped = defaultdict(list)
        for r in _csv(path / "charuco-detections.csv"):
            grouped[(int(r["set_index"]), int(r["sequence_id"]), int(r["camera"]))].append(r)
        for (set_index, sequence_id, camera), rows in grouped.items():
            if len(rows) < min_corners:
                continue
            rows.sort(key=lambda r: int(r["corner_id"]))
            observations[(dataset_index, set_index, camera)] = Observation(
                dataset_index, set_index, sequence_id, camera,
                np.array([int(r["corner_id"]) for r in rows], dtype=np.int32),
                np.array([[float(r["x"]), float(r["y"])] for r in rows], dtype=np.float64),
            )
    return datasets, observations


def board_points(squares_x, squares_y, square_length_m):
    return np.array([
        [(x + 1) * square_length_m, (y + 1) * square_length_m, 0.0]
        for y in range(squares_y - 1) for x in range(squares_x - 1)
    ], dtype=np.float64)


def object_points(obs, corners):
    if np.any(obs.ids < 0) or np.any(obs.ids >= len(corners)):
        raise ValueError("ChArUco corner ID outside target range")
    return corners[obs.ids]


def project_residual(obs, corners, K, D, rvec, tvec):
    predicted, _ = cv2.fisheye.projectPoints(
        object_points(obs, corners).reshape(1, -1, 3), rvec, tvec, K, D)
    errors = np.linalg.norm(predicted.reshape(-1, 2) - obs.image_points, axis=1)
    return float(np.sqrt(np.mean(errors**2))), errors


def calibrate_camera(camera, obs_list, corners, size):
    if len(obs_list) < 8:
        raise ValueError(f"camera {camera}: only {len(obs_list)} strong views")
    # 1xN multi-channel layout works in both OpenCV 4 and the OpenCV 5
    # fisheye bindings (OpenCV 5 currently rejects the equivalent Nx1 layout).
    object_sets = [object_points(o, corners).reshape(1, -1, 3) for o in obs_list]
    image_sets = [o.image_points.reshape(1, -1, 2) for o in obs_list]
    flags = fisheye_flag("CALIB_RECOMPUTE_EXTRINSIC") | fisheye_flag("CALIB_FIX_SKEW")
    criteria = (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_MAX_ITER, 200, 1e-10)

    def fit(indices):
        K = np.array([[size[0] * .54, 0, size[0] / 2], [0, size[1] * .54, size[1] / 2], [0, 0, 1]], np.float64)
        D = np.zeros((4, 1), np.float64)
        rms, K, D, rvecs, tvecs = cv2.fisheye.calibrate(
            [object_sets[i] for i in indices], [image_sets[i] for i in indices], size, K, D,
            flags=flags, criteria=criteria)
        residuals = [project_residual(obs_list[i], corners, K, D, r, t)[0]
                     for i, r, t in zip(indices, rvecs, tvecs)]
        return float(rms), K, D, rvecs, tvecs, residuals

    all_indices = list(range(len(obs_list)))
    first = fit(all_indices)
    med = float(np.median(first[5])); mad = float(np.median(np.abs(np.asarray(first[5]) - med)))
    threshold = max(1.0, med + 4.0 * 1.4826 * mad)
    candidates = [i for i, value in enumerate(first[5]) if value > threshold]
    # Conservative: one pass, never discard more than 10%, and retain at least 12 views.
    max_reject = min(len(candidates), len(obs_list) // 10, max(0, len(obs_list) - 12))
    rejected = set(sorted(candidates, key=lambda i: first[5][i], reverse=True)[:max_reject])
    accepted = [i for i in all_indices if i not in rejected]
    final = fit(accepted) if rejected else first
    per_view = []
    final_by_index = {i: (r, t, e) for i, r, t, e in zip(accepted, final[3], final[4], final[5])}
    for i, obs in enumerate(obs_list):
        entry = {"dataset": obs.dataset, "set_index": obs.set_index, "corner_count": int(len(obs.ids)),
                 "accepted": i in final_by_index}
        if i in final_by_index:
            r, t, e = final_by_index[i]
            entry.update(rms_px=float(e), rvec=[float(x) for x in r.reshape(3)],
                         tvec_m=[float(x) for x in t.reshape(3)])
        else:
            entry["initial_rms_px"] = float(first[5][i])
        per_view.append(entry)

    points = np.concatenate([o.image_points for o in obs_list])
    hull_area = cv2.contourArea(cv2.convexHull(points.astype(np.float32))) / (size[0] * size[1])
    occupied = len({(min(3, int(p[0] * 4 / size[0])), min(3, int(p[1] * 4 / size[1]))) for p in points})
    normals = []
    depths = []
    for r, t in zip(final[3], final[4]):
        R, _ = cv2.Rodrigues(r)
        normals.append(math.degrees(math.acos(float(np.clip(abs(R[2,2]), 0.0, 1.0)))))
        depths.append(float(np.asarray(t).reshape(3)[2]))
    warnings = []
    if len(accepted) < 25: warnings.append("fewer than 25 accepted views")
    if hull_area < .25: warnings.append("image-plane convex-hull coverage below 25%")
    if occupied < 10: warnings.append("fewer than 10 of 16 image coverage cells occupied")
    return {
        "width": size[0], "height": size[1], "K": final[1], "D": final[2].reshape(4),
        "rms": final[0], "accepted": len(accepted), "rejected": len(rejected),
        "per_view": per_view,
        "coverage": {"convex_hull_fraction": float(hull_area), "grid_4x4_cells": occupied,
                     "board_normal_angle_deg": stats(normals), "depth_m": stats(depths),
                     "K_condition_number": float(np.linalg.cond(final[1]))},
        "warnings": warnings,
    }


def common_points(a, b, corners):
    ia = {int(v): i for i, v in enumerate(a.ids)}
    ib = {int(v): i for i, v in enumerate(b.ids)}
    ids = sorted(set(ia) & set(ib))
    return (corners[ids].reshape(1, -1, 3),
            a.image_points[[ia[i] for i in ids]].reshape(1, -1, 2),
            b.image_points[[ib[i] for i in ids]].reshape(1, -1, 2), ids)


def pair_extrinsic(a, b, observations, corners, intrinsics, size, min_common):
    objects, images_a, images_b, keys = [], [], [], []
    frame_keys = sorted({(d, s) for d, s, c in observations if c == a} &
                        {(d, s) for d, s, c in observations if c == b})
    for d, s in frame_keys:
        obj, pa, pb, ids = common_points(observations[(d,s,a)], observations[(d,s,b)], corners)
        if len(ids) >= min_common:
            objects.append(obj); images_a.append(pa); images_b.append(pb); keys.append((d,s))
    if len(objects) < 3:
        return None
    ia, ib = intrinsics[a], intrinsics[b]
    # Calibrate the rigid pair in the normalized rectilinear plane after
    # fisheye-aware undistortion. This is mathematically equivalent to fixing
    # identity pinhole intrinsics, and avoids OpenCV 5's broken Python wrapper
    # for fisheye.stereoCalibrate with variable-size ChArUco observations.
    norm_a = [cv2.fisheye.undistortPoints(p, ia["K"], ia["D"].reshape(4,1)).astype(np.float32)
              for p in images_a]
    norm_b = [cv2.fisheye.undistortPoints(p, ib["K"], ib["D"].reshape(4,1)).astype(np.float32)
              for p in images_b]
    objects32 = [p.astype(np.float32) for p in objects]
    flags = cv2.CALIB_FIX_INTRINSIC
    criteria = (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_MAX_ITER, 200, 1e-9)
    rms_norm, _K1, _D1, _K2, _D2, R, t, _E, _F = cv2.stereoCalibrate(
        objects32, norm_a, norm_b, np.eye(3), None, np.eye(3), None, size,
        flags=flags, criteria=criteria)
    focal = math.sqrt(math.sqrt(ia["K"][0,0] * ia["K"][1,1] * ib["K"][0,0] * ib["K"][1,1]))
    rms = float(rms_norm * focal)
    return {"camera_a": a, "camera_b": b, "T_camera_b_camera_a": transform(R, t),
            "rms_px": float(rms), "frames": len(objects),
            "shared_corner_observations": int(sum(x.shape[1] for x in objects)),
            "baseline_m": float(np.linalg.norm(t)), "frame_keys": keys}


def build_rig(pairwise):
    graph = defaultdict(list)
    for edge in pairwise:
        a, b, T_b_a = edge["camera_a"], edge["camera_b"], edge["T_camera_b_camera_a"]
        weight = edge["rms_px"] / math.sqrt(edge["frames"])
        graph[a].append((weight, b, T_b_a)); graph[b].append((weight, a, invert(T_b_a)))
    result = {0: np.eye(4)}
    queue = [(0.0, 0, np.eye(4))]
    while queue:
        cost, camera, T_R_C = heapq.heappop(queue)
        if camera in result and camera != 0 and cost > result[camera][0]:
            continue
        if camera != 0: result[camera] = (cost, T_R_C)
        for weight, nxt, T_nxt_camera in graph[camera]:
            new = T_R_C @ invert(T_nxt_camera)
            if nxt not in result or (nxt != 0 and cost + weight < result[nxt][0]):
                heapq.heappush(queue, (cost + weight, nxt, new))
    if any(c not in result for c in range(CAMERA_COUNT)):
        raise ValueError("pairwise overlap graph does not connect all four cameras")
    T_R_C = {0: np.eye(4), **{c: result[c][1] for c in range(1, CAMERA_COUNT)}}
    closures = []
    for edge in pairwise:
        a, b = edge["camera_a"], edge["camera_b"]
        predicted = invert(T_R_C[b]) @ T_R_C[a]
        delta = invert(edge["T_camera_b_camera_a"]) @ predicted
        closures.append({"pair": f"{a}-{b}", "rotation_deg": rotation_angle_deg(delta[:3,:3]),
                         "translation_m": float(np.linalg.norm(delta[:3,3]))})
    return T_R_C, closures


def pnp_pose(obs, corners, intrinsic):
    undistorted = cv2.fisheye.undistortPoints(obs.image_points.reshape(-1,1,2), intrinsic["K"],
                                              intrinsic["D"].reshape(4,1))
    ok, rvec, tvec = cv2.solvePnP(object_points(obs, corners), undistorted, np.eye(3), None,
                                  flags=cv2.SOLVEPNP_ITERATIVE)
    if not ok: return None
    R, _ = cv2.Rodrigues(rvec)
    T = transform(R, tvec)
    predicted, _ = cv2.projectPoints(object_points(obs, corners), rvec, tvec, np.eye(3), None)
    rms = float(np.sqrt(np.mean(np.sum((predicted.reshape(-1,2)-undistorted.reshape(-1,2))**2, axis=1))))
    # Convert normalized-plane error to an approximate pixel residual.
    rms_px = rms * math.sqrt(intrinsic["K"][0,0] * intrinsic["K"][1,1])
    return T, rms_px


def solve_board_poses(observations, corners, intrinsics, T_R_C):
    grouped = defaultdict(list)
    for (d, s, c), obs in observations.items():
        solved = pnp_pose(obs, corners, intrinsics[c])
        if solved and solved[1] < 2.0:
            grouped[(d,s)].append((c, T_R_C[c] @ solved[0], solved[1], len(obs.ids)))
    poses, diagnostics = {}, []
    for key, candidates in grouped.items():
        rotations = [x[1][:3,:3] for x in candidates]
        translations = np.array([x[1][:3,3] for x in candidates])
        R = rotation_mean(rotations); t = np.median(translations, axis=0)
        rot_res = [rotation_angle_deg(R.T @ x) for x in rotations]
        trans_res = np.linalg.norm(translations - t, axis=1)
        if np.median(rot_res) > 2.0 or np.median(trans_res) > .02:
            continue
        poses[key] = transform(R, t)
        diagnostics.append({"dataset": key[0], "set_index": key[1], "camera_count": len(candidates),
                            "T_rig_board": matrix_json(poses[key]),
                            "rotation_residual_deg": stats(rot_res),
                            "translation_residual_m": stats(trans_res),
                            "camera_reprojection_rms_px": stats([x[2] for x in candidates])})
    return poses, diagnostics, {"candidate_frames": len(grouped),
                                "accepted_frames": len(poses),
                                "rejected_frames": len(grouped) - len(poses)}


def manifest_slam_pose(row):
    if int(row.get("slam_pose_valid", "0") or 0) == 0: return None
    p = [float(row[f"tracker_p{x}"]) for x in "xyz"]
    q = [float(row[f"tracker_q{x}"]) for x in "xyzw"]
    return transform(quat_xyzw_to_matrix(q), p)


def runtime_pose(raw, variant="raw"):
    Rz = quat_xyzw_to_matrix([0, 0, math.sqrt(.5), math.sqrt(.5)])
    corrected = transform(Rz @ raw[:3,:3], raw[:3,3]) # exact process_slam_record behavior
    if variant == "raw": return raw.copy()
    if variant == "runtime_tracker": return corrected
    if variant == "runtime_head":
        return corrected @ transform(t=[.000247, -.000273, .104826])
    if variant == "rigid_z_correction": return transform(Rz) @ raw
    if variant == "position_z_correction": return transform(raw[:3,:3], Rz @ raw[:3,3])
    raise ValueError(variant)


def fixed_board_residuals(slam_poses, board_poses, T_tracker_rig, groups=None):
    fixed = [S @ T_tracker_rig @ C for S, C in zip(slam_poses, board_poses)]
    groups = list(groups) if groups is not None else [0] * len(fixed)
    rot, trans, centers = [], [], {}
    for group in sorted(set(groups)):
        subset = [T for T, g in zip(fixed, groups) if g == group]
        Rmean = rotation_mean([T[:3,:3] for T in subset]); tmed = np.median([T[:3,3] for T in subset], axis=0)
        rot.extend(rotation_angle_deg(Rmean.T @ T[:3,:3]) for T in subset)
        trans.extend(float(np.linalg.norm(T[:3,3] - tmed)) for T in subset)
        centers[str(group)] = transform(Rmean, tmed)
    return fixed, stats(rot), stats(trans), centers


def per_group_fixed_board_diagnostics(slam_poses, board_poses, T_tracker_rig, groups):
    fixed = [S @ T_tracker_rig @ C for S, C in zip(slam_poses, board_poses)]
    result = {}
    for group in sorted(set(groups)):
        subset = [T for T, g in zip(fixed, groups) if g == group]
        Rmean = rotation_mean([T[:3,:3] for T in subset]); xyz = np.array([T[:3,3] for T in subset])
        tmed = np.median(xyz, axis=0)
        result[str(group)] = {
            "rotation_residual_deg": stats([rotation_angle_deg(Rmean.T @ T[:3,:3]) for T in subset]),
            "translation_residual_m": stats(np.linalg.norm(xyz - tmed, axis=1)),
            "fixed_board_axis_range_m": [float(x) for x in np.ptp(xyz, axis=0)],
            "fixed_board_first_to_last_m": [float(x) for x in (xyz[-1] - xyz[0])],
        }
    return result


def relative_motion_residuals(slam_poses, board_poses, T_tracker_rig, groups):
    rot, trans = [], []
    for stride in (1, 3, 9):
        for i in range(len(slam_poses) - stride):
            j = i + stride
            if groups[i] != groups[j]: continue
            A = invert(slam_poses[j]) @ slam_poses[i]
            B = board_poses[j] @ invert(board_poses[i])
            delta = invert(A @ T_tracker_rig) @ (T_tracker_rig @ B)
            rot.append(rotation_angle_deg(delta[:3,:3])); trans.append(np.linalg.norm(delta[:3,3]))
    return {"rotation_deg": stats(rot), "translation_m": stats(trans)}


def _rotation_vector(R):
    rvec, _ = cv2.Rodrigues(np.asarray(R, dtype=np.float64))
    return rvec.reshape(3)


def handeye_park_explicit(slam_poses, board_poses, groups=None):
    """Solve A X = X B using Park/Kabsch rotation plus linear translation.

    For the fixed-board geometry, A=inv(S_j)S_i and B=C_j inv(C_i), where
    S=T_slam_tracker and C=T_rig_board.  Keeping this construction here (and
    under a direction-sensitive synthetic test) avoids relying on undocumented
    OpenCV wrapper inversions.
    """
    alphas, betas, motions = [], [], []
    # Adjacent poses plus longer baselines reduce both O(n^2) cost and the
    # influence of tiny inter-frame rotations.
    groups = list(groups) if groups is not None else [0] * len(slam_poses)
    for stride in (1, 3, 9):
        for i in range(len(slam_poses) - stride):
            j = i + stride
            if groups[i] != groups[j]:
                continue
            A = invert(slam_poses[j]) @ slam_poses[i]
            B = board_poses[j] @ invert(board_poses[i])
            alpha, beta = _rotation_vector(A[:3,:3]), _rotation_vector(B[:3,:3])
            if np.linalg.norm(alpha) < math.radians(.15) or np.linalg.norm(beta) < math.radians(.15):
                continue
            alphas.append(alpha); betas.append(beta); motions.append((A, B))
    if len(motions) < 6:
        raise ValueError("insufficient rotational motion for hand-eye calibration")
    source, target = np.asarray(betas), np.asarray(alphas)
    U, _s, Vt = np.linalg.svd(source.T @ target)
    R = Vt.T @ U.T
    if np.linalg.det(R) < 0:
        Vt[-1, :] *= -1; R = Vt.T @ U.T
    lhs, rhs = [], []
    for A, B in motions:
        lhs.append(A[:3,:3] - np.eye(3)); rhs.append(R @ B[:3,3] - A[:3,3])
    t, _residuals, rank, _singular = np.linalg.lstsq(np.vstack(lhs), np.hstack(rhs), rcond=None)
    if rank < 3:
        raise ValueError("hand-eye translation system is rank deficient")
    return transform(R, t)


def handeye_once(slam_poses, board_poses, method=None, groups=None):
    if hasattr(cv2, "calibrateHandEye") and method is not None:
        groups = list(groups) if groups is not None else [0] * len(slam_poses)
        solutions = []
        for group in sorted(set(groups)):
            ss = [x for x, g in zip(slam_poses, groups) if g == group]
            bb = [x for x, g in zip(board_poses, groups) if g == group]
            if len(ss) < 6: continue
            R, t = cv2.calibrateHandEye([x[:3,:3] for x in ss], [x[:3,3] for x in ss],
                                        [x[:3,:3] for x in bb], [x[:3,3] for x in bb], method=method)
            solutions.append(transform(R, t))
        if not solutions:
            raise ValueError("no session has enough poses for OpenCV hand-eye")
        return transform(rotation_mean([x[:3,:3] for x in solutions]),
                         np.median([x[:3,3] for x in solutions], axis=0))
    return handeye_park_explicit(slam_poses, board_poses, groups)


def solve_handeye(datasets, board_by_key):
    samples = []
    for key, board in sorted(board_by_key.items()):
        d, s = key; raw = manifest_slam_pose(datasets[d].manifest[s])
        if raw is not None: samples.append((d, raw, board))
    if hasattr(cv2, "calibrateHandEye"):
        methods = [(name, getattr(cv2, constant)) for name, constant in (
            ("TSAI","CALIB_HAND_EYE_TSAI"),("PARK","CALIB_HAND_EYE_PARK"),
            ("HORAUD","CALIB_HAND_EYE_HORAUD"),("ANDREFF","CALIB_HAND_EYE_ANDREFF"),
            ("DANIILIDIS","CALIB_HAND_EYE_DANIILIDIS")) if hasattr(cv2, constant)]
    else:
        methods = [("PARK_EXPLICIT", None)]
    hypotheses = []
    for variant in ("raw", "runtime_tracker", "runtime_head", "rigid_z_correction", "position_z_correction"):
        groups = [x[0] for x in samples]
        slam = [runtime_pose(x[1], variant) for x in samples]; boards = [x[2] for x in samples]
        for invert_slam, invert_board in ((False,False),(True,False),(False,True),(True,True)):
            ss = [invert(x) for x in slam] if invert_slam else slam
            bb = [invert(x) for x in boards] if invert_board else boards
            for method_name, method in methods:
                try:
                    X = handeye_once(ss, bb, method, groups)
                    if not np.all(np.isfinite(X)): continue
                    _, rr, tr, fixed = fixed_board_residuals(ss, bb, X, groups)
                    hypotheses.append({"slam_variant": variant, "invert_slam": invert_slam,
                        "invert_board": invert_board, "method": method_name, "T_tracker_rig": X,
                        "rotation_residual_deg": rr, "translation_residual_m": tr,
                        "T_slam_board_by_dataset": fixed,
                        "per_dataset": per_group_fixed_board_diagnostics(ss, bb, X, groups),
                        "relative_motion_residual": relative_motion_residuals(ss, bb, X, groups)})
                except (ValueError, cv2.error):
                    pass
    hypotheses.sort(key=lambda x: (x["translation_residual_m"]["median"], x["rotation_residual_deg"]["median"]))
    # The recorder stores wire-remapped samples before slam_correction_pose.
    # The runtime-equivalent tracker pose is therefore the primary convention.
    nominal = [x for x in hypotheses if x["slam_variant"] == "runtime_tracker" and not x["invert_slam"] and not x["invert_board"]]
    nominal.sort(key=lambda x: x["translation_residual_m"]["median"])
    best_nominal = nominal[0] if nominal else None
    method_spread = {}
    if nominal:
        ref = nominal[0]["T_tracker_rig"]
        method_spread = {"rotation_deg": stats([rotation_angle_deg(ref[:3,:3].T @ x["T_tracker_rig"][:3,:3]) for x in nominal]),
                         "translation_m": stats([np.linalg.norm(ref[:3,3]-x["T_tracker_rig"][:3,3]) for x in nominal])}
    scale_diagnostics = {}
    for group in sorted({x[0] for x in samples}):
        subset = [x for x in samples if x[0] == group]
        boards = [x[2] for x in subset]
        trials = []
        for scale in np.linspace(.6, 1.4, 17):
            slam = []
            for _, raw, _ in subset:
                S = runtime_pose(raw, "runtime_tracker"); S[:3,3] *= scale; slam.append(S)
            try:
                X = handeye_once(slam, boards)
                _, _rr, tr, _center = fixed_board_residuals(slam, boards, X)
                trials.append({"slam_translation_scale": float(scale),
                               "median_translation_residual_m": tr["median"],
                               "p95_translation_residual_m": tr["p95"],
                               "T_tracker_rig": matrix_json(X)})
            except (ValueError, cv2.error):
                pass
        scale_diagnostics[str(group)] = {
            "best": min(trials, key=lambda x: x["median_translation_residual_m"]) if trials else None,
            "unit_scale": next((x for x in trials if abs(x["slam_translation_scale"] - 1.0) < 1e-9), None),
            "tested_range": [0.6, 1.4], "step": 0.05,
        }
    return samples, hypotheses, best_nominal, method_spread, scale_diagnostics


def serializable_intrinsic(value, T_R_C):
    return {"width": value["width"], "height": value["height"],
            "K": [[float(x) for x in row] for row in value["K"]], "D": [float(x) for x in value["D"]],
            "rms_px": value["rms"], "accepted_observations": value["accepted"],
            "rejected_observations": value["rejected"], "coverage": value["coverage"],
            "per_view": value["per_view"], "warnings": value["warnings"],
            "transform_to_rig_T_rig_camera": matrix_json(T_R_C)}


def solve(dataset_paths, square_length_m, marker_length_m, min_corners=8, min_common=6):
    datasets, observations = load_inputs(dataset_paths, min_corners)
    corners = board_points(7, 5, square_length_m); size = (640, 640)
    by_camera = {c: sorted([o for (_,_,camera), o in observations.items() if camera == c], key=lambda o:o.key)
                 for c in range(CAMERA_COUNT)}
    intrinsics = {c: calibrate_camera(c, by_camera[c], corners, size) for c in range(CAMERA_COUNT)}
    pairwise = []
    for a in range(CAMERA_COUNT):
        for b in range(a + 1, CAMERA_COUNT):
            result = pair_extrinsic(a, b, observations, corners, intrinsics, size, min_common)
            if result: pairwise.append(result)
    T_R_C, closures = build_rig(pairwise)
    board_poses, board_diag, board_quality = solve_board_poses(observations, corners, intrinsics, T_R_C)
    samples, hypotheses, nominal, method_spread, scale_diagnostics = solve_handeye(datasets, board_poses)
    warnings = [f"camera {c}: {w}" for c,v in intrinsics.items() for w in v["warnings"]]
    translation_trusted = bool(
        nominal and nominal["translation_residual_m"]["p95"] < .05 and
        all(v["translation_residual_m"]["median"] < .03 for v in nominal["per_dataset"].values()))
    if not translation_trusted:
        warnings.append("SLAM-to-rig translation is untrusted: at least one session exceeds the 30 mm median or combined 50 mm p95 guardrail")
    if not hasattr(cv2, "calibrateHandEye"):
        warnings.append("OpenCV calibrateHandEye Python binding unavailable; used explicit Park AX=XB solver")
    for edge in pairwise:
        if edge["frames"] < 10:
            warnings.append(f"camera pair {edge['camera_a']}-{edge['camera_b']} has only {edge['frames']} synchronized stereo views")
    for closure in closures:
        if closure["rotation_deg"] > .5 or closure["translation_m"] > .005:
            warnings.append(f"camera pair {closure['pair']} rig closure exceeds 0.5 deg or 5 mm")
    slam_json = {
        "transform_convention": "T_A_B maps coordinates in B into A; recorded pose is the pre-correction T_slam_tracker sample and the nominal hypothesis reproduces runtime correction",
        "runtime_processing": {
            "wire_remap": "position=(wire_z,wire_y,-wire_x), quaternion_xyzw=(-wire_qy,-wire_qx,wire_qz,wire_qw)",
            "slam_correction": "R_out=R_z(+90deg)*R_raw while p_out=p_raw+p_correction (not general SE(3) composition)",
            "relation_history": "stores corrected T_slam_tracker and interpolates/predicts it in VTS time",
            "head_pose": "T_slam_head=T_slam_tracker*T_tracker_head; T_tracker_head translation=(0.000247,-0.000273,0.104826)m",
        },
        "sample_count": len(samples), "tested_hypothesis_count": len(hypotheses),
        "opencv_methods": sorted({x["method"] for x in hypotheses}), "method_spread": method_spread,
        "slam_translation_scale_test": scale_diagnostics,
        "opencv_calibrate_handeye_available": hasattr(cv2, "calibrateHandEye"),
        "trusted_rotation": bool(nominal and nominal["rotation_residual_deg"]["median"] < 1.0),
        "trusted_translation": translation_trusted,
        "best_nominal": None if nominal is None else {
            "method": nominal["method"], "T_tracker_rig": matrix_json(nominal["T_tracker_rig"]),
            "rotation_residual_deg": nominal["rotation_residual_deg"],
            "translation_residual_m": nominal["translation_residual_m"],
            "per_dataset": nominal["per_dataset"],
            "relative_motion_residual": nominal["relative_motion_residual"]},
        "best_tested_hypotheses": [{
            "slam_variant": x["slam_variant"], "invert_slam": x["invert_slam"], "invert_board": x["invert_board"],
            "method": x["method"], "rotation_residual_deg": x["rotation_residual_deg"],
            "translation_residual_m": x["translation_residual_m"]} for x in hypotheses[:12]],
        "interpretation": "T_imu_head is a fixed right-side transform and is absorbed by hand-eye. The runtime's orientation-only +90deg slam correction materially changes translation consistency because the recorded pre-correction pose combines a position already in tracker axes with an orientation that still needs correction; a full rigid +90deg correction does not help.",
    }
    pair_json = [{k:(matrix_json(v) if k.startswith("T_") else v) for k,v in e.items() if k != "frame_keys"} for e in pairwise]
    return {
        "schema_version": SCHEMA_VERSION,
        "headset_serial": next((d.metadata.get("headset_serial") for d in datasets if d.metadata.get("headset_serial")), None),
        "target": {"squares_x":7,"squares_y":5,"measured_square_length_m":square_length_m,
                   "marker_length_m":marker_length_m,"dictionary":"DICT_4X4_50"},
        "datasets": [str(d.path) for d in datasets],
        "visible_cameras": {f"camera{c}": serializable_intrinsic(intrinsics[c], T_R_C[c]) for c in range(CAMERA_COUNT)},
        "rig": {"reference_camera":0,"pairwise":pair_json,"closure_residuals":closures,
                "board_pose_frames":len(board_poses),"board_pose_quality":board_quality,
                "board_pose_diagnostics":board_diag},
        "slam": slam_json,
        "tracking_readout": {
            "mode3_to_mode12_visible": {"scale_x":.5,"scale_y":.5,"same_camera_order":True,"flip":False,"translation_px":[0,0],"status":"experimentally_exact"},
            "mode12_tracking_to_mode4": {"scale_x":2.0,"scale_y":2.0,"same_camera_order":True,"status":"experimentally_exact"},
            "mode12_visible_to_tracking": {"status":"estimated_unresolved","transform":None,
                "note":"preliminary affine registration is bootstrap data, not runtime calibration"}},
        "quality": {"observation_counts":{f"camera{c}":len(by_camera[c]) for c in range(CAMERA_COUNT)},
                    "accepted_views":{f"camera{c}":intrinsics[c]["accepted"] for c in range(CAMERA_COUNT)},
                    "rejected_views":{f"camera{c}":intrinsics[c]["rejected"] for c in range(CAMERA_COUNT)},
                    "warnings":warnings},
    }


def print_summary(result):
    print(f"Solved {len(result['datasets'])} datasets; rig frame = camera 0")
    for name, camera in result["visible_cameras"].items():
        t = np.asarray(camera["transform_to_rig_T_rig_camera"])[:3,3]
        print(f"{name}: RMS {camera['rms_px']:.3f}px, views {camera['accepted_observations']} accepted/"
              f"{camera['rejected_observations']} rejected, rig offset {1000*np.linalg.norm(t):.1f}mm")
    print("pair baselines: " + ", ".join(f"{p['camera_a']}-{p['camera_b']}={p['baseline_m']*1000:.1f}mm"
                                         for p in result["rig"]["pairwise"]))
    slam = result["slam"]; nominal = slam["best_nominal"]
    if nominal:
        print(f"SLAM ({nominal['method']}): rotation median {nominal['rotation_residual_deg']['median']:.3f}deg; "
              f"translation median {nominal['translation_residual_m']['median']*1000:.1f}mm; "
              f"trusted rotation={slam['trusted_rotation']} translation={slam['trusted_translation']}")

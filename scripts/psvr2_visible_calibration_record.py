#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Record PSVR2 mode-3 visible cameras plus SLAM pose in the shared VTS clock.

This is intended for the ChArUco calibration capture. It talks directly to the
headset with PyUSB, claims camera interface 6 and SLAM interface 3, selects
camera mode 3, pairs camera sets 0 and 3 by hardware sequence/VTS, and writes
four 640x640 L8 images asynchronously. A second reader records the onboard SLAM
stream; the writer interpolates the remapped SLAM tracker pose at each camera
VTS and stores both the interpolated pose and the bracketing SLAM timestamps.

The SLAM parser intentionally follows Monado's runtime behaviour: an exact
512-byte transfer from endpoint 0x83 is treated as a slam_usb_record by fixed
offsets. The nominal "SLA" magic and packet-size fields are recorded as
 diagnostics but are not required for acceptance, because the working Monado
 driver does not gate parsing on them either.

Close Monado, GAV and SteamVR before running.
Requires: python3 -m pip install pyusb
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import queue
import struct
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path

try:
    import usb.core
    import usb.util
except ImportError as exc:
    raise SystemExit("pyusb is required: python3 -m pip install pyusb") from exc

PSVR2_VID = 0x054C
PSVR2_PID = 0x0CDE
SLAM_INTERFACE = 3
SLAM_ENDPOINT_IN = 0x83
CAMERA_INTERFACE = 6
CAMERA_ENDPOINT_IN = 0x87
REPORT_SET_CAMERA_MODE = 0x0B
CAMERA_SUBCMD = 0x01
CAMERA_MODE_VISIBLE_FOUR = 3
CAMERA_CTRL_REQUEST_TYPE = 0x42
CAMERA_CTRL_REQUEST = 0x09
CAMERA_HEADER_SIZE = 256
CAMERA_PACKET_SIZE = 819456
CAMERA_WIDTH = 640
CAMERA_HEIGHT = 640
CAMERA_PLANE_SIZE = CAMERA_WIDTH * CAMERA_HEIGHT
CAMERA_READ_SIZE = 1_040_640
SLAM_PACKET_SIZE = 512
SLAM_READ_SIZE = 1024
UINT32_MOD = 1 << 32


def signed_u32_delta(a: int, b: int) -> int:
    """Return signed a-b for wrapping uint32 microsecond clocks."""
    return ((int(a) - int(b) + (1 << 31)) % UINT32_MOD) - (1 << 31)


def set_camera_mode(dev, mode: int) -> None:
    payload = struct.pack("<HHI", REPORT_SET_CAMERA_MODE, CAMERA_SUBCMD, 8)
    payload += struct.pack("<II", 1, mode)
    written = dev.ctrl_transfer(
        CAMERA_CTRL_REQUEST_TYPE,
        CAMERA_CTRL_REQUEST,
        REPORT_SET_CAMERA_MODE,
        0,
        payload,
        timeout=500,
    )
    if written != len(payload):
        raise RuntimeError(f"short camera-mode control write: {written}/{len(payload)}")


def parse_camera_header(packet: bytes) -> dict | None:
    if len(packet) < 30 or packet[:2] != b"VI":
        return None
    fields = struct.unpack_from("<HIIIHHHHHH", packet, 2)
    return {
        "version": fields[0],
        "packet_size": fields[1],
        "vts_us": fields[2],
        "sequence_id": fields[3],
        "camera_set": fields[4],
        "image_height": fields[5],
        "active_height": fields[6],
        "image_width": fields[7],
        "active_width": fields[8],
        "unknown2": fields[9],
    }


def decode_mode3_sbs(packet: bytes) -> tuple[bytes, bytes]:
    if len(packet) != CAMERA_PACKET_SIZE:
        raise ValueError("unexpected mode-3 packet size")
    payload = memoryview(packet)[CAMERA_HEADER_SIZE:]
    left = bytearray(CAMERA_PLANE_SIZE)
    right = bytearray(CAMERA_PLANE_SIZE)
    stride = CAMERA_WIDTH * 2
    for y in range(CAMERA_HEIGHT):
        src = y * stride
        dst = y * CAMERA_WIDTH
        left[dst : dst + CAMERA_WIDTH] = payload[src : src + CAMERA_WIDTH]
        right[dst : dst + CAMERA_WIDTH] = payload[src + CAMERA_WIDTH : src + stride]
    return bytes(left), bytes(right)


@dataclass
class SlamSample:
    host_arrival_ns: int
    vts_us: int
    unknown1: int
    magic: bytes
    const1: int
    packet_size_field: int
    position: tuple[float, float, float]
    orientation: tuple[float, float, float, float]  # x,y,z,w, Monado-remapped tracker frame


def normalize_quat(q: tuple[float, float, float, float]) -> tuple[float, float, float, float]:
    n = math.sqrt(sum(v * v for v in q))
    if n < 1e-12:
        return (0.0, 0.0, 0.0, 1.0)
    return tuple(v / n for v in q)  # type: ignore[return-value]


def parse_slam(packet: bytes, host_arrival_ns: int) -> tuple[SlamSample | None, str | None]:
    """Parse exactly the fixed-offset record used by Monado's process_slam_record()."""
    if len(packet) != SLAM_PACKET_SIZE:
        return None, "bad_length"

    magic, const1, packet_size, vts_us, unknown1, px, py, pz, qw, qx, qy, qz = struct.unpack_from(
        "<3sBIII3f4f", packet, 0
    )

    values = (px, py, pz, qw, qx, qy, qz)
    if not all(math.isfinite(v) for v in values):
        return None, "nonfinite_pose"

    raw_quat_norm = math.sqrt(qw * qw + qx * qx + qy * qy + qz * qz)
    # A valid tracking quaternion should be near unit length. This broad range
    # rejects obviously mis-decoded data without over-constraining the device.
    if raw_quat_norm < 0.25 or raw_quat_norm > 4.0:
        return None, "bad_quaternion"

    # Match the wire->Monado tracker-axis remap in psvr2.c process_slam_record().
    position = (pz, py, -px)
    orientation = normalize_quat((-qy, -qx, qz, qw))
    return (
        SlamSample(
            host_arrival_ns=host_arrival_ns,
            vts_us=vts_us,
            unknown1=unknown1,
            magic=magic,
            const1=const1,
            packet_size_field=packet_size,
            position=position,
            orientation=orientation,
        ),
        None,
    )


def slerp(q0, q1, t: float):
    a = list(q0)
    b = list(q1)
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0.0:
        b = [-x for x in b]
        dot = -dot
    dot = max(-1.0, min(1.0, dot))
    if dot > 0.9995:
        return normalize_quat(tuple(a[i] + t * (b[i] - a[i]) for i in range(4)))
    theta = math.acos(dot)
    sin_theta = math.sin(theta)
    w0 = math.sin((1.0 - t) * theta) / sin_theta
    w1 = math.sin(t * theta) / sin_theta
    return normalize_quat(tuple(w0 * a[i] + w1 * b[i] for i in range(4)))


class SlamStore:
    def __init__(self, maxlen: int = 512):
        self.samples = deque(maxlen=maxlen)
        self.cond = threading.Condition()

    def push(self, sample: SlamSample) -> None:
        with self.cond:
            self.samples.append(sample)
            self.cond.notify_all()

    def pose_at(self, vts_us: int, wait_s: float = 0.25):
        deadline = time.monotonic() + wait_s
        with self.cond:
            while True:
                result = self._pose_at_locked(vts_us)
                if result is not None and (result["interpolated"] or time.monotonic() >= deadline):
                    return result
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return result
                self.cond.wait(min(remaining, 0.02))

    def _pose_at_locked(self, vts_us: int):
        if not self.samples:
            return None
        before = None
        after = None
        before_delta = None
        after_delta = None
        for sample in self.samples:
            delta = signed_u32_delta(sample.vts_us, vts_us)
            if delta <= 0 and (before_delta is None or delta > before_delta):
                before, before_delta = sample, delta
            if delta >= 0 and (after_delta is None or delta < after_delta):
                after, after_delta = sample, delta

        if before is not None and after is not None and before_delta is not None and after_delta is not None:
            span = after_delta - before_delta
            if 0 <= span <= 100_000:
                t = 0.0 if span == 0 else (-before_delta) / span
                position = tuple(
                    before.position[i] + t * (after.position[i] - before.position[i]) for i in range(3)
                )
                orientation = slerp(before.orientation, after.orientation, t)
                return {
                    "valid": True,
                    "interpolated": span != 0,
                    "before_vts_us": before.vts_us,
                    "after_vts_us": after.vts_us,
                    "nearest_delta_us": min(abs(before_delta), abs(after_delta)),
                    "position": position,
                    "orientation": orientation,
                }

        nearest = min(self.samples, key=lambda sample: abs(signed_u32_delta(sample.vts_us, vts_us)))
        delta = signed_u32_delta(nearest.vts_us, vts_us)
        if abs(delta) > 50_000:
            return None
        return {
            "valid": True,
            "interpolated": False,
            "before_vts_us": nearest.vts_us,
            "after_vts_us": nearest.vts_us,
            "nearest_delta_us": abs(delta),
            "position": nearest.position,
            "orientation": nearest.orientation,
        }


@dataclass
class CameraJob:
    set_index: int
    sequence_id: int
    vts_us: int
    frames: tuple[bytes, bytes, bytes, bytes]


class Recorder:
    def __init__(self, output_dir: Path, stride: int):
        self.output_dir = output_dir
        self.frames_dir = output_dir / "frames"
        self.stride = stride
        self.stop = threading.Event()
        self.slam = SlamStore()
        self.jobs: queue.Queue[CameraJob | None] = queue.Queue(maxsize=32)
        self.pending: dict[int, dict] = {}
        self.set_index = 0
        self.stats = {
            "camera_packets": 0,
            "camera_sets_complete": 0,
            "camera_sets_queued": 0,
            "camera_sets_dropped": 0,
            "camera_pair_vts_mismatch": 0,
            "slam_packets": 0,
            "slam_valid": 0,
            "slam_bad_length": 0,
            "slam_nonfinite_pose": 0,
            "slam_bad_quaternion": 0,
            "slam_magic_mismatch": 0,
            "slam_packet_size_mismatch": 0,
            "slam_unknown1_not3": 0,
            "written_sets": 0,
            "pose_failures": 0,
        }
        self.stats_lock = threading.Lock()
        self.manifest = None
        self.manifest_writer = None
        self.slam_csv = None
        self.slam_writer = None

    def inc(self, name: str, value: int = 1):
        with self.stats_lock:
            self.stats[name] += value

    def setup_files(self):
        self.output_dir.mkdir(parents=True, exist_ok=True)
        self.frames_dir.mkdir(parents=True, exist_ok=True)
        self.manifest = (self.output_dir / "manifest.csv").open("w", newline="")
        self.manifest_writer = csv.writer(self.manifest)
        self.manifest_writer.writerow(
            [
                "set_index", "sequence_id", "camera_vts_us", "slam_pose_valid", "slam_interpolated",
                "slam_before_vts_us", "slam_after_vts_us", "slam_nearest_delta_us",
                "tracker_px", "tracker_py", "tracker_pz", "tracker_qx", "tracker_qy", "tracker_qz", "tracker_qw",
                "camera0_file", "camera1_file", "camera2_file", "camera3_file",
            ]
        )
        self.slam_csv = (self.output_dir / "slam.csv").open("w", newline="")
        self.slam_writer = csv.writer(self.slam_csv)
        self.slam_writer.writerow(
            [
                "host_arrival_ns", "vts_us", "unknown1", "magic_hex", "const1", "packet_size_field",
                "tracker_px", "tracker_py", "tracker_pz", "tracker_qx", "tracker_qy", "tracker_qz", "tracker_qw",
            ]
        )
        metadata = {
            "schema_version": 3,
            "purpose": "psvr2_four_camera_visible_charuco_calibration",
            "camera_mode": 3,
            "camera_count": 4,
            "camera_mapping": {
                "camera0": "mode3 set0 left",
                "camera1": "mode3 set0 right",
                "camera2": "mode3 set3 left",
                "camera3": "mode3 set3 right",
            },
            "image_format": "L8_PGM",
            "image_width": CAMERA_WIDTH,
            "image_height": CAMERA_HEIGHT,
            "sequence_stride": self.stride,
            "time_domain": "raw PSVR2 VTS microseconds shared by camera and SLAM streams",
            "slam_pose": (
                "fixed-offset 512-byte endpoint-0x83 pose remapped to Monado tracker axes; "
                "nominal magic/packet-size fields are diagnostic only; no host-clock conversion or head-offset transform applied"
            ),
            "manifest": "manifest.csv",
            "slam_samples": "slam.csv",
            "charuco_target": {
                "squares_x": 7,
                "squares_y": 5,
                "square_length_mm_nominal": 40.0,
                "marker_length_mm_nominal": 30.0,
                "dictionary": "DICT_4X4_50",
                "actual_square_length_mm": None,
            },
        }
        (self.output_dir / "dataset.json").write_text(json.dumps(metadata, indent=2) + "\n")

    def camera_reader(self, dev):
        while not self.stop.is_set():
            try:
                data = dev.read(CAMERA_ENDPOINT_IN, CAMERA_READ_SIZE, timeout=100)
            except usb.core.USBTimeoutError:
                continue
            except usb.core.USBError as exc:
                if not self.stop.is_set():
                    print(f"camera USB error: {exc}", file=sys.stderr)
                break
            packet = bytes(data)
            if not packet:
                continue
            self.inc("camera_packets")
            header = parse_camera_header(packet)
            if (
                header is None
                or len(packet) != CAMERA_PACKET_SIZE
                or header["camera_set"] not in (0, 3)
                or header["image_width"] != CAMERA_WIDTH
                or header["image_height"] != CAMERA_HEIGHT
            ):
                continue
            sequence = int(header["sequence_id"])
            if sequence % self.stride != 0:
                continue
            left, right = decode_mode3_sbs(packet)
            entry = self.pending.setdefault(sequence, {"vts_us": int(header["vts_us"])})
            if entry["vts_us"] != int(header["vts_us"]):
                self.inc("camera_pair_vts_mismatch")
                self.pending.pop(sequence, None)
                continue
            entry[int(header["camera_set"])] = (left, right)
            if 0 not in entry or 3 not in entry:
                if len(self.pending) > 64:
                    oldest = min(self.pending)
                    self.pending.pop(oldest, None)
                    self.inc("camera_sets_dropped")
                continue

            self.inc("camera_sets_complete")
            frames = (entry[0][0], entry[0][1], entry[3][0], entry[3][1])
            job = CameraJob(self.set_index, sequence, entry["vts_us"], frames)
            self.set_index += 1
            self.pending.pop(sequence, None)
            try:
                self.jobs.put_nowait(job)
                self.inc("camera_sets_queued")
            except queue.Full:
                self.inc("camera_sets_dropped")

    def slam_reader(self, dev):
        first_bad_prefix_printed = False
        while not self.stop.is_set():
            try:
                data = dev.read(SLAM_ENDPOINT_IN, SLAM_READ_SIZE, timeout=100)
            except usb.core.USBTimeoutError:
                continue
            except usb.core.USBError as exc:
                if not self.stop.is_set():
                    print(f"SLAM USB error: {exc}", file=sys.stderr)
                break
            packet = bytes(data)
            if not packet:
                continue
            self.inc("slam_packets")
            sample, error = parse_slam(packet, time.monotonic_ns())
            if sample is None:
                if error == "bad_length":
                    self.inc("slam_bad_length")
                elif error == "nonfinite_pose":
                    self.inc("slam_nonfinite_pose")
                elif error == "bad_quaternion":
                    self.inc("slam_bad_quaternion")
                if not first_bad_prefix_printed:
                    print(
                        f"First rejected SLAM transfer: length={len(packet)} prefix={packet[:32].hex()} reason={error}",
                        file=sys.stderr,
                    )
                    first_bad_prefix_printed = True
                continue

            if sample.magic != b"SLA":
                self.inc("slam_magic_mismatch")
            if sample.packet_size_field != SLAM_PACKET_SIZE:
                self.inc("slam_packet_size_mismatch")
            if sample.unknown1 != 3:
                self.inc("slam_unknown1_not3")

            self.inc("slam_valid")
            self.slam.push(sample)
            self.slam_writer.writerow(
                [
                    sample.host_arrival_ns,
                    sample.vts_us,
                    sample.unknown1,
                    sample.magic.hex(),
                    sample.const1,
                    sample.packet_size_field,
                    *sample.position,
                    *sample.orientation,
                ]
            )
            self.slam_csv.flush()

    @staticmethod
    def write_pgm(path: Path, pixels: bytes):
        with path.open("wb") as f:
            f.write(f"P5\n{CAMERA_WIDTH} {CAMERA_HEIGHT}\n255\n".encode("ascii"))
            f.write(pixels)

    def writer(self):
        while True:
            try:
                job = self.jobs.get(timeout=0.1)
            except queue.Empty:
                if self.stop.is_set() and self.jobs.empty():
                    break
                continue
            if job is None:
                break
            pose = self.slam.pose_at(job.vts_us, 0.25)
            names = []
            for camera, pixels in enumerate(job.frames):
                name = f"frames/set-{job.set_index:06d}-camera{camera}.pgm"
                self.write_pgm(self.output_dir / name, pixels)
                names.append(name)

            if pose is None:
                self.inc("pose_failures")
                row = [job.set_index, job.sequence_id, job.vts_us, 0, 0, "", "", "", "", "", "", "", "", "", "", *names]
            else:
                row = [
                    job.set_index, job.sequence_id, job.vts_us, 1, int(pose["interpolated"]),
                    pose["before_vts_us"], pose["after_vts_us"], pose["nearest_delta_us"],
                    *pose["position"], *pose["orientation"], *names,
                ]
            self.manifest_writer.writerow(row)
            self.manifest.flush()
            self.inc("written_sets")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--duration", type=float, default=60.0, help="capture duration in seconds (default 60)")
    parser.add_argument("--stride", type=int, default=6, help="keep every Nth hardware sequence (default 6 ~=10 Hz)")
    args = parser.parse_args()
    if args.duration <= 0 or args.duration > 900:
        raise SystemExit("--duration must be >0 and <=900 seconds")
    if args.stride < 1 or args.stride > 120:
        raise SystemExit("--stride must be between 1 and 120")

    recorder = Recorder(args.output_dir, args.stride)
    recorder.setup_files()
    dev = usb.core.find(idVendor=PSVR2_VID, idProduct=PSVR2_PID)
    if dev is None:
        raise SystemExit("PS VR2 USB device not found")

    claimed = []
    camera_thread = slam_thread = writer_thread = None
    try:
        try:
            dev.set_configuration()
        except usb.core.USBError:
            pass
        for interface in (SLAM_INTERFACE, CAMERA_INTERFACE):
            usb.util.claim_interface(dev, interface)
            claimed.append(interface)
            try:
                dev.set_interface_altsetting(interface=interface, alternate_setting=0)
            except usb.core.USBError:
                pass

        set_camera_mode(dev, CAMERA_MODE_VISIBLE_FOUR)
        camera_thread = threading.Thread(target=recorder.camera_reader, args=(dev,), name="psvr2-camera", daemon=True)
        slam_thread = threading.Thread(target=recorder.slam_reader, args=(dev,), name="psvr2-slam", daemon=True)
        writer_thread = threading.Thread(target=recorder.writer, name="psvr2-writer", daemon=True)
        writer_thread.start()
        slam_thread.start()
        camera_thread.start()

        print(
            f"Recording mode-3 visible calibration data for {args.duration:.1f}s to {args.output_dir} "
            f"(stride {args.stride}).",
            file=sys.stderr,
        )
        deadline = time.monotonic() + args.duration
        next_status = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            time.sleep(0.05)
            if time.monotonic() >= next_status:
                with recorder.stats_lock:
                    stats = dict(recorder.stats)
                print(
                    f"  camera_packets={stats['camera_packets']} complete={stats['camera_sets_complete']} "
                    f"written={stats['written_sets']} queue={recorder.jobs.qsize()} "
                    f"slam={stats['slam_valid']}/{stats['slam_packets']} pose_failures={stats['pose_failures']} "
                    f"slam_bad_len={stats['slam_bad_length']} magic_mismatch={stats['slam_magic_mismatch']}",
                    file=sys.stderr,
                )
                next_status += 2.0
    finally:
        recorder.stop.set()
        try:
            set_camera_mode(dev, 0)
        except Exception:
            pass
        for thread in (camera_thread, slam_thread):
            if thread is not None:
                thread.join(timeout=2.0)
        if writer_thread is not None:
            writer_thread.join(timeout=10.0)
        for interface in reversed(claimed):
            try:
                usb.util.release_interface(dev, interface)
            except Exception:
                pass
        usb.util.dispose_resources(dev)
        if recorder.manifest is not None:
            recorder.manifest.close()
        if recorder.slam_csv is not None:
            recorder.slam_csv.close()

    with recorder.stats_lock:
        stats = dict(recorder.stats)
    print(json.dumps(stats, indent=2), file=sys.stderr)
    if stats["written_sets"] == 0:
        print("No synchronized calibration sets were written.", file=sys.stderr)
        return 1
    if stats["pose_failures"]:
        print("Warning: some camera sets could not be associated with a SLAM pose.", file=sys.stderr)
    print(f"Dataset: {args.output_dir / 'dataset.json'}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

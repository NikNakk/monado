#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Send PS Sense tracking-LED commands over Bluetooth HID and watch the result with the PS VR2 mode-4 cameras.

For probing the right controller's always-lit fault (LEDs lit whatever they are told, status LED off) without
Monado. Monado must not be running: this opens the Sense over IOKit and the headset cameras over libusb.

Status (25 Sep): the controller accepts these reports (off darkened a ring the calibration probe had left lit), but
forced_on lit the ring in only 1 of 5 tries, although its bytes match the calibration probe's. The C probe
(build-sense/src/xrt/auxiliary/os/pssense_hid_probe --hand left|right --force-ir-seconds N) lights it every time.
Camera 1's px>200 count is the clearest indicator in the current setup (0 dark, ~50-110 lit).

    .venv/bin/python scripts/pssense_led_poke.py OUT_DIR --side R --steps "watch,off,status_on,init,off,all_on,off"

Each step sends its command every ~10 ms for --hold seconds, capturing the cameras over its last second, and
prints the LED-shaped blobs each camera saw. Steps:
    watch      send nothing
    off        phase LED_ALL_OFF (5), what the driver sends to darken a controller
    status_on  LED_ALL_OFF plus the status-LED set-enable flag with status_led_enable=1
    status_off LED_ALL_OFF plus status-LED set-enable with status_led_enable=0
    init       phase INIT (0)
    all_on     phase LED_ALL_ON (6), starting 50 ms ahead on the controller's clock
    forced_on  PRESCAN, 2.1 ms pulses every 2.0 ms: lit continuously (the PSSENSE_FORCE_IR recipe)
    prescan    phase PRESCAN (1), period id 42, all LEDs, 16.683 ms cycle
    debug      phase DEBUG (7)
    rumble     LED_ALL_OFF plus vibration at half amplitude (feel for it)
    calib      re-read calibration feature report 0x05 (no capture). The controller ignored every LED command until
               this had been read once after connecting: on 25 Sep the left stayed dark under forced_on without it.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import struct
import subprocess
import sys
import threading
import time
import zlib
import json
import random
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from pssense_sony_wire_prepare import rebase_settings, phase_reference, aligned_prescan_phase

VENDOR = 1356
PRODUCT = {"L": 3653, "R": 3654}

# IOKit / CoreFoundation through ctypes.
iokit = ctypes.CDLL("/System/Library/Frameworks/IOKit.framework/IOKit")
cf = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
cf.CFStringCreateWithCString.restype = ctypes.c_void_p
cf.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
cf.CFNumberCreate.restype = ctypes.c_void_p
cf.CFNumberCreate.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p]
cf.CFDictionaryCreateMutable.restype = ctypes.c_void_p
cf.CFDictionaryCreateMutable.argtypes = [ctypes.c_void_p, ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p]
cf.CFDictionarySetValue.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
cf.CFSetGetCount.restype = ctypes.c_long
cf.CFSetGetCount.argtypes = [ctypes.c_void_p]
cf.CFSetGetValues.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
iokit.IOHIDManagerCreate.restype = ctypes.c_void_p
iokit.IOHIDManagerCreate.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
iokit.IOHIDManagerSetDeviceMatching.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
iokit.IOHIDManagerCopyDevices.restype = ctypes.c_void_p
iokit.IOHIDManagerCopyDevices.argtypes = [ctypes.c_void_p]
iokit.IOHIDDeviceOpen.restype = ctypes.c_int32
iokit.IOHIDDeviceOpen.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
iokit.IOHIDDeviceSetReport.restype = ctypes.c_int32
iokit.IOHIDDeviceSetReport.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long, ctypes.c_char_p, ctypes.c_long]
iokit.IOHIDDeviceScheduleWithRunLoop.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
iokit.IOHIDDeviceUnscheduleFromRunLoop.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
cf.CFRunLoopGetCurrent.restype = ctypes.c_void_p
cf.CFRunLoopRunInMode.restype = ctypes.c_int32
cf.CFRunLoopRunInMode.argtypes = [ctypes.c_void_p, ctypes.c_double, ctypes.c_bool]
INPUT_CALLBACK = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int32, ctypes.c_void_p, ctypes.c_int, ctypes.c_uint32,
                                  ctypes.POINTER(ctypes.c_uint8), ctypes.c_long)
iokit.IOHIDDeviceRegisterInputReportCallback.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_long,
                                                         INPUT_CALLBACK, ctypes.c_void_p]
iokit.IOHIDDeviceGetReport.restype = ctypes.c_int32
iokit.IOHIDDeviceGetReport.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_long, ctypes.c_void_p,
                                       ctypes.POINTER(ctypes.c_long)]
K_IOHID_REPORT_FEATURE = 2
K_CF_STRING_UTF8 = 0x08000100
#: Offset of the controller's IMU tick counter (1/3 us) in the 78-byte Bluetooth input report.
DEVICE_TIMESTAMP_OFFSET = 49
K_CF_NUMBER_SINT32 = 3
K_IOHID_REPORT_OUTPUT = 1


def cfstr(s: str):
    return cf.CFStringCreateWithCString(None, s.encode(), K_CF_STRING_UTF8)


def cfnum(v: int):
    x = ctypes.c_int32(v)
    return cf.CFNumberCreate(None, K_CF_NUMBER_SINT32, ctypes.byref(x))


def open_devices(product: int) -> list[int]:
    manager = iokit.IOHIDManagerCreate(None, 0)
    match = cf.CFDictionaryCreateMutable(None, 0,
                                         ctypes.addressof(ctypes.c_void_p.in_dll(cf, "kCFTypeDictionaryKeyCallBacks")),
                                         ctypes.addressof(ctypes.c_void_p.in_dll(cf, "kCFTypeDictionaryValueCallBacks")))
    cf.CFDictionarySetValue(match, cfstr("VendorID"), cfnum(VENDOR))
    cf.CFDictionarySetValue(match, cfstr("ProductID"), cfnum(product))
    iokit.IOHIDManagerSetDeviceMatching(manager, match)
    devices = iokit.IOHIDManagerCopyDevices(manager)
    if not devices:
        return []
    n = cf.CFSetGetCount(devices)
    values = (ctypes.c_void_p * n)()
    cf.CFSetGetValues(devices, values)
    out = []
    for dev in values:
        if iokit.IOHIDDeviceOpen(dev, 0) == 0:
            out.append(dev)
    return out


def read_device_clock(device: int, seconds: float = 0.5) -> tuple[int, int] | None:
    """Latest (controller IMU ticks, host monotonic ns) from the device's input reports."""
    latest: list[tuple[int, int]] = []
    buffer = (ctypes.c_uint8 * 128)()

    def on_report(_ctx, _result, _sender, _type, report_id, report, length):
        if report_id == 0x31 and length >= DEVICE_TIMESTAMP_OFFSET + 4:
            data = bytes(report[i] for i in range(length))
            latest.append((struct.unpack_from("<I", data, DEVICE_TIMESTAMP_OFFSET)[0], time.monotonic_ns()))

    callback = INPUT_CALLBACK(on_report)
    mode = ctypes.c_void_p.in_dll(cf, "kCFRunLoopDefaultMode")
    loop = cf.CFRunLoopGetCurrent()
    iokit.IOHIDDeviceRegisterInputReportCallback(device, buffer, len(buffer), callback, None)
    iokit.IOHIDDeviceScheduleWithRunLoop(device, loop, mode)
    cf.CFRunLoopRunInMode(mode, seconds, False)
    iokit.IOHIDDeviceUnscheduleFromRunLoop(device, loop, mode)
    return latest[-1] if latest else None


def read_calibration(device: int, recorder=None) -> list[str]:
    """Read feature report 0x05 twice (the two calibration parts), as the driver and calibration probe do at start."""
    parts = []
    for _ in range(2):
        buf = (ctypes.c_uint8 * 64)()
        buf[0] = 0x05
        length = ctypes.c_long(64)
        ret = iokit.IOHIDDeviceGetReport(device, K_IOHID_REPORT_FEATURE, 0x05, buf, ctypes.byref(length))
        if recorder:
            recorder.write("feature_rx", device=device, report_id=5, result=ret,
                           data_hex=bytes(buf[:max(0, min(length.value, 64))]).hex())
        parts.append(f"ret={ret:#x} part={buf[1]:#04x}")
    return parts


class Recorder:
    def __init__(self, path: Path):
        self.file = path.open("x")
        self.lock = threading.Lock()

    def write(self, event: str, **fields):
        with self.lock:
            self.file.write(json.dumps(dict(event=event, host_monotonic_ns=time.monotonic_ns(),
                                           clock_monotonic_ns=time.clock_gettime_ns(time.CLOCK_MONOTONIC),
                                           host_realtime_ns=time.time_ns(), **fields)) + "\n")
            self.file.flush()


class InputRecorder:
    """Continuous complete input reports; clock observations use callback receipt time, not exposure time."""
    def __init__(self, devices, recorder):
        self.devices, self.recorder = devices, recorder
        self.latest = None
        self.stop = threading.Event()
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()
        self.ready.wait(2)

    def run(self):
        mode = ctypes.c_void_p.in_dll(cf, "kCFRunLoopDefaultMode")
        loop = cf.CFRunLoopGetCurrent()
        callbacks, buffers = [], []
        for dev in self.devices:
            def on_report(_ctx, result, _sender, report_type, report_id, report, length, device=dev):
                host_ns = time.monotonic_ns()
                data = ctypes.string_at(report, length)
                if device == self.devices[0] and result == 0 and report_id == 0x31 and length >= DEVICE_TIMESTAMP_OFFSET + 4:
                    self.latest = (struct.unpack_from("<I", data, DEVICE_TIMESTAMP_OFFSET)[0], host_ns)
                self.recorder.write("hid_rx", device=device, result=result, report_type=report_type,
                                    report_id=report_id, callback_monotonic_ns=host_ns, data_hex=data.hex())
            callback = INPUT_CALLBACK(on_report)
            buffer = (ctypes.c_uint8 * 512)()
            callbacks.append(callback)
            buffers.append(buffer)
            iokit.IOHIDDeviceRegisterInputReportCallback(dev, buffer, len(buffer), callback, None)
            iokit.IOHIDDeviceScheduleWithRunLoop(dev, loop, mode)
        self.ready.set()
        while not self.stop.is_set():
            cf.CFRunLoopRunInMode(mode, 0.05, False)
        for dev in self.devices:
            iokit.IOHIDDeviceUnscheduleFromRunLoop(dev, loop, mode)

    def ticks_now(self):
        if self.latest is None:
            raise RuntimeError("no controller clock observation")
        ticks, host_ns = self.latest
        age = time.monotonic_ns() - host_ns
        if age > 100_000_000:
            raise RuntimeError(f"controller clock observation stale: {age / 1e6:.1f} ms")
        return (ticks + age * 3 // 1000) & 0xFFFFFFFF


class Sender:
    """Builds and sends 78-byte PS5-layout output reports (report 0x31), as the driver does over Bluetooth."""

    def __init__(self, devices: list[int], recorder=None):
        self.devices = devices
        self.recorder = recorder
        self.seq = 0
        self.counter = 0
        self.led_seq = 0
        self.zero_host_timestamp = True

    def report(self, phase: int, period_id: int = 0, cycle_position: int = 0, masks: bytes = b"\x00" * 4,
               flag2: int = 0, status_led: int = 0, cycle_ns: int = 16683000, flag1: int = 0,
               vibration: int = 0, settings: bytes | None = None) -> bytes:
        led = struct.pack("<BBBII4s", phase, self.led_seq & 0xFF, period_id, cycle_position & 0xFFFFFFFF,
                          cycle_ns * 3, masks)
        # Zero, as the calibration probe sends: its reports are the ones known to light the ring outside Monado.
        host_us = 0 if self.zero_host_timestamp else (time.monotonic_ns() // 1000) & 0xFFFFFFFF
        if settings is None:
            settings = struct.pack("<BBBB11sI", flag1, flag2, vibration, 0, b"\x00" * 11, host_us) + led + struct.pack(
                "<BB2s", 0, status_led, b"\x00\x00")
        else:
            settings = bytearray(settings)
            if len(settings) != 38:
                raise ValueError("captured settings must be complete (38 bytes)")
            # Monado's independently understood public settings layout. Keep all other bytes.
            struct.pack_into("<I", settings, 15, (time.monotonic_ns() // 1000) & 0xFFFFFFFF)
            settings[20] = self.led_seq & 255
            settings = bytes(settings)
        assert len(settings) == 38, len(settings)
        body = struct.pack("<BBB", 0x31, (self.seq << 4) & 0xF0, 0x10) + settings + struct.pack("<B", self.counter) + (
            b"\x00" * 32)
        crc = zlib.crc32(body, zlib.crc32(b"\xa2")) & 0xFFFFFFFF
        self.seq = (self.seq + 1) % 16
        self.counter = (self.counter + 1) & 0xFF
        return body + struct.pack("<I", crc)

    def send(self, data: bytes) -> int:
        ok = 0
        for dev in self.devices:
            before = time.monotonic_ns()
            result = iokit.IOHIDDeviceSetReport(dev, K_IOHID_REPORT_OUTPUT, data[0], data, len(data))
            if self.recorder:
                self.recorder.write("hid_tx", device=dev, result=result, before_monotonic_ns=before,
                                    data_hex=data.hex())
            if result == 0:
                ok += 1
        return ok


#: Steps whose LED schedule starts at a cycle position on the controller's clock (filled in at run time).
TIMED = {"forced_on", "prescan", "all_on"}

STEPS = {
    "watch": None,
    "off": dict(phase=5),
    "status_on": dict(phase=5, flag2=1 << 2, status_led=1),
    "status_off": dict(phase=5, flag2=1 << 2, status_led=0),
    "init": dict(phase=0),
    # LED_ALL_OFF plus vibration (flag1 bits 0-1, amplitude 0x80): tells whether the controller acts on output
    # reports at all while its LEDs ignore them.
    "rumble": dict(phase=5, flag1=0x03, vibration=0x80),
    "all_on": dict(phase=6, period_id=42, masks=b"\xff" * 4),
    # The calibration probe's optically verified "always lit": 2.1 ms pulses every 2.0 ms (PSSENSE_FORCE_IR).
    "forced_on": dict(phase=1, period_id=42, masks=b"\xff" * 4, cycle_ns=2000000),
    "prescan": dict(phase=1, period_id=42, masks=b"\xff" * 4),
    "debug": dict(phase=7),
}


def camera_images(pose_dir, camera_set, plane):
    pattern = f"*mode-04-size-*-set-{camera_set}-example-*-plane{plane}"
    return sorted([*pose_dir.glob(pattern + ".pgm"), *pose_dir.glob(pattern + ".png")])


def led_blobs(pose_dir: Path) -> list[int]:
    """LED-shaped bright blobs per camera, median over the captured frames (same filter as the tracker)."""
    out = []
    for s, plane in ((4, 0), (4, 1), (5, 0), (5, 1)):
        counts = []
        for f in camera_images(pose_dir, s, plane):
            im = cv2.imread(str(f), 0)[:, :508]
            _, bw = cv2.threshold(im, 100, 255, cv2.THRESH_BINARY)
            n, _, stats, _ = cv2.connectedComponentsWithStats(bw)
            c = 0
            for x, y, w, h, area in stats[1:]:
                longest, shortest = max(w, h), min(w, h)
                if longest <= 16 and w * h <= 200 and longest <= 3 * max(shortest, 1):
                    c += 1
            counts.append(c)
        out.append(int(np.median(counts)) if counts else -1)
    return out


def bright_pixels(pose_dir: Path) -> list[int]:
    """Median count of pixels above 200 per camera. Robust to a ring so close its LEDs fail the LED-shape filter."""
    out = []
    for s, plane in ((4, 0), (4, 1), (5, 0), (5, 1)):
        ims = [cv2.imread(str(f), 0)[:, :508] for f in camera_images(pose_dir, s, plane)]
        out.append(int(np.median([(im > 200).sum() for im in ims])) if ims else -1)
    return out


def capture(out: Path, sequence="4", repeat=1, bc4_layout=None, raw_if8=False, sample=0.6, compact_examples=False) -> None:
    survey = Path(__file__).resolve().parent / "psvr2_camera_mode_survey.py"
    command = [sys.executable, str(survey), str(out), "--sequence", sequence, "--repeat", str(repeat),
               "--settle", "0.5", "--sample", str(sample), "--examples", "500", "--save-every", "2", "--no-contact-sheet"]
    if bc4_layout:
        command += ["--bc4-layout", bc4_layout]
    if raw_if8:
        command += ["--raw-if8"]
    if compact_examples:
        command += ["--compact-examples"]
    with (out.parent / f"{out.name}-camera.log").open("x") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", type=Path)
    parser.add_argument("--side", choices=["L", "R"], default="R")
    parser.add_argument("--steps", default="watch,off,calib,off,status_on,init,off,forced_on,off")
    parser.add_argument("--no-calibration-read", action="store_true",
                        help="skip reading feature report 0x05 before sending (the probe that lights the ring reads it)")
    parser.add_argument("--hold", type=float, default=4.0, help="seconds per step before capturing")
    parser.add_argument("--sequence", default="4", help="camera visits under each unchanged LED setting, e.g. 4,0x10,4")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--sample", type=float, default=1.0)
    parser.add_argument("--bc4-layout", choices=["sbs", "stacked", "planar"])
    parser.add_argument("--raw-if8", action="store_true")
    parser.add_argument("--compact-examples", action="store_true", help="losslessly compress raw camera examples and use unmodified L8 PNGs")
    parser.add_argument("--initial-led-sequence", type=int, default=0,
                        help="last latched sequence from the previous sender; first action increments it")
    parser.add_argument("--step-repeats", type=int, default=1, help="repeat the complete step list")
    parser.add_argument("--shuffle-seed", type=int, help="shuffle each step-list repetition; records the actual plan")
    parser.add_argument("--settings-plan", type=Path, help="wire-validated complete-settings plan from pssense_sony_wire_prepare.py")
    parser.add_argument("--limit-steps", type=int, help="bounded initial smoke run of a prepared plan")
    parser.add_argument("--start-step", type=int, default=0, help="recorded plan offset for a refreshed-phase batch")
    parser.add_argument("--phase-reference-log", type=Path,
                        help="explicit alignment treatment: recent healthy lock from the existing Mac phase diagnostic")
    parser.add_argument("--minimum-lead-ms", type=int, default=100,
                        help="captured PRESCAN future lead; 100 ms leaves margin for receipt-clock catch-up")
    parser.add_argument("--off-roi", type=int, nargs=4, metavar=("X0", "Y0", "X1", "Y1"),
                        help="camera0 ring ROI; stop if >25%% OFF frames have >10 pixels above 100 DN")
    parser.add_argument("--prescan-gate", action="store_true", help="require ring pixels in at least 50%% of PRESCAN control frames")
    args = parser.parse_args()
    out = args.out.expanduser().resolve()
    if str(out).startswith(("/tmp", "/private/tmp")):
        print("refusing to write under /tmp", file=sys.stderr)
        return 2
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        parser.error("output directory must be empty")
    names = args.steps.split(",")
    if any(name not in {*STEPS, "calib"} for name in names) or args.hold < 0.4 or args.sample <= 0 or args.repeat < 1 or args.step_repeats < 1:
        parser.error("invalid steps or duration (hold must be at least 0.4 seconds)")
    if args.off_roi:
        x0, y0, x1, y1 = args.off_roi
        if not (0 <= x0 < x1 <= 508 and 0 <= y0 < y1 <= 508):
            parser.error("OFF ROI must be a nonempty rectangle within the active 508x508 image")
    rng = random.Random(args.shuffle_seed)
    plan = []
    for _ in range(args.step_repeats):
        block = names.copy()
        if args.shuffle_seed is not None:
            rng.shuffle(block)
        plan.extend(block)
    prepared = None
    if args.settings_plan:
        prepared = json.loads(args.settings_plan.expanduser().read_text())
        if prepared["format"] != "pssense-observed-settings-plan-v1" or prepared["side"] != args.side:
            parser.error("settings plan format/side mismatch")
        entries = prepared["plan"][args.start_step:args.start_step + args.limit_steps] if args.limit_steps else prepared["plan"][args.start_step:]
        if not entries:
            parser.error("settings plan is empty")
        for entry in entries:
            data = bytes.fromhex(entry["settings_hex"])
            if len(data) != 38 or data[19] not in (1, 2, 5):
                parser.error("unsupported or incomplete captured settings")
            if data[19] == 2 and data[22:26] != bytes(4):
                parser.error("only observed zero-offset BROAD is supported")
        plan = [entry["label"] for entry in entries]
    recorder = Recorder(out / "reports.jsonl")
    arguments = {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()}
    recorder.write("run_start", side=args.side, plan=plan, prepared=prepared, arguments=arguments)
    reference = phase_reference(args.phase_reference_log.read_text(), args.side) if args.phase_reference_log else None
    if reference:
        if prepared is None:
            parser.error("phase reference requires a captured settings plan")
        recorder.write("phase_alignment_reference", reference=reference)

    devices = open_devices(PRODUCT[args.side])
    print(f"{args.side}: opened {len(devices)} HID interface(s)")
    if devices and not args.no_calibration_read:
        print("calibration feature reads:", ", ".join(read_calibration(devices[0], recorder)))
    if not devices and (prepared or any(name == "calib" or STEPS[name] is not None for name in names)):
        print("controller not connected (it powers down a while after the host stops talking to it)", file=sys.stderr)
        return 1
    sender = Sender(devices, recorder)
    sender.led_seq = args.initial_led_sequence & 255
    inputs = InputRecorder(devices, recorder)
    deadline = time.monotonic() + 2
    while devices and inputs.latest is None and time.monotonic() < deadline:
        time.sleep(0.01)
    if devices and inputs.latest is None:
        inputs.stop.set()
        inputs.thread.join()
        print("no input reports, so no controller clock", file=sys.stderr)
        return 1

    current: dict = {"cmd": None, "sent": 0, "ok": 0}
    lock = threading.Lock()
    stop = threading.Event()

    pump_errors = []

    def pump():
        while not stop.is_set():
            with lock:
                cmd = current["cmd"]
                if cmd is not None:
                    current["sent"] += 1
                    try:
                        current["ok"] += 1 if sender.send(sender.report(**cmd)) else 0
                    except Exception as exc:
                        pump_errors.append(str(exc))
                        stop.set()
            time.sleep(0.0107)

    thread = threading.Thread(target=pump, daemon=True)
    thread.start()
    log = open(out / "steps.csv", "x")
    log.write("wall_time,index,step,sent,ok,blobs0,blobs1,blobs2,blobs3,bright0,bright1,bright2,bright3\n")
    try:
        for i, name in enumerate(plan):
            if name == "calib":
                print(time.strftime("%H:%M:%S"), f"step {i:2d} calib      re-read calibration:",
                      ", ".join(read_calibration(devices[0], recorder)), flush=True)
                continue
            hold, sample = args.hold, args.sample
            phase = None
            if prepared:
                entry = entries[i]
                original = bytes.fromhex(entry["settings_hex"])
                phase = original[19]
                ticks = inputs.ticks_now() if phase == 1 else 0
                source_phase = entry["prescan_phase_from_receipt_ns"]
                applied_phase = aligned_prescan_phase(original, reference, ticks,
                    time.clock_gettime_ns(time.CLOCK_MONOTONIC)) if phase == 1 and reference else source_phase
                data = rebase_settings(original, ticks, applied_phase, args.minimum_lead_ms * 1_000_000)
                cmd = dict(phase=phase, settings=data)
                hold, sample = entry["settle_s"], entry["sample_s"]
                recorder.write("settings_rebased", index=i, label=name, original_settings_hex=original.hex(),
                               rebased_settings_hex=data.hex(), source=entry["source"], latest_clock=inputs.latest,
                               source_receipt_phase_ns=source_phase, applied_phase_ns=applied_phase,
                               alignment_treatment=bool(reference), minimum_lead_ns=args.minimum_lead_ms * 1_000_000)
            else:
                cmd = None if STEPS[name] is None else dict(STEPS[name])
                phase = cmd.get("phase") if cmd else None
                if cmd is not None and name in TIMED:
                    cmd["cycle_position"] = (inputs.ticks_now() + 150_000) & 0xFFFFFFFF
            with lock:
                # The controller latches LED settings when the sequence number changes: once per step.
                sender.led_seq += 1
                recorder.write("action", index=i, label=name, led_sequence=sender.led_seq & 255,
                               command={k: v.hex() if isinstance(v, bytes) else v for k, v in (cmd or {}).items()},
                               latest_clock=inputs.latest)
                current.update(cmd=cmd, sent=0, ok=0)
            start = time.strftime("%H:%M:%S")
            time.sleep(hold)
            if pump_errors:
                raise RuntimeError(pump_errors[-1])
            step_dir = out / f"{i:02d}-{name}"
            recorder.write("capture_start", index=i, label=name)
            capture(step_dir, args.sequence, args.repeat, args.bc4_layout, args.raw_if8, sample, args.compact_examples)
            recorder.write("capture_end", index=i, label=name)
            with lock:
                sent, ok = current["sent"], current["ok"]
            blobs = led_blobs(step_dir)
            bright = bright_pixels(step_dir)
            recorder.write("step_end", index=i, label=name, sent=sent, ok=ok, blobs=blobs, bright=bright)
            if cmd is not None and (sent == 0 or ok != sent):
                raise RuntimeError("HID writes failed; do not interpret images as an applied setting")
            if args.off_roi and (phase == 5 or phase == 1 and args.prescan_gate):
                x0, y0, x1, y1 = args.off_roi
                paths = camera_images(step_dir, 4, 0)
                counts = [int((cv2.imread(str(p), 0)[y0:y1, x0:x1] > 100).sum()) for p in paths]
                fraction = sum(c > 10 for c in counts) / len(counts) if counts else None
                recorder.write("off_gate" if phase == 5 else "prescan_gate", label=name, roi=args.off_roi, populated_fraction=fraction,
                               frame_count=len(counts), counts=counts)
                if phase == 5 and (fraction is None or fraction > 0.25):
                    raise RuntimeError("OFF gate failed: stop, inspect framing/latch/stuck-lit state; power-cycle before further trials")
                if phase == 1 and (fraction is None or fraction < 0.5):
                    raise RuntimeError("PRESCAN positive control failed; refresh phase before interpreting waveform trials")
            print(f"{start} step {i:2d} {name:10s} sent {ok}/{sent}  LED blobs {blobs}  px>200 {bright}", flush=True)
            log.write(f"{start},{i},{name},{sent},{ok},{','.join(map(str, blobs))},{','.join(map(str, bright))}\n")
            log.flush()
    finally:
        # Latch OFF and keep it alive for 0.5 seconds before closing the link.
        with lock:
            sender.led_seq += 1
            current["cmd"] = dict(phase=5)
            recorder.write("action", label="cleanup_off", led_sequence=sender.led_seq & 255)
        time.sleep(0.5)
        stop.set()
        thread.join()
        inputs.stop.set()
        inputs.thread.join()
        log.close()
        recorder.write("run_end")
        recorder.file.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

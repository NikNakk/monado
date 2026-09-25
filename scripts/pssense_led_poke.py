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
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))

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


def read_calibration(device: int) -> list[str]:
    """Read feature report 0x05 twice (the two calibration parts), as the driver and calibration probe do at start."""
    parts = []
    for _ in range(2):
        buf = (ctypes.c_uint8 * 64)()
        buf[0] = 0x05
        length = ctypes.c_long(64)
        ret = iokit.IOHIDDeviceGetReport(device, K_IOHID_REPORT_FEATURE, 0x05, buf, ctypes.byref(length))
        parts.append(f"ret={ret:#x} part={buf[1]:#04x}")
    return parts


class Sender:
    """Builds and sends 78-byte PS5-layout output reports (report 0x31), as the driver does over Bluetooth."""

    def __init__(self, devices: list[int]):
        self.devices = devices
        self.seq = 0
        self.counter = 0
        self.led_seq = 0
        self.zero_host_timestamp = True

    def report(self, phase: int, period_id: int = 0, cycle_position: int = 0, masks: bytes = b"\x00" * 4,
               flag2: int = 0, status_led: int = 0, cycle_ns: int = 16683000) -> bytes:
        led = struct.pack("<BBBII4s", phase, self.led_seq & 0xFF, period_id, cycle_position & 0xFFFFFFFF,
                          cycle_ns * 3, masks)
        # Zero, as the calibration probe sends: its reports are the ones known to light the ring outside Monado.
        host_us = 0 if self.zero_host_timestamp else (time.monotonic_ns() // 1000) & 0xFFFFFFFF
        settings = struct.pack("<BBBB11sI", 0, flag2, 0, 0, b"\x00" * 11, host_us) + led + struct.pack(
            "<BB2s", 0, status_led, b"\x00\x00")
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
            if iokit.IOHIDDeviceSetReport(dev, K_IOHID_REPORT_OUTPUT, data[0], data, len(data)) == 0:
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
    "all_on": dict(phase=6, period_id=42, masks=b"\xff" * 4),
    # The calibration probe's optically verified "always lit": 2.1 ms pulses every 2.0 ms (PSSENSE_FORCE_IR).
    "forced_on": dict(phase=1, period_id=42, masks=b"\xff" * 4, cycle_ns=2000000),
    "prescan": dict(phase=1, period_id=42, masks=b"\xff" * 4),
    "debug": dict(phase=7),
}


def led_blobs(pose_dir: Path) -> list[int]:
    """LED-shaped bright blobs per camera, median over the captured frames (same filter as the tracker)."""
    out = []
    for s, plane in ((4, 0), (4, 1), (5, 0), (5, 1)):
        counts = []
        for f in sorted(pose_dir.glob(f"mode-04-size-*-set-{s}-example-*-plane{plane}.pgm")):
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
        ims = [cv2.imread(str(f), 0)[:, :508] for f in sorted(pose_dir.glob(f"mode-04-size-*-set-{s}-example-*-plane{plane}.pgm"))]
        out.append(int(np.median([(im > 200).sum() for im in ims])) if ims else -1)
    return out


def capture(out: Path) -> None:
    survey = Path(__file__).resolve().parent / "psvr2_camera_mode_survey.py"
    subprocess.run([sys.executable, str(survey), str(out), "--modes", "4", "--settle", "0.3", "--sample", "0.6",
                    "--examples", "6", "--save-every", "2", "--no-contact-sheet"], capture_output=True, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", type=Path)
    parser.add_argument("--side", choices=["L", "R"], default="R")
    parser.add_argument("--steps", default="watch,off,calib,off,status_on,init,off,forced_on,off")
    parser.add_argument("--no-calibration-read", action="store_true",
                        help="skip reading feature report 0x05 before sending (the probe that lights the ring reads it)")
    parser.add_argument("--hold", type=float, default=4.0, help="seconds per step before capturing")
    args = parser.parse_args()
    out = args.out.expanduser().resolve()
    if str(out).startswith(("/tmp", "/private/tmp")):
        print("refusing to write under /tmp", file=sys.stderr)
        return 2
    out.mkdir(parents=True, exist_ok=True)

    devices = open_devices(PRODUCT[args.side])
    print(f"{args.side}: opened {len(devices)} HID interface(s)")
    if devices and not args.no_calibration_read:
        print("calibration feature reads:", ", ".join(read_calibration(devices[0])))
    if not devices and any(STEPS[name] is not None for name in args.steps.split(",")):
        print("controller not connected (it powers down a while after the host stops talking to it)", file=sys.stderr)
        return 1
    sender = Sender(devices)
    # One clock reading, extrapolated at 3 ticks/us: pausing the report stream to re-read it between steps left
    # the controller ignoring the next command. Reports go out continuously, as the driver and probe send them.
    clock = read_device_clock(devices[0]) if devices else None
    if devices and clock is None:
        print("no input reports, so no controller clock", file=sys.stderr)
        return 1

    def ticks_now() -> int:
        ticks, host_ns = clock
        return ticks + (time.monotonic_ns() - host_ns) * 3 // 1000

    current: dict = {"cmd": None, "sent": 0, "ok": 0}
    lock = threading.Lock()
    stop = threading.Event()

    def pump():
        while not stop.is_set():
            with lock:
                cmd = current["cmd"]
                if cmd is not None:
                    current["sent"] += 1
                    current["ok"] += 1 if sender.send(sender.report(**cmd)) else 0
            time.sleep(0.0107)

    thread = threading.Thread(target=pump, daemon=True)
    thread.start()
    log = open(out / "steps.csv", "a")
    try:
        for i, name in enumerate(args.steps.split(",")):
            if name == "calib":
                print(time.strftime("%H:%M:%S"), f"step {i:2d} calib      re-read calibration:",
                      ", ".join(read_calibration(devices[0])), flush=True)
                continue
            cmd = None if STEPS[name] is None else dict(STEPS[name])
            if cmd is not None and name in TIMED:
                cmd["cycle_position"] = ticks_now() + 50_000_000 * 3 // 1000
            with lock:
                # The controller latches LED settings when the sequence number changes: once per step.
                sender.led_seq += 1
                current.update(cmd=cmd, sent=0, ok=0)
            start = time.strftime("%H:%M:%S")
            time.sleep(args.hold)
            step_dir = out / f"{i:02d}-{name}"
            capture(step_dir)
            with lock:
                sent, ok = current["sent"], current["ok"]
            blobs = led_blobs(step_dir)
            bright = bright_pixels(step_dir)
            print(f"{start} step {i:2d} {name:10s} sent {ok}/{sent}  LED blobs {blobs}  px>200 {bright}", flush=True)
            log.write(f"{start},{i},{name},{sent},{ok},{','.join(map(str, blobs))},{','.join(map(str, bright))}\n")
            log.flush()
    finally:
        stop.set()
        thread.join()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

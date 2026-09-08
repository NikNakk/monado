#!/usr/bin/env python3
"""Survey PS VR2 camera modes over libusb without starting Monado.

The tool claims only interface 6 (camera), cycles selected modes, records packet
metadata, preserves representative raw packets, and opportunistically decodes
simple uncompressed L8 layouts using the VI packet header.

For cross-mode registration, --sequence preserves repeated visits to the same
mode with unique visit-numbered filenames. For example, --sequence 3,12,3
--repeat 3 captures three stationary mode-3 -> mode-12 -> mode-3 cycles without
overwriting either mode-3 visit.

Requires pyusb and a libusb backend. On macOS with Homebrew libusb:
    python3 -m pip install pyusb pillow

Run with SteamVR/Monado/GAV closed:
    python3 scripts/psvr2_camera_mode_survey.py /tmp/psvr2-mode-survey
"""

from __future__ import annotations

import argparse
import csv
import json
import struct
import sys
import time
from collections import defaultdict
from pathlib import Path

try:
    import usb.core
    import usb.util
except ImportError as exc:
    raise SystemExit("pyusb is required: python3 -m pip install pyusb") from exc

PSVR2_VID = 0x054C
PSVR2_PID = 0x0CDE
CAMERA_INTERFACE = 6
CAMERA_ENDPOINT_IN = 0x87
REPORT_SET_CAMERA_MODE = 0x0B
CAMERA_SUBCMD = 0x01
USB_CAM_HEADER_SIZE = 256
USB_CAM_MAX_XFER_SIZE = 1_040_640
MODES = range(1, 17)

# bmRequestType used by Monado: vendor | endpoint recipient | OUT.
CAMERA_CTRL_REQUEST_TYPE = 0x42
CAMERA_CTRL_REQUEST = 0x09


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


def parse_vi_header(packet: bytes) -> dict[str, int | bool]:
    result: dict[str, int | bool] = {"vi": len(packet) >= 2 and packet[:2] == b"VI"}
    if not result["vi"] or len(packet) < 30:
        return result
    (
        version,
        packet_size,
        vts_us,
        sequence_id,
        camera_set,
        image_height,
        active_height,
        image_width,
        active_width,
        unknown2,
    ) = struct.unpack_from("<HIIIHHHHHH", packet, 2)
    result.update(
        version=version,
        header_packet_size=packet_size,
        vts_us=vts_us,
        sequence_id=sequence_id,
        camera_set=camera_set,
        image_height=image_height,
        active_height=active_height,
        image_width=image_width,
        active_width=active_width,
        unknown2=unknown2,
    )
    return result


def write_pgm(path: Path, width: int, height: int, pixels: bytes) -> None:
    if len(pixels) != width * height:
        raise ValueError("PGM payload does not match dimensions")
    with path.open("wb") as f:
        f.write(f"P5\n{width} {height}\n255\n".encode("ascii"))
        f.write(pixels)


def opportunistic_decode(packet: bytes, header: dict[str, int | bool], out_prefix: Path) -> list[str]:
    """Decode only layouts strongly implied by header dimensions and byte count.

    No assumptions about camera ordering are made. If the post-header payload is
    exactly N * width * height bytes, save N contiguous L8 planes. This works
    for mode 4 and reveals other simple planar modes without hard-coding them.
    """
    if not header.get("vi"):
        return []
    width = int(header.get("image_width", 0))
    height = int(header.get("image_height", 0))
    if width <= 0 or height <= 0 or len(packet) <= USB_CAM_HEADER_SIZE:
        return []
    payload = packet[USB_CAM_HEADER_SIZE:]
    plane_size = width * height
    if plane_size == 0 or len(payload) % plane_size != 0:
        return []
    planes = len(payload) // plane_size
    if planes < 1 or planes > 8:
        return []
    paths: list[str] = []
    for i in range(planes):
        path = Path(f"{out_prefix}-plane{i}.pgm")
        write_pgm(path, width, height, payload[i * plane_size : (i + 1) * plane_size])
        paths.append(path.name)
    return paths


def read_packet(dev, timeout_ms: int) -> bytes | None:
    try:
        data = dev.read(CAMERA_ENDPOINT_IN, USB_CAM_MAX_XFER_SIZE, timeout=timeout_ms)
    except usb.core.USBTimeoutError:
        return None
    return bytes(data)


def drain(dev, duration_s: float = 0.25) -> None:
    deadline = time.monotonic() + duration_s
    while time.monotonic() < deadline:
        try:
            dev.read(CAMERA_ENDPOINT_IN, USB_CAM_MAX_XFER_SIZE, timeout=20)
        except usb.core.USBTimeoutError:
            pass


def survey_mode(
    dev,
    mode: int,
    out_dir: Path,
    settle_s: float,
    sample_s: float,
    max_examples: int,
    visit_index: int | None = None,
) -> dict:
    visit_text = f" visit {visit_index}" if visit_index is not None else ""
    print(f"mode 0x{mode:02x}{visit_text}: selecting", flush=True)
    set_camera_mode(dev, mode)
    drain(dev, settle_s)

    deadline = time.monotonic() + sample_s
    counts: dict[tuple[int, int], int] = defaultdict(int)
    examples: dict[tuple[int, int], int] = defaultdict(int)
    rows: list[dict] = []
    decoded: list[str] = []
    packet_index = 0
    visit_prefix = f"visit-{visit_index:02d}-" if visit_index is not None else ""

    while time.monotonic() < deadline:
        packet = read_packet(dev, 100)
        if packet is None:
            continue
        packet_index += 1
        header = parse_vi_header(packet)
        camera_set = int(header.get("camera_set", -1))
        key = (len(packet), camera_set)
        counts[key] += 1
        rows.append(
            {
                "packet_index": packet_index,
                "size": len(packet),
                "vi": int(bool(header.get("vi"))),
                "vts_us": header.get("vts_us", ""),
                "sequence_id": header.get("sequence_id", ""),
                "camera_set": header.get("camera_set", ""),
                "image_width": header.get("image_width", ""),
                "image_height": header.get("image_height", ""),
                "active_width": header.get("active_width", ""),
                "active_height": header.get("active_height", ""),
            }
        )

        if examples[key] < max_examples:
            example_no = examples[key]
            stem = out_dir / (
                f"{visit_prefix}mode-{mode:02x}-size-{len(packet)}-set-{camera_set}-example-{example_no}"
            )
            raw_path = Path(f"{stem}.bin")
            raw_path.write_bytes(packet)
            decoded.extend(opportunistic_decode(packet, header, stem))
            examples[key] += 1

    csv_path = out_dir / f"{visit_prefix}mode-{mode:02x}-packets.csv"
    with csv_path.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "packet_index",
                "size",
                "vi",
                "vts_us",
                "sequence_id",
                "camera_set",
                "image_width",
                "image_height",
                "active_width",
                "active_height",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)

    summary = {
        "mode": mode,
        "visit_index": visit_index,
        "packet_count": len(rows),
        "packet_types": [
            {"size": size, "camera_set": camera_set, "count": count}
            for (size, camera_set), count in sorted(counts.items())
        ],
        "decoded_images": decoded,
        "packets_csv": csv_path.name,
    }
    print(
        f"mode 0x{mode:02x}{visit_text}: {len(rows)} packets, "
        + ", ".join(f"{size}B/set{camera_set}={count}" for (size, camera_set), count in sorted(counts.items())),
        flush=True,
    )
    return summary


def build_contact_sheet(out_dir: Path, summaries: list[dict]) -> str | None:
    try:
        from PIL import Image, ImageDraw, ImageOps
    except ImportError:
        return None

    image_paths: list[tuple[int, int | None, Path]] = []
    for summary in summaries:
        mode = int(summary["mode"])
        visit_index = summary.get("visit_index")
        for name in summary.get("decoded_images", []):
            path = out_dir / name
            if path.exists():
                image_paths.append((mode, visit_index, path))
    if not image_paths:
        return None

    thumb_w, thumb_h = 320, 220
    label_h = 28
    cols = 3
    rows = (len(image_paths) + cols - 1) // cols
    sheet = Image.new("L", (cols * thumb_w, rows * (thumb_h + label_h)), 255)
    draw = ImageDraw.Draw(sheet)
    for i, (mode, visit_index, path) in enumerate(image_paths):
        image = Image.open(path).convert("L")
        thumb = ImageOps.contain(image, (thumb_w, thumb_h))
        x = (i % cols) * thumb_w + (thumb_w - thumb.width) // 2
        y0 = (i // cols) * (thumb_h + label_h)
        y = y0 + label_h + (thumb_h - thumb.height) // 2
        sheet.paste(thumb, (x, y))
        visit_label = f" visit {visit_index}" if visit_index is not None else ""
        draw.text((i % cols * thumb_w + 4, y0 + 5), f"mode 0x{mode:02x}{visit_label}  {path.name}", fill=0)
    path = out_dir / "contact-sheet.png"
    sheet.save(path)
    return path.name


def parse_mode_list(text: str) -> list[int]:
    if "-" in text and "," not in text:
        lo, hi = (int(x, 0) for x in text.split("-", 1))
        modes = list(range(lo, hi + 1))
    else:
        modes = [int(x.strip(), 0) for x in text.split(",") if x.strip()]
    modes = [m for m in modes if 1 <= m <= 16]
    if not modes:
        raise SystemExit("no valid camera modes selected")
    return modes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--settle", type=float, default=0.35, help="seconds to discard after changing mode")
    parser.add_argument("--sample", type=float, default=1.0, help="seconds to sample each mode visit")
    parser.add_argument("--examples", type=int, default=1, help="raw examples per packet-size/camera-set type per visit")
    parser.add_argument("--modes", default="1-16", help="normal survey, e.g. 1-16 or 1,2,3,4,12")
    parser.add_argument(
        "--sequence",
        help="ordered mode visits for registration, preserving duplicates, e.g. 3,12,3",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=1,
        help="repeat --sequence this many times (default 1)",
    )
    args = parser.parse_args()

    if args.repeat < 1:
        raise SystemExit("--repeat must be at least 1")

    sequence_capture = args.sequence is not None
    if sequence_capture:
        base_sequence = parse_mode_list(args.sequence)
        modes = base_sequence * args.repeat
    else:
        if args.repeat != 1:
            raise SystemExit("--repeat is only valid with --sequence")
        base_sequence = None
        modes = parse_mode_list(args.modes)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    dev = usb.core.find(idVendor=PSVR2_VID, idProduct=PSVR2_PID)
    if dev is None:
        raise SystemExit("PS VR2 USB device not found")

    if sequence_capture:
        print(
            "Cross-mode sequence capture: keep the headset and scene completely stationary until the command finishes.",
            flush=True,
        )
        print("Capture plan: " + " -> ".join(f"0x{mode:02x}" for mode in modes), flush=True)

    claimed = False
    summaries: list[dict] = []
    try:
        try:
            dev.set_configuration()
        except usb.core.USBError:
            # Existing configuration is normally already correct on macOS.
            pass
        usb.util.claim_interface(dev, CAMERA_INTERFACE)
        claimed = True
        try:
            dev.set_interface_altsetting(interface=CAMERA_INTERFACE, alternate_setting=0)
        except usb.core.USBError:
            pass

        for visit_index, mode in enumerate(modes):
            preserved_visit = visit_index if sequence_capture else None
            try:
                summaries.append(
                    survey_mode(
                        dev,
                        mode,
                        args.output_dir,
                        args.settle,
                        args.sample,
                        args.examples,
                        visit_index=preserved_visit,
                    )
                )
            except Exception as exc:
                print(f"mode 0x{mode:02x}: ERROR: {exc}", file=sys.stderr)
                summaries.append(
                    {
                        "mode": mode,
                        "visit_index": preserved_visit,
                        "error": str(exc),
                        "packet_count": 0,
                        "packet_types": [],
                    }
                )
    finally:
        try:
            set_camera_mode(dev, 0)
        except Exception:
            pass
        if claimed:
            try:
                usb.util.release_interface(dev, CAMERA_INTERFACE)
            except Exception:
                pass
        usb.util.dispose_resources(dev)

    contact_sheet = build_contact_sheet(args.output_dir, summaries)
    result = {
        "format": "psvr2-camera-mode-survey-v2",
        "created_unix_s": time.time(),
        "sequence_capture": sequence_capture,
        "base_sequence": base_sequence,
        "repeat": args.repeat if sequence_capture else 1,
        "capture_plan": modes,
        "settle_s": args.settle,
        "sample_s": args.sample,
        "examples_per_packet_type": args.examples,
        "modes": summaries,
        "contact_sheet": contact_sheet,
        "notes": [
            "Raw .bin packets are authoritative for undocumented layouts.",
            "PGMs are emitted only when VI header dimensions exactly tile the post-header payload as contiguous L8 planes.",
            "Plane number is not assumed to identify a physical camera until cross-mode registration proves it.",
            "Sequence captures preserve repeated visits with visit-NN filename prefixes and require a stationary headset/scene.",
        ],
    }
    (args.output_dir / "survey.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"wrote {args.output_dir / 'survey.json'}")
    if contact_sheet:
        print(f"wrote {args.output_dir / contact_sheet}")
    else:
        print("no contact sheet generated (install Pillow, or no simple L8 layouts were decoded)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

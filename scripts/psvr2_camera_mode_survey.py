#!/usr/bin/env python3
"""Survey PS VR2 camera modes over libusb without starting Monado.

The tool claims only interface 6 (camera), cycles selected modes, records packet
metadata, preserves representative raw packets, and decodes known/simple L8
layouts without assigning physical camera identities.

For cross-mode registration, --sequence preserves repeated visits to the same
mode with unique visit-numbered filenames. For example, --sequence 3,12,3
--repeat 3 captures three stationary mode-3 -> mode-12 -> mode-3 cycles without
overwriting either mode-3 visit.

Requires pyusb and a libusb backend. On macOS with Homebrew libusb:
    python3 -m pip install pyusb pillow
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
except ImportError:
    # Keep the stream framer importable for dependency-free unit tests.
    usb = None

PSVR2_VID = 0x054C
PSVR2_PID = 0x0CDE
CAMERA_INTERFACE = 6
CAMERA_ENDPOINT_IN = 0x87
REPORT_SET_CAMERA_MODE = 0x0B
CAMERA_SUBCMD = 0x01
USB_CAM_HEADER_SIZE = 256
USB_CAM_MAX_XFER_SIZE = 1_040_640
# macOS/libusb may wait for a large requested transfer until its timeout and
# return only the bytes received so far.  Keep reads below the observed Darwin
# transfer fragment limit, then frame the bulk byte stream ourselves.
USB_CAM_READ_SIZE = 64 * 1024
CAMERA_CTRL_REQUEST_TYPE = 0x42
CAMERA_CTRL_REQUEST = 0x09
# These set numbers are established packet-format identifiers, not physical
# camera assignments.  They let sequence captures reject queued frames from
# the previous high-bandwidth mode before starting the visit timer.
EXPECTED_CAMERA_SETS = {
    3: {0, 3},
    4: {4, 5},
    12: {8, 9},
}
CAMERA_MODE_SYNC_TIMEOUT_S = 3.0


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


class CameraPacketFramer:
    """Reassemble complete VI camera packets from arbitrary USB read chunks."""

    def __init__(self) -> None:
        self.buffer = bytearray()
        self.discarded_bytes = 0
        self.invalid_headers = 0

    @staticmethod
    def _valid_header(header: dict[str, int | bool]) -> bool:
        packet_size = int(header.get("header_packet_size", 0))
        width = int(header.get("image_width", 0))
        height = int(header.get("image_height", 0))
        return (
            bool(header.get("vi"))
            and int(header.get("version", 0)) == 0x200
            and packet_size >= USB_CAM_HEADER_SIZE
            and packet_size <= USB_CAM_MAX_XFER_SIZE
            and width > 0
            and height > 0
            and width <= 4096
            and height <= 4096
        )

    def feed(self, chunk: bytes) -> list[bytes]:
        if chunk:
            self.buffer.extend(chunk)

        packets = []
        while True:
            signature = self.buffer.find(b"VI")
            if signature < 0:
                # Preserve a trailing V in case the signature straddles reads.
                keep = 1 if self.buffer.endswith(b"V") else 0
                discard = len(self.buffer) - keep
                if discard > 0:
                    del self.buffer[:discard]
                    self.discarded_bytes += discard
                break
            if signature > 0:
                del self.buffer[:signature]
                self.discarded_bytes += signature

            if len(self.buffer) < 30:
                break
            header = parse_vi_header(self.buffer)
            if not self._valid_header(header):
                # This was an incidental VI byte pair in image data.
                del self.buffer[:2]
                self.discarded_bytes += 2
                self.invalid_headers += 1
                continue

            packet_size = int(header["header_packet_size"])
            if len(self.buffer) < packet_size:
                break
            packets.append(bytes(self.buffer[:packet_size]))
            del self.buffer[:packet_size]

        return packets

    def reset(self) -> None:
        self.discarded_bytes += len(self.buffer)
        self.buffer.clear()


def write_pgm(path: Path, width: int, height: int, pixels: bytes | bytearray) -> None:
    if len(pixels) != width * height:
        raise ValueError("PGM payload does not match dimensions")
    with path.open("wb") as f:
        f.write(f"P5\n{width} {height}\n255\n".encode("ascii"))
        f.write(pixels)


def decode_l8(packet: bytes, header: dict[str, int | bool], out_prefix: Path) -> tuple[list[str], str | None]:
    """Decode layouts demonstrated by captures, otherwise only safe planar L8.

    819456-byte 640x640x2 packets (modes 1/2/3/etc.) are a 1280x640
    side-by-side raster: every row contains 640 pixels from plane 0 followed by
    640 from plane 1. Treating the two images as contiguous planes produces
    ghosted composites and is incorrect.

    Other packets are decoded only when the payload exactly tiles as contiguous
    width*height L8 planes. This covers mode 4 and mode-12 sets 8/9.
    """
    if not header.get("vi"):
        return [], None
    width = int(header.get("image_width", 0))
    height = int(header.get("image_height", 0))
    if width <= 0 or height <= 0 or len(packet) <= USB_CAM_HEADER_SIZE:
        return [], None

    payload = packet[USB_CAM_HEADER_SIZE:]
    plane_size = width * height
    paths: list[str] = []

    if len(packet) == 819456 and width == 640 and height == 640 and len(payload) == 2 * plane_size:
        stride = width * 2
        for plane in range(2):
            pixels = bytearray(plane_size)
            for y in range(height):
                src = y * stride + plane * width
                dst = y * width
                pixels[dst : dst + width] = payload[src : src + width]
            path = Path(f"{out_prefix}-plane{plane}.pgm")
            write_pgm(path, width, height, pixels)
            paths.append(path.name)
        return paths, "sbs_l8"

    if plane_size == 0 or len(payload) % plane_size != 0:
        return [], None
    planes = len(payload) // plane_size
    if planes < 1 or planes > 8:
        return [], None
    for plane in range(planes):
        path = Path(f"{out_prefix}-plane{plane}.pgm")
        write_pgm(path, width, height, payload[plane * plane_size : (plane + 1) * plane_size])
        paths.append(path.name)
    return paths, "planar_l8"


def read_chunk(dev, timeout_ms: int) -> bytes | None:
    try:
        data = dev.read(CAMERA_ENDPOINT_IN, USB_CAM_READ_SIZE, timeout=timeout_ms)
    except usb.core.USBTimeoutError:
        return None
    return bytes(data)


def drain(dev, duration_s: float = 0.25) -> None:
    deadline = time.monotonic() + duration_s
    while time.monotonic() < deadline:
        try:
            dev.read(CAMERA_ENDPOINT_IN, USB_CAM_READ_SIZE, timeout=20)
        except usb.core.USBTimeoutError:
            pass


def survey_mode(
    dev,
    mode: int,
    out_dir: Path,
    settle_s: float,
    sample_s: float,
    max_examples: int,
    save_every: int = 1,
    visit_index: int | None = None,
) -> dict:
    visit_text = f" visit {visit_index}" if visit_index is not None else ""
    print(f"mode 0x{mode:02x}{visit_text}: selecting", flush=True)
    set_camera_mode(dev, mode)
    drain(dev, settle_s)

    expected_camera_sets = EXPECTED_CAMERA_SETS.get(mode)
    synchronized = expected_camera_sets is None
    sync_deadline = time.monotonic() + CAMERA_MODE_SYNC_TIMEOUT_S
    deadline = time.monotonic() + sample_s if synchronized else None
    counts: dict[tuple[int, int], int] = defaultdict(int)
    examples: dict[tuple[int, int], int] = defaultdict(int)
    rows: list[dict] = []
    decoded: list[str] = []
    layouts: set[str] = set()
    packet_index = 0
    usb_chunk_count = 0
    usb_byte_count = 0
    empty_read_count = 0
    pre_sync_discarded_frames = 0
    framer = CameraPacketFramer()
    visit_prefix = f"visit-{visit_index:02d}-" if visit_index is not None else ""

    while deadline is None or time.monotonic() < deadline:
        if deadline is None and time.monotonic() >= sync_deadline:
            break
        chunk = read_chunk(dev, 100)
        if chunk is None:
            continue
        usb_chunk_count += 1
        usb_byte_count += len(chunk)
        if not chunk:
            empty_read_count += 1
            continue

        for packet in framer.feed(chunk):
            header = parse_vi_header(packet)
            camera_set = int(header["camera_set"])
            if not synchronized:
                if camera_set not in expected_camera_sets:
                    pre_sync_discarded_frames += 1
                    continue
                synchronized = True
                deadline = time.monotonic() + sample_s

            packet_index += 1
            key = (len(packet), camera_set)
            counts[key] += 1
            row = {
                "packet_index": packet_index,
                "host_monotonic_ns": time.monotonic_ns(),
                "size": len(packet),
                "vi": 1,
                "vts_us": header["vts_us"],
                "sequence_id": header["sequence_id"],
                "camera_set": header["camera_set"],
                "image_width": header["image_width"],
                "image_height": header["image_height"],
                "active_width": header["active_width"],
                "active_height": header["active_height"],
                "raw_file": "",
                "decoded_files": "",
            }
            rows.append(row)

            if examples[key] < max_examples and (counts[key] - 1) % save_every == 0:
                example_no = examples[key]
                stem = out_dir / (
                    f"{visit_prefix}mode-{mode:02x}-size-{len(packet)}-set-{camera_set}-example-{example_no}"
                )
                raw_path = Path(f"{stem}.bin")
                raw_path.write_bytes(packet)
                image_names, layout = decode_l8(packet, header, stem)
                row["raw_file"] = raw_path.name
                row["decoded_files"] = ";".join(image_names)
                decoded.extend(image_names)
                if layout is not None:
                    layouts.add(layout)
                examples[key] += 1

    csv_path = out_dir / f"{visit_prefix}mode-{mode:02x}-packets.csv"
    with csv_path.open("w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "packet_index",
                "host_monotonic_ns",
                "size",
                "vi",
                "vts_us",
                "sequence_id",
                "camera_set",
                "image_width",
                "image_height",
                "active_width",
                "active_height",
                "raw_file",
                "decoded_files",
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
        "decoded_layouts": sorted(layouts),
        "packets_csv": csv_path.name,
        "usb_chunk_count": usb_chunk_count,
        "usb_byte_count": usb_byte_count,
        "empty_read_count": empty_read_count,
        "discarded_prefix_bytes": framer.discarded_bytes,
        "invalid_header_count": framer.invalid_headers,
        "partial_bytes_at_end": len(framer.buffer),
        "expected_camera_sets": sorted(expected_camera_sets) if expected_camera_sets is not None else None,
        "mode_synchronized": synchronized,
        "pre_sync_discarded_frames": pre_sync_discarded_frames,
    }
    print(
        f"mode 0x{mode:02x}{visit_text}: {len(rows)} packets, "
        + ", ".join(f"{size}B/set{camera_set}={count}" for (size, camera_set), count in sorted(counts.items())),
        flush=True,
    )
    if not synchronized:
        print(
            f"mode 0x{mode:02x}{visit_text}: did not observe expected camera sets "
            f"{sorted(expected_camera_sets)} within {CAMERA_MODE_SYNC_TIMEOUT_S:.1f}s",
            file=sys.stderr,
        )
    return summary


def build_contact_sheet(out_dir: Path, summaries: list[dict]) -> tuple[str | None, str | None]:
    try:
        from PIL import Image, ImageDraw, ImageOps
    except ImportError as exc:
        return None, f"Pillow is not installed: {exc}"

    image_paths: list[tuple[int, int | None, Path]] = []
    for summary in summaries:
        mode = int(summary["mode"])
        visit_index = summary.get("visit_index")
        for name in summary.get("decoded_images", []):
            path = out_dir / name
            if path.exists():
                image_paths.append((mode, visit_index, path))
    if not image_paths:
        return None, "no decodable L8 images were captured"

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
    return path.name, None


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
    parser.add_argument("--save-every", type=int, default=1,
                        help="save every Nth packet of each type (default: 1; CSV still records every packet)")
    parser.add_argument("--no-contact-sheet", action="store_true",
                        help="skip contact-sheet generation, useful for long calibration captures")
    parser.add_argument("--modes", default="1-16", help="normal survey, e.g. 1-16 or 1,2,3,4,12")
    parser.add_argument("--sequence", help="ordered mode visits preserving duplicates, e.g. 3,12,3")
    parser.add_argument("--repeat", type=int, default=1, help="repeat --sequence this many times")
    args = parser.parse_args()

    if usb is None:
        raise SystemExit("pyusb is required: python3 -m pip install pyusb")

    if args.repeat < 1:
        raise SystemExit("--repeat must be at least 1")
    if args.examples < 1:
        raise SystemExit("--examples must be at least 1")
    if args.save_every < 1:
        raise SystemExit("--save-every must be at least 1")
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
        print("Keep the headset and scene completely stationary until capture finishes.", flush=True)
        print("Capture plan: " + " -> ".join(f"0x{mode:02x}" for mode in modes), flush=True)

    claimed = False
    summaries: list[dict] = []
    try:
        try:
            dev.set_configuration()
        except usb.core.USBError:
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
                        args.save_every,
                        visit_index=preserved_visit,
                    )
                )
            except Exception as exc:
                print(f"mode 0x{mode:02x}: ERROR: {exc}", file=sys.stderr)
                summaries.append(
                    {"mode": mode, "visit_index": preserved_visit, "error": str(exc), "packet_count": 0, "packet_types": []}
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

    if args.no_contact_sheet:
        contact_sheet, contact_sheet_error = None, "disabled by --no-contact-sheet"
    else:
        contact_sheet, contact_sheet_error = build_contact_sheet(args.output_dir, summaries)
    result = {
        "format": "psvr2-camera-mode-survey-v4",
        "created_unix_s": time.time(),
        "sequence_capture": sequence_capture,
        "base_sequence": base_sequence,
        "repeat": args.repeat if sequence_capture else 1,
        "capture_plan": modes,
        "settle_s": args.settle,
        "sample_s": args.sample,
        "examples_per_packet_type": args.examples,
        "save_every_per_packet_type": args.save_every,
        "modes": summaries,
        "contact_sheet": contact_sheet,
        "notes": [
            "Raw .bin packets are authoritative for undocumented layouts.",
            "819456-byte 640x640x2 packets are decoded as a 1280x640 side-by-side raster, not contiguous planes.",
            "Other PGMs are emitted only for payloads demonstrated/safely inferred as contiguous L8 planes.",
            "USB reads are byte-stream fragments; packet rows and raw examples contain reassembled VI frames.",
            "Plane number is not assumed to identify a physical camera until cross-mode registration proves it.",
            "Sequence captures preserve repeated visits with visit-NN filename prefixes and require a stationary headset/scene.",
        ],
    }
    (args.output_dir / "survey.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"wrote {args.output_dir / 'survey.json'}")
    if contact_sheet:
        print(f"wrote {args.output_dir / contact_sheet}")
    else:
        print(f"no contact sheet generated ({contact_sheet_error})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

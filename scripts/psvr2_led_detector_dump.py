#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Read a PSVR2_LED_DETECTOR_RECORD file (the headset's LED detector stream, USB interface 8).

Usage:
    psvr2_led_detector_dump.py FILE [--csv RECORDS.csv]

Prints a summary: packets, rate, header counter and device-timestamp continuity, records per section. With --csv,
writes one row per populated record with its packet's host receipt time, header counter and device time, the
section (camera, numbered as Monado's mode-4 cameras) and index, and the record's fields as established by
comparison with Monado's own blobs (5 Oct): a u16 tag at 0 (1, sometimes 2), a u16 at 2 (low byte 0xff), the
bounding box xmin, xmax, ymin, ymax at 4-11 in mode-4 pixels, then u32 m00 (summed intensity), m10 and m01
(intensity-weighted x and y sums measured from xmin, ymin; xmin + m10/m00 matches Monado's blob centroid to 0.01 px
median), and three further u32 at 24-35 that scale with spot size (probably second moments; unconfirmed).

File format (little-endian): "PSLD" + u32 version 1; per packet u64 host monotonic ns, u32 transfer length, u32
stored length, stored bytes. A full packet (36,944 bytes) is stored as its 64-byte header followed, per section, by
the u32 record count and that many 36-byte records.
"""

import argparse
import csv
import statistics
import struct
import sys
from collections import Counter

FULL = 36944
HEADER = 64
SECTIONS = 4
RECORD = 36


def packets(path):
    data = open(path, "rb").read()
    if data[:4] != b"PSLD":
        sys.exit("not a PSLD file")
    pos = 8
    while pos + 16 <= len(data):
        host_ns, length, stored = struct.unpack_from("<QII", data, pos)
        pos += 16
        body = data[pos:pos + stored]
        pos += stored
        if len(body) < stored:
            break
        sections = []
        if length == FULL and stored < FULL:
            p = HEADER
            for _ in range(SECTIONS):
                (count,) = struct.unpack_from("<I", body, p)
                kept = min(count, 256)
                sections.append((count, [body[p + 4 + i * RECORD:p + 4 + (i + 1) * RECORD] for i in range(kept)]))
                p += 4 + kept * RECORD
        elif length == FULL:
            for s in range(SECTIONS):
                base = HEADER + s * (4 + 256 * RECORD)
                (count,) = struct.unpack_from("<I", body, base)
                kept = min(count, 256)
                sections.append((count, [body[base + 4 + i * RECORD:base + 4 + (i + 1) * RECORD] for i in range(kept)]))
        yield host_ns, length, body[:HEADER], sections


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--csv")
    args = ap.parse_args()

    rows = list(packets(args.file))
    if not rows:
        sys.exit("no packets")
    lengths = Counter(r[1] for r in rows)
    full = [r for r in rows if r[1] == FULL]
    span = (rows[-1][0] - rows[0][0]) / 1e9
    print(f"{len(rows)} packets over {span:.1f} s ({len(rows) / max(span, 1e-9):.1f}/s); lengths {dict(lengths)}")
    if full:
        tags = Counter(h[:2] for _, _, h, _ in full)
        counters = [struct.unpack_from("<I", h, 20)[0] for _, _, h, _ in full]
        device_us = [struct.unpack_from("<I", h, 8)[0] for _, _, h, _ in full]
        jumps = Counter(b - a for a, b in zip(counters, counters[1:]))
        dt = [(b - a) & 0xFFFFFFFF for a, b in zip(device_us, device_us[1:])]
        print(f"header tags {dict(tags)}; counter steps {dict(jumps.most_common(5))}; "
              f"device time step median {statistics.median(dt) if dt else 0} us")
        for s in range(SECTIONS):
            counts = [sec[s][0] for _, _, _, sec in full]
            print(f"  section {s}: records per packet mean {statistics.mean(counts):.2f}, max {max(counts)}, "
                  f"non-empty {100 * sum(1 for c in counts if c) / len(counts):.0f}%")
    if args.csv:
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["host_ns", "counter", "device_us", "section", "index", "tag", "u16_2", "xmin", "xmax",
                        "ymin", "ymax", "m00", "m10", "m01", "u32_24", "u32_28", "u32_32", "cx", "cy"])
            for host_ns, _, h, sections in full:
                counter = struct.unpack_from("<I", h, 20)[0]
                device = struct.unpack_from("<I", h, 8)[0]
                for s, (_, records) in enumerate(sections):
                    for i, rec in enumerate(records):
                        f = struct.unpack_from("<6H6I", rec)
                        m00 = f[6]
                        cx = f[2] + f[7] / m00 if m00 else ""
                        cy = f[4] + f[8] / m00 if m00 else ""
                        w.writerow([host_ns, counter, device, s, i, *f, cx, cy])
        print(f"records written to {args.csv}")


if __name__ == "__main__":
    main()

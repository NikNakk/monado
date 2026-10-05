#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Read a PSVR2_LED_DETECTOR_RECORD file (the headset's LED detector stream, USB interface 8).

Usage:
    psvr2_led_detector_dump.py FILE [--csv RECORDS.csv]

Prints a summary: packets, rate, header counter and device-timestamp continuity, records per section. With --csv,
writes one row per populated record with its packet's host receipt time, header counter and device time, the
section (camera) and index, and the record's fields as currently understood from wire observation only: a u16 tag
at 0, a u16 at 2, four u16 coordinate-like values at 4-11 (ordered pairs within 0-508), and six u32 at 12-35 whose
meaning is not yet known.

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
            w.writerow(["host_ns", "counter", "device_us", "section", "index", "tag", "u16_2", "x0", "y0", "x1",
                        "y1", "u32_12", "u32_16", "u32_20", "u32_24", "u32_28", "u32_32"])
            for host_ns, _, h, sections in full:
                counter = struct.unpack_from("<I", h, 20)[0]
                device = struct.unpack_from("<I", h, 8)[0]
                for s, (_, records) in enumerate(sections):
                    for i, rec in enumerate(records):
                        w.writerow([host_ns, counter, device, s, i, *struct.unpack_from("<6H6I", rec)])
        print(f"records written to {args.csv}")


if __name__ == "__main__":
    main()

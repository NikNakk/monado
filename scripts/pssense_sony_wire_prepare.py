#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
"""Prepare wire-only Sense settings from the supplied PCAP and corrected oracle CSVs.

No Sony private structure or algorithm is used. Public PCAP-NG enhanced packet
blocks, HCI ACL/L2CAP lengths and HIDP CRC validate signature candidates. Native
settings are independently cross-checked against the supplied observable CSV and
the existing Monado 38-byte settings layout. This tool does not send to hardware.
"""
import argparse
import collections
import csv
import gzip
import hashlib
import io
import json
from pathlib import Path
import random
import re
import math
import struct
import zipfile
import zlib


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def packets(path):
    with gzip.open(path, "rb") as f:
        while header := f.read(8):
            if len(header) != 8:
                raise ValueError("truncated PCAP-NG block header")
            kind, size = struct.unpack("<II", header)
            if size < 12 or size > 16 * 1024 * 1024 or size % 4:
                raise ValueError("invalid PCAP-NG block size")
            body = f.read(size - 8)
            if len(body) != size - 8 or struct.unpack_from("<I", body, len(body) - 4)[0] != size:
                raise ValueError("truncated PCAP-NG block or mismatched trailer")
            if kind == 0x0A0D0D0A and struct.unpack_from("<I", body)[0] != 0x1A2B3C4D:
                raise ValueError("only little-endian PCAP-NG is supported")
            if kind == 1:
                if struct.unpack_from("<H", body)[0] != 290:
                    raise ValueError("expected ETW linktype 290")
            if kind == 6:
                interface, hi, lo, captured, original = struct.unpack_from("<IIIII", body)
                if interface != 0 or captured != original or 20 + captured > len(body) - 4:
                    raise ValueError("unsupported interface or truncated packet")
                # This supplied trace's interface uses PCAP-NG default microsecond resolution.
                yield (hi << 32) | lo, body[20:20 + captured]


def reports(packet):
    for seed in (0xA1, 0xA2):
        start = 0
        while (j := packet.find(bytes([seed, 0x31]), start)) >= 0:
            start = j + 2
            if j < 8 or len(packet) < j + 79:
                continue
            handle, acl_len, l2cap_len, cid = struct.unpack_from("<HHHH", packet, j - 8)
            if acl_len != 83 or l2cap_len != 79 or cid not in (0x42, 0x43):
                continue
            report = packet[j + 1:j + 79]
            if zlib.crc32(report[:-4], zlib.crc32(bytes([seed]))) != int.from_bytes(report[-4:], "little"):
                continue
            yield seed, handle & 0xFFF, cid, report


def verify_settings(report, row):
    """Alignment of observable fields, validated for every matched packet, not a private offset table."""
    settings = report[2:40]
    actual = dict(mode=report[1], flags=settings[0], report_timestamp=struct.unpack_from("<I", settings, 15)[0],
                  phase=settings[19], sequence=settings[20], period=settings[21],
                  cycle_position=struct.unpack_from("<i", settings, 22)[0],
                  cycle_length=struct.unpack_from("<I", settings, 26)[0],
                  **{f"led{i}": settings[30 + i] for i in range(4)})
    if any(int(row[k]) != v for k, v in actual.items()):
        raise ValueError("native report does not match observable CSV/settings layout")
    return settings


def rebase_settings(settings, controller_ticks, phase_ns, minimum_lead_ns=50_000_000):
    data = bytearray(settings)
    if len(data) != 38:
        raise ValueError("complete 38-byte settings required")
    if data[19] == 1:
        period_ns = int.from_bytes(data[26:30], "little") // 3
        if period_ns <= 0 or phase_ns is None:
            raise ValueError("PRESCAN requires a period and captured phase")
        delta_ns = int(phase_ns) % period_ns
        if delta_ns < minimum_lead_ns:
            delta_ns += ((minimum_lead_ns - delta_ns + period_ns - 1) // period_ns) * period_ns
        struct.pack_into("<I", data, 22, (controller_ticks + delta_ns * 3 // 1000) & 0xFFFFFFFF)
    elif data[19] == 2 and data[22:26] != bytes(4):
        raise ValueError("only observed zero BROAD relative offsets are supported")
    return bytes(data)


def phase_reference(log, side):
    """Read an existing Mac diagnostic's locked pulse anchor, not a Sony clock."""
    locked, anchor = False, None
    for line in log.splitlines():
        if f"LED_BOOTSTRAP side={side} " in line:
            if "event=locked " in line:
                locked = True
            elif any(f"event={event} " in line for event in ("lost", "scan_start", "stuck_lit")):
                locked, anchor = False, None
        if locked and f"LED_SCHEDULE side={side} " in line:
            fields = dict(re.findall(r"(\w+)=(-?\d+)", line))
            anchor = {k: int(fields[k]) for k in ("cycle_position", "blink_host", "period", "pulse")}
    if anchor is None:
        raise ValueError("no healthy locked phase anchor in diagnostic log")
    return anchor


def aligned_prescan_phase(settings, reference, controller_ticks, clock_monotonic_ns):
    """Globally align a settings trial to a separately measured Mac pulse phase.

    The captured cycle length stays unchanged. This is an explicitly logged
    alignment treatment, distinct from preserving the source's receipt phase.
    Refuse a power-cycle/clock discontinuity instead of replaying its old ticks.
    """
    elapsed = clock_monotonic_ns - reference["blink_host"]
    if not 0 <= elapsed <= 90_000_000_000:
        raise ValueError("phase reference must be fresh (under 90 seconds); refresh with the existing diagnostic")
    estimate = (reference["cycle_position"] + elapsed * 3 // 1000) & 0xffffffff
    error = ((controller_ticks - estimate + 2**31) % 2**32) - 2**31
    if abs(error) > 150_000:
        raise ValueError("controller clock discontinuity versus locked reference; rerun phase diagnostic")
    period_ns = int.from_bytes(settings[26:30], "little") // 3
    if period_ns <= 0:
        raise ValueError("invalid captured cycle")
    # Project the original healthy pulse centre to the present, then advance
    # by whole captured cycles with rebase_settings' normal minimum lead.
    age_ticks = ((controller_ticks - reference["cycle_position"] + 2**31) % 2**32) - 2**31
    cycle_ticks = period_ns * 3 / 1000
    cycles = math.ceil((age_ticks + 180_000) / cycle_ticks)
    delta_ns = int((cycles * cycle_ticks - age_ticks) * 1000 / 3)
    return delta_ns % period_ns


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("handoff", type=Path)
    parser.add_argument("pcap", type=Path)
    parser.add_argument("out", type=Path)
    parser.add_argument("--side", choices=["R", "L"], default="R")
    parser.add_argument("--seed", type=int, default=20261004)
    args = parser.parse_args()
    out = args.out.expanduser().resolve()
    if out == Path("/private/tmp") or Path("/private/tmp") in out.parents:
        parser.error("never record under /tmp")
    out.mkdir(parents=True, exist_ok=True)
    if any(out.iterdir()):
        parser.error("output must be empty")
    with zipfile.ZipFile(args.handoff) as z:
        rows = list(csv.DictReader(io.StringIO(gzip.decompress(z.read("analysis/a231-all-handles.csv.gz")).decode())))
        corrected = list(csv.DictReader(io.StringIO(gzip.decompress(z.read(f"analysis/{args.side}/a231.csv.gz")).decode())))
        inventory = json.loads(z.read("analysis/capture-inventory.json"))
        # Extract only readme/observable analysis; do not copy Windows implementation/provenance code into Monado.
        (out / "README-handoff.txt").write_bytes(z.read("README-handoff.txt"))
        (out / "capture-summary.md").write_bytes(z.read("analysis/capture-summary.md"))
    expected_handle = 20 if args.side == "R" else 21
    if {int(r["acl_handle"]) for r in corrected} != {expected_handle}:
        raise ValueError("corrected side/handle mismatch")
    indexed = {(int(r["capture_ts_us"]), int(r["acl_handle"])): r for r in rows}
    native = []
    latest_rx = {}
    counts = collections.Counter()
    with (out / "wire-reports.jsonl").open("x") as log:
        for ts, packet in packets(args.pcap):
            for seed, handle, cid, report in reports(packet):
                if handle not in (20, 21):
                    continue
                counts[f"{handle}_{seed:02x}"] += 1
                rec = dict(capture_realtime_us=ts, handle=handle, cid=cid,
                           direction="rx" if seed == 0xA1 else "tx", data_hex=report.hex())
                log.write(json.dumps(rec) + "\n")
                if seed == 0xA1:
                    latest_rx[handle] = (ts, int.from_bytes(report[49:53], "little"))
                else:
                    row = indexed.get((ts, handle))
                    if row is None:
                        continue
                    settings = verify_settings(report, row)
                    if handle == expected_handle:
                        native.append(dict(source=row, report_hex=report.hex(), settings_hex=settings.hex(),
                                           preceding_rx_clock=latest_rx.get(handle)))
    selected_rows = {int(r["capture_ts_us"]) for r in corrected}
    verified_rows = {int(r["source"]["capture_ts_us"]) for r in native}
    if not selected_rows <= verified_rows:
        raise ValueError("missing complete CRC-valid reports for selected corrected CSV")
    (out / "native-settings.json").write_text(json.dumps(native, indent=2) + "\n")

    def vector(record, label):
        settings = bytes.fromhex(record["settings_hex"])
        phase, cycle_ns = settings[19], int.from_bytes(settings[26:30], "little") // 3
        delta_ns = None
        if phase == 1:
            if not record["preceding_rx_clock"] or cycle_ns <= 0:
                raise ValueError("PRESCAN lacks an input-clock anchor")
            rx_ts, ticks = record["preceding_rx_clock"]
            tx_ts = int(record["source"]["capture_ts_us"])
            if not 0 <= tx_ts - rx_ts <= 100000:
                raise ValueError("captured input-clock anchor stale")
            estimate = (ticks + (tx_ts - rx_ts) * 3) & 0xFFFFFFFF
            position = int.from_bytes(settings[22:26], "little")
            signed_delta = ((position - estimate + 2**31) % 2**32) - 2**31
            delta_ns = (signed_delta * 1000 // 3) % cycle_ns
        elif phase == 2 and settings[22:26] != bytes(4):
            raise ValueError("this preparation supports only observed zero BROAD relative offsets")
        return dict(label=label, settings_hex=record["settings_hex"], source=record["source"],
                    report_sha256=hashlib.sha256(bytes.fromhex(record["report_hex"])).hexdigest(),
                    prescan_phase_from_receipt_ns=delta_ns, settle_s=1.0, sample_s=10.0)

    overlap = [r for r in native if int(r["source"]["capture_ts_us"]) in selected_rows]
    # Observed full setting representatives; do not synthesize absent led0 values.
    broad = {}
    for rec in overlap:
        s = bytes.fromhex(rec["settings_hex"])
        if s[19] == 2 and s[0] == 0x12:
            broad.setdefault(s[30:34].hex(), rec)
    off = next(r for r in native if int(r["source"]["phase"]) == 5 and int(r["source"]["flags"]) == 18)
    prescan = next(r for r in overlap if int(r["source"]["phase"]) == 1 and int(r["source"]["period"]) == 32)
    controls = [vector(off, "OFF"), vector(prescan, "PRESCAN")]
    vectors = [vector(rec, f"BROAD-{mask}") for mask, rec in sorted(broad.items())]
    rng = random.Random(args.seed)
    plan = []
    for block in range(3):
        trials = vectors + controls
        rng.shuffle(trials)
        # Each trial gets a dark and a captured PRESCAN base anchor before the tested setting.
        for trial in trials:
            plan.extend([dict(controls[0], label=f"b{block}-baseline-OFF", sample_s=2.0, settle_s=4.0),
                         dict(controls[1], label=f"b{block}-anchor-PRESCAN", sample_s=2.0, settle_s=1.0),
                         dict(trial, label=f"b{block}-{trial['label']}")])
    result = dict(format="pssense-observed-settings-plan-v1", side=args.side, seed=args.seed,
                  handoff_sha256=sha(args.handoff), pcap_sha256=sha(args.pcap), wire_counts=dict(counts),
                  selected_reports_verified=len(selected_rows), vectors=vectors, controls=controls, plan=plan,
                  limitations=["Captured settings are translated to Monado's existing PS5-style framing, not native Sony framing.",
                               "Opaque native transport/haptic tails are retained in wire evidence, not assigned meanings.",
                               "PRESCAN phase is preserved relative to a receipt-biased controller-clock estimate; live camera exposure phase is not calibrated.",
                               "BROAD zero offset is preserved relative to the preceding rebased PRESCAN base.",
                               "No live Sony semantic labels; no private algorithm or geometry used."])
    (out / "settings-plan.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(out=str(out), verified=len(selected_rows), broad_vectors=len(vectors), counts=dict(counts))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

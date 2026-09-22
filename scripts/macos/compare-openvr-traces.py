#!/usr/bin/env python3
"""
Compare OpenVR API call streams from OpenComposite and xrizer.

OpenComposite input should be produced with:
    logAllOpenVRCalls=true
    logGetTrackedProperty=true

xrizer input should be produced with:
    RUST_LOG=xrizer=trace,openvr_calls=trace,tracked_property=trace

The script normalises interface versions (IVRCompositor_027 vs
IVRCompositor029) and prints a compact milestone stream plus the first
SequenceMatcher divergence.
"""

from __future__ import annotations

import argparse
import collections
import difflib
import re
from pathlib import Path

OC_RE = re.compile(
    r"CVR(?P<iface>[A-Za-z0-9]+)_\d+::(?P<method>[A-Za-z0-9_]+):\d+\s+-\s+Entered function"
)
XR_RE = re.compile(
    r"Entered\s+IVR(?P<iface>[A-Za-z0-9]+?)(?:_?\d+)?::(?P<method>[A-Za-z0-9_]+)"
)

MILESTONES = {
    "Compositor.WaitGetPoses",
    "Compositor.GetLastPoses",
    "Compositor.GetLastPoseForTrackedDeviceIndex",
    "Compositor.Submit",
    "Compositor.PostPresentHandoff",
    "Compositor.SetStageOverride_Async",
    "Compositor.ClearStageOverride",
    "Compositor.SuspendRendering",
    "Compositor.FadeGrid",
    "Compositor.FadeToColor",
    "Compositor.SetSkyboxOverride",
    "Compositor.ClearSkyboxOverride",
    "Compositor.IsCurrentSceneFocusAppLoading",
    "Compositor.CanRenderScene",
    "Compositor.GetFrameTiming",
    "System.PollNextEvent",
    "System.PollNextEventWithPose",
    "Input.UpdateActionState",
    "Input.GetDigitalActionData",
    "Input.GetAnalogActionData",
    "Input.GetPoseActionData",
    "Input.GetPoseActionDataForNextFrame",
    "Input.GetSkeletalActionData",
    "Input.GetSkeletalBoneData",
}


def normalise(path: Path) -> list[str]:
    out: list[str] = []
    for line in path.read_text(errors="replace").splitlines():
        m = OC_RE.search(line) or XR_RE.search(line)
        if not m:
            continue
        out.append(f"{m.group('iface')}.{m.group('method')}")
    return out


def rle(items: list[str]) -> list[tuple[str, int]]:
    if not items:
        return []
    result: list[tuple[str, int]] = []
    prev = items[0]
    count = 1
    for item in items[1:]:
        if item == prev:
            count += 1
        else:
            result.append((prev, count))
            prev, count = item, 1
    result.append((prev, count))
    return result


def print_stream(label: str, items: list[str], limit: int) -> None:
    print(f"\n{label}: {len(items)} milestone calls")
    for item, count in rle(items)[:limit]:
        suffix = f" x{count}" if count > 1 else ""
        print(f"  {item}{suffix}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("opencomposite", type=Path)
    ap.add_argument("xrizer", type=Path)
    ap.add_argument("--limit", type=int, default=120)
    ap.add_argument("--context", type=int, default=12)
    args = ap.parse_args()

    oc_all = normalise(args.opencomposite)
    xr_all = normalise(args.xrizer)
    oc = [x for x in oc_all if x in MILESTONES]
    xr = [x for x in xr_all if x in MILESTONES]

    print(f"OpenComposite parsed calls: {len(oc_all)}")
    print(f"xrizer parsed calls:       {len(xr_all)}")

    print_stream("OpenComposite milestones", oc, args.limit)
    print_stream("xrizer milestones", xr, args.limit)

    print("\nMilestone call counts:")
    keys = sorted(set(oc) | set(xr))
    oc_counts = collections.Counter(oc)
    xr_counts = collections.Counter(xr)
    for key in keys:
        if oc_counts[key] != xr_counts[key]:
            print(f"  {key:52s} OC={oc_counts[key]:7d} XR={xr_counts[key]:7d}")

    matcher = difflib.SequenceMatcher(a=oc, b=xr, autojunk=False)
    first = next((op for op in matcher.get_opcodes() if op[0] != "equal"), None)
    if first is None:
        print("\nNo milestone-sequence divergence found.")
        return 0

    tag, i1, i2, j1, j2 = first
    c = args.context
    print(f"\nFirst milestone divergence: {tag}")
    print(f"  OpenComposite [{i1}:{i2}] vs xrizer [{j1}:{j2}]")
    print("\n  OpenComposite context:")
    for idx in range(max(0, i1 - c), min(len(oc), i2 + c)):
        mark = ">" if i1 <= idx < i2 else " "
        print(f"  {mark} {idx:6d} {oc[idx]}")
    print("\n  xrizer context:")
    for idx in range(max(0, j1 - c), min(len(xr), j2 + c)):
        mark = ">" if j1 <= idx < j2 else " "
        print(f"  {mark} {idx:6d} {xr[idx]}")

    print("\nFor xrizer semantic state, also grep:")
    print("  grep -E '\\[alyx-comp\\]|\\[alyx-event\\]|SuspendRendering|stage override' alyx-launch.log")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

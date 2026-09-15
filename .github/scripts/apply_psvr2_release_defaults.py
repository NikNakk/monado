#!/usr/bin/env python3
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {".c", ".h", ".m", ".mm", ".cc", ".cpp"}
FEATURE = "XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS"
FEATURE_GUARD = f"defined(XRT_OS_OSX) && defined({FEATURE})"

DEFAULTS = {
    "XRT_MACOS_ASYNC_PRESENT": "true",
    "XRT_MACOS_CLIENT_FRAME_DIVISOR": "0",
    "XRT_MACOS_CLIENT_FRAME_MIN_HOLD": "0",
    "XRT_MACOS_COMPOSITOR_QOS": "true",
    "XRT_MACOS_DEFER_GPU_TIMESTAMPS": "true",
    "XRT_MACOS_DRAWABLE_SLOT": "true",
    "XRT_MACOS_EARLY_DRAWABLE": "false",
    "XRT_MACOS_LATE_RENDER_DESIRED_OFFSET_US": "2000",
    "XRT_MACOS_MAX_DRAWABLES": "3",
    "XRT_MACOS_METAL_SHARED_EVENT_WAIT": "true",
    "XRT_MACOS_PRESENT_IMMEDIATE": "false",
    "XRT_MACOS_PRESENT_MIN_DURATION_US": "8000",
    "XRT_MACOS_PRESENT_MIN_LEAD_US": "2000",
    "XRT_MACOS_PRESENT_PRELATCH_US": "2000",
    "XRT_MACOS_PRESENT_STALE_SUBSTITUTE": "true",
    "XRT_MACOS_PRESENT_WORKER": "false",
    "XRT_MACOS_SKIP_BLOCKING_GPU_TIMESTAMPS": "false",
    "PSVR2_ACCELERATION_HORIZON_MS": "90.0f",
    "PSVR2_ACCELERATION_PREDICTION": "true",
    "PSVR2_CONTINUITY_LIMIT_MM": "7.5f",
    "PSVR2_CONTINUITY_PREDICTION": "true",
    "PSVR2_CONTINUITY_TAU_MS": "4.0f",
    "PSVR2_FILTERED_LINEAR_PREDICTION": "false",
    "PSVR2_FULL_LINEAR_HORIZON": "true",
}


def source_files():
    return [
        p
        for p in (ROOT / "src" / "xrt").rglob("*")
        if p.is_file() and p.suffix in SOURCE_SUFFIXES
    ]


def set_debug_option_defaults():
    files = source_files()
    missing = []
    print("Requested runtime defaults:")
    for env_name, new_default in DEFAULTS.items():
        pattern = re.compile(
            r'(DEBUG_GET_ONCE_(?:BOOL|NUM|FLOAT)_OPTION\(\s*[^,\n]+,\s*"'
            + re.escape(env_name)
            + r'"\s*,\s*)([^)\n]+)(\))'
        )
        matches = []
        for path in files:
            text = path.read_text()
            matches.extend((path, m.group(2).strip()) for m in pattern.finditer(text))
        if not matches:
            missing.append(env_name)
            print(f"  MISSING macro: {env_name}")
            continue
        if len(matches) != 1:
            raise RuntimeError(
                f"{env_name} matched {len(matches)} option declarations: {matches}"
            )
        path, old_default = matches[0]
        text = path.read_text()
        updated, count = pattern.subn(
            lambda m: m.group(1) + new_default + m.group(3), text
        )
        assert count == 1
        path.write_text(updated)
        print(f"  {env_name}: {old_default} -> {new_default} ({path.relative_to(ROOT)})")

    print("\nXPC diagnostic labels:")
    for token in ("XPC_FLAGS", "XPC_SERVICE_NAME"):
        hits = []
        for path in ROOT.rglob("*"):
            if not path.is_file() or ".git" in path.parts:
                continue
            try:
                lines = path.read_text().splitlines()
            except (UnicodeDecodeError, OSError):
                continue
            for lineno, line in enumerate(lines, 1):
                if token in line:
                    hits.append(f"{path.relative_to(ROOT)}:{lineno}: {line.strip()}")
        if hits:
            print(f"  {token}:")
            for hit in hits[:20]:
                print(f"    {hit}")
        else:
            print(
                f"  {token}: no repository source occurrence "
                "(diagnostic output, not a runtime option)"
            )

    if not missing:
        return

    print("\nExact-string context for requested defaults without DEBUG_GET_ONCE declarations:")
    for env_name in missing:
        print(f"-- {env_name} --")
        found = False
        for path in ROOT.rglob("*"):
            if not path.is_file() or ".git" in path.parts:
                continue
            try:
                lines = path.read_text().splitlines()
            except (UnicodeDecodeError, OSError):
                continue
            for lineno, line in enumerate(lines, 1):
                if env_name not in line:
                    continue
                found = True
                lo = max(0, lineno - 3)
                hi = min(len(lines), lineno + 2)
                for n in range(lo, hi):
                    print(f"{path.relative_to(ROOT)}:{n + 1}: {lines[n]}")
        if not found:
            print("  no exact source occurrence")
    raise RuntimeError("Refusing to commit a partial defaults migration")


def guard_osx_blocks(text, markers):
    lines = text.splitlines(keepends=True)
    starts = []
    i = 0
    while i < len(lines):
        if lines[i].strip() == "#ifdef XRT_OS_OSX":
            depth = 1
            j = i + 1
            while j < len(lines):
                stripped = lines[j].lstrip()
                if stripped.startswith("#if"):
                    depth += 1
                elif stripped.startswith("#endif"):
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            if depth != 0:
                raise RuntimeError("Unbalanced XRT_OS_OSX block")
            body = "".join(lines[i + 1 : j])
            if any(marker in body for marker in markers):
                starts.append(i)
            i = j
        i += 1
    for idx in starts:
        lines[idx] = f"#if {FEATURE_GUARD}\n"
    return "".join(lines), len(starts)


def patch_psvr2():
    path = ROOT / "src/xrt/drivers/psvr2/psvr2.c"
    text = path.read_text()

    fallbacks = {
        "psvr2_prediction_parameter(debug_get_float_option_psvr2_continuity_limit_mm(), 5.0f, 0.0f, 20.0f)":
            "psvr2_prediction_parameter(debug_get_float_option_psvr2_continuity_limit_mm(), 7.5f, 0.0f, 20.0f)",
        "psvr2_prediction_parameter(debug_get_float_option_psvr2_acceleration_horizon_ms(), 80.0f, 0.0f, 120.0f)":
            "psvr2_prediction_parameter(debug_get_float_option_psvr2_acceleration_horizon_ms(), 90.0f, 0.0f, 120.0f)",
    }
    for old, new in fallbacks.items():
        if old not in text:
            raise RuntimeError(f"Expected PSVR2 fallback not found: {old}")
        text = text.replace(old, new, 1)

    timing_opts = (
        'DEBUG_GET_ONCE_BOOL_OPTION(psvr2_timing_trace, "PSVR2_TIMING_TRACE", false)\n'
        'DEBUG_GET_ONCE_BOOL_OPTION(psvr2_driver_timing_trace, "PSVR2_DRIVER_TIMING_TRACE", true)\n'
    )
    if timing_opts not in text:
        raise RuntimeError("Expected PSVR2 timing trace option declarations not found")
    text = text.replace(
        timing_opts,
        f"#if {FEATURE_GUARD}\n" + timing_opts + "#endif\n",
        1,
    )

    text, guarded = guard_osx_blocks(
        text, ("psvr2_timing_trace_", "trace_latest_slam_vts_ns")
    )
    if guarded < 6:
        raise RuntimeError(
            f"Expected at least 6 PSVR2 diagnostic OSX blocks, guarded {guarded}"
        )
    path.write_text(text)
    print(f"Guarded {guarded} PSVR2 high-frequency diagnostic blocks")


def patch_renderer():
    path = ROOT / "src/xrt/compositor/main/comp_renderer.c"
    text = path.read_text()

    for call in (
        "renderer_late_render_trace_open(r);",
        "renderer_late_render_trace_close(r);",
        "renderer_late_render_trace_frame(r);",
    ):
        if text.count(call) != 1:
            raise RuntimeError(
                f"Expected exactly one renderer call site for {call}, found {text.count(call)}"
            )
        text = text.replace(
            call,
            f"#ifdef {FEATURE}\n\t{call}\n#endif",
            1,
        )

    diagnostic_wait_init = (
        "\tr->late_render_target_ns = 0;\n"
        "\tr->late_render_pose_begin_ns = 0;\n"
        "\tr->late_render_pose_end_ns = 0;\n"
        "\tr->late_render_wait_begin_ns = (int64_t)os_monotonic_get_ns();\n"
        "\tr->late_render_wait_end_ns = r->late_render_wait_begin_ns;\n"
    )
    if diagnostic_wait_init not in text:
        raise RuntimeError("Expected late-render diagnostic wait initialisation block not found")
    text = text.replace(
        diagnostic_wait_init,
        f"#ifdef {FEATURE}\n" + diagnostic_wait_init + "#endif\n",
        1,
    )

    target_store = "\tr->late_render_target_ns = target_ns;"
    if text.count(target_store) != 1:
        raise RuntimeError("Expected one late_render_target_ns assignment")
    text = text.replace(
        target_store,
        f"#ifdef {FEATURE}\n{target_store}\n#endif",
        1,
    )

    wait_end_store = "\t\t\tr->late_render_wait_end_ns = now_ns;"
    if text.count(wait_end_store) != 1:
        raise RuntimeError("Expected one late_render_wait_end_ns assignment")
    text = text.replace(
        wait_end_store,
        f"#ifdef {FEATURE}\n{wait_end_store}\n#endif",
        1,
    )

    text, guarded = guard_osx_blocks(
        text, ("late_render_pose_begin_ns =", "late_render_pose_end_ns =")
    )
    if guarded < 3:
        raise RuntimeError(
            f"Expected at least 3 renderer pose-timestamp blocks, guarded {guarded}"
        )
    path.write_text(text)
    print(f"Guarded {guarded} renderer diagnostic timestamp blocks")


def patch_multi_trace_wrapper():
    path = ROOT / "src/xrt/compositor/multi/comp_multi_system_macos_trace.h"
    text = path.read_text()
    pattern = re.compile(
        r"(#define multi_compositor_latch_frame_locked\(mc, when_ns, system_frame_id\)[^\n]*\\\n"
        r"\tmacos_trace_multi_compositor_latch_frame_locked\([^\n]+\)\n)"
    )
    match = pattern.search(text)
    if not match:
        raise RuntimeError("Expected multi-compositor trace latch macro not found")
    replacement = f"#ifdef {FEATURE}\n" + match.group(1) + "#endif\n"
    text = text[: match.start()] + replacement + text[match.end() :]
    path.write_text(text)


def patch_cmake_option():
    path = ROOT / "src/xrt/CMakeLists.txt"
    text = path.read_text()
    if FEATURE in text:
        return
    block = (
        "# High-frequency macOS/PSVR2 CSV timing diagnostics are useful during cadence\n"
        "# investigations, but they add work to IMU, pose, and compositor hot paths.\n"
        "option(\n"
        f"\t{FEATURE}\n"
        '\t"Compile high-frequency macOS/PSVR2 timing diagnostics"\n'
        "\tOFF\n"
        "\t)\n"
        f"if({FEATURE})\n"
        f"\tadd_compile_definitions({FEATURE})\n"
        "endif()\n\n"
    )
    marker = "# Order matters.\n"
    if marker not in text:
        raise RuntimeError("Could not find insertion point in src/xrt/CMakeLists.txt")
    path.write_text(text.replace(marker, block + marker, 1))


def patch_diagnostics_workflow():
    path = ROOT / ".github/workflows/macos-psvr2-timing-diagnostics.yml"
    text = path.read_text()
    cmake_flag = f"-D{FEATURE}=ON"
    if cmake_flag in text:
        return
    marker = "-DBUILD_TESTING=OFF\n"
    if text.count(marker) != 2:
        raise RuntimeError("Expected two timing-diagnostics CMake configure blocks")
    path.write_text(
        text.replace(marker, marker + f"          {cmake_flag}\n")
    )


def main():
    set_debug_option_defaults()
    patch_psvr2()
    patch_renderer()
    patch_multi_trace_wrapper()
    patch_cmake_option()
    patch_diagnostics_workflow()
    print(
        f"\nCompile-time gating added: {FEATURE}=OFF by default; "
        "the diagnostics workflow explicitly enables it."
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise

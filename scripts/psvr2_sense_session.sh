#!/usr/bin/env bash
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
#
# Record one PS Sense constellation session into a persistent, never-overwritten folder and score it.
#
#   scripts/psvr2_sense_session.sh NAME CALIBRATION.json [DURATION_S] [NOTE]
#
# Creates $PSVR2_DATASETS/sessions/<YYYYMMDD-HHMMSS>-NAME/ containing:
#   run.log      stderr of monado-cli psvr2-constellation
#   poses.csv    stdout (10 Hz pose + diagnostics rows)
#   capture/     stride-sampled camera frames (PSVR2_CONSTELLATION_CAPTURE_STRIDE, default 6; 0 disables)
#   env.txt      every PSVR2_/PSSENSE_/CONSTELLATION_/T_LED_ variable in effect
#   git.txt      commit, branch and dirty state of the monado checkout
#   calibration.json, note.txt, score.txt, score.json
# and appends checksums to $PSVR2_DATASETS/SHA256SUMS.
#
# Defaults can be overridden from the environment, e.g. PSSENSE_LED_BOOTSTRAP=0 for the old LED sync.
# Never record into /tmp: macOS purges it.

set -euo pipefail

if [ $# -lt 2 ]; then
	sed -n '4,20p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
fi

NAME="$1"
CALIBRATION="$2"
DURATION="${3:-30}"
NOTE="${4:-}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DATASETS="${PSVR2_DATASETS:-$HOME/Code/psvr2-datasets}"
CLI="${MONADO_CLI:-$REPO/build-sense/src/xrt/targets/cli/monado-cli}"
PYTHON="${PYTHON:-$REPO/.venv/bin/python}"
[ -x "$PYTHON" ] || PYTHON=python3

if [ ! -x "$CLI" ]; then
	echo "monado-cli not found at $CLI (set MONADO_CLI)" >&2
	exit 1
fi
if [ ! -f "$CALIBRATION" ]; then
	echo "calibration not found: $CALIBRATION" >&2
	exit 1
fi

SESSION="$DATASETS/sessions/$(date +%Y%m%d-%H%M%S)-$NAME"
if [ -e "$SESSION" ]; then
	echo "refusing to overwrite $SESSION" >&2
	exit 1
fi
mkdir -p "$SESSION"

export PSVR2_CAMERA_STREAMS="${PSVR2_CAMERA_STREAMS:-1}"
export PSVR2_CAMERA_MODE="${PSVR2_CAMERA_MODE:-4}"
export PSSENSE_TIMING_DIAG="${PSSENSE_TIMING_DIAG:-1}"
export PSSENSE_LED_BOOTSTRAP="${PSSENSE_LED_BOOTSTRAP:-1}"
export PSSENSE_FORCE_IR="${PSSENSE_FORCE_IR:-0}"
export PSSENSE_CLOCK_OFFSET_SNAP_US="${PSSENSE_CLOCK_OFFSET_SNAP_US:-250}"
STRIDE="${PSVR2_CONSTELLATION_CAPTURE_STRIDE:-6}"

cp "$CALIBRATION" "$SESSION/calibration.json"
[ -n "$NOTE" ] && printf '%s\n' "$NOTE" > "$SESSION/note.txt"
env | grep -E '^(PSVR2_|PSSENSE_|CONSTELLATION_|T_LED_)' | sort > "$SESSION/env.txt" || true
{
	echo "commit $(git -C "$REPO" rev-parse HEAD)"
	echo "branch $(git -C "$REPO" rev-parse --abbrev-ref HEAD)"
	echo "calibration_source $CALIBRATION"
	echo "cli $CLI"
	git -C "$REPO" status --short --untracked-files=no
} > "$SESSION/git.txt"

CAPTURE_ARGS=()
if [ "$STRIDE" != "0" ]; then
	export PSVR2_CONSTELLATION_CAPTURE_STRIDE="$STRIDE"
	CAPTURE_ARGS=("$SESSION/capture")
fi

echo "Recording $DURATION s into $SESSION" >&2
set +e
"$CLI" psvr2-constellation "$SESSION/calibration.json" "$DURATION" "${CAPTURE_ARGS[@]+"${CAPTURE_ARGS[@]}"}" \
	> "$SESSION/poses.csv" 2> >(tee "$SESSION/run.log" >&2)
STATUS=$?
set -e
wait || true
echo "monado-cli exit status $STATUS" | tee -a "$SESSION/git.txt" >&2

"$PYTHON" "$REPO/scripts/psvr2_sense_session_score.py" "$SESSION" --json "$SESSION/score.json" \
	| tee "$SESSION/score.txt" || echo "scoring failed" >&2

if command -v shasum > /dev/null; then
	SUM="shasum -a 256"
else
	SUM="sha256sum"
fi
(cd "$DATASETS" && for f in "sessions/$(basename "$SESSION")"/{run.log,poses.csv,calibration.json,score.json}; do
	[ -f "$f" ] && $SUM "$f"
done) >> "$DATASETS/SHA256SUMS"

echo "Session saved: $SESSION" >&2
exit "$STATUS"

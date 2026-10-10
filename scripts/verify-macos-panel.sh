#!/usr/bin/env bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# On-panel verification of the macOS arm (Leia panel CONNECTED, SRService +
# SREyeTracker running, a FACE IN FRONT OF THE PANEL for the tracking checks).
# Run after ./scripts/build-macos.sh.
#
#   ./scripts/verify-macos-panel.sh                # run everything
#   ./scripts/verify-macos-panel.sh --parse-only   # re-judge the logs already in
#                                                  # build-macos/_verify (no app run)
#
# Checks, each PASS/FAIL:
#   1. claims    — `displayxr-cli displays --claims`: leia-sr VERIFIED on the panel.
#   2. selftest  — `displayxr-cli selftest` passes with leia-sr active.
#   3. single    — cube_handle_metal_macos fully on the panel: SR weave, window on
#                  the panel, srWeaverSetPresentOrigin accepted.
#   4. tracking  — during the single run, at least one "weaver eyes" sample that
#                  is NOT the SR nominal (0,100,600) and is plausible (both eyes
#                  z in 300..1200 mm). FAIL = nobody was tracked: needs a face.
#   5. straddle  — window across the panel's left edge with its MAJORITY on the
#                  PANEL: two segments, the Leia session DP weaves the panel's.
#   6. per-screen — window across the panel's left edge with its MAJORITY (and
#                  content origin) on the BUILT-IN display, so the panel is NOT
#                  the primary screen: the Leia segment DP must come from
#                  create_dp_metal_for_screen (bound to the SR display, EXTERNAL
#                  routing) and weave its segment; tracked eyes from that DP when
#                  it logs any.
# Every app it starts is killed by PID; it touches no other process.
#
# Environment:
#   PANEL_X, PANEL_Y   panel top-left in top-down points (default: parsed from
#                      the leia_mac enumeration line)
#   APP                cube_handle_metal_macos to run (default: built from the
#                      runtime tree build-macos.sh exported, into build-macos/_test_apps)
#   OPENXR_PREFIX      OpenXR SDK prefix for that build (default /tmp/openxr-install-1.1.63)
#   RUN_SECONDS        seconds per app run (default 12)
#   DXR_LEIA_MAC_EYE_LOG_MS  weaver-eyes log period during the runs (default 500 here)

set -uo pipefail

PARSE_ONLY=0
for arg in "$@"; do
    case "$arg" in
    --parse-only) PARSE_ONLY=1 ;;
    *) echo "Unknown option: $arg (supported: --parse-only)" >&2; exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$ROOT/build-macos"
OUT="${VERIFY_OUT:-$B/_verify}"
mkdir -p "$OUT"
FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

if [ "$PARSE_ONLY" = "0" ]; then
    [ -f "$B/env.sh" ] || { echo "run ./scripts/build-macos.sh first" >&2; exit 1; }
    # shellcheck disable=SC1091
    . "$B/env.sh"
    export XRT_LOG=info
    export DXR_LEIA_MAC_EYE_LOG_MS="${DXR_LEIA_MAC_EYE_LOG_MS:-500}"
    CLI="$B/runtime-build/src/xrt/targets/cli/displayxr-cli"
    TEXTURES_DIR="${TEXTURES_DIR:-$(cd "$ROOT/.." && pwd)/displayxr-runtime/_package/DisplayXR-macOS/bin}"
    RUN_SECONDS="${RUN_SECONDS:-12}"
    if [ -z "${APP:-}" ]; then
        APP="$B/_test_apps/bin/cube_handle_metal_macos"
        if [ ! -x "$APP" ]; then
            echo "==> building cube_handle_metal_macos from $B/_runtime-src/test_apps"
            nice -n 19 cmake -S "$B/_runtime-src/test_apps" -B "$B/_test_apps" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
                -DCMAKE_PREFIX_PATH="${OPENXR_PREFIX:-/tmp/openxr-install-1.1.63}" >"$OUT/test_apps_cfg.log" 2>&1
            nice -n 19 ninja -j "${DXR_BUILD_JOBS:-4}" -C "$B/_test_apps" cube_handle_metal_macos \
                >"$OUT/test_apps_build.log" 2>&1
        fi
    fi
    [ -x "$APP" ] || { echo "no app at $APP" >&2; exit 1; }
    "$CLI" displays --claims >"$OUT/claims.log" 2>&1
    "$CLI" selftest >"$OUT/selftest.log" 2>&1
fi

# Panel position (top-down points) from the enumeration line.
if [ -z "${PANEL_X:-}" ]; then
    line="$(grep -m1 "SR display \[0\]" "$OUT/claims.log" 2>/dev/null || true)"
    PANEL_X="$(echo "$line" | sed -n "s/.*' (\(-\{0,1\}[0-9]*\),\(-\{0,1\}[0-9]*\) .*/\1/p")"
    PANEL_Y="$(echo "$line" | sed -n "s/.*' (\(-\{0,1\}[0-9]*\),\(-\{0,1\}[0-9]*\) .*/\2/p")"
fi
PANEL_X="${PANEL_X:-1512}"
PANEL_Y="${PANEL_Y:-0}"

run_app() { # $1 = name, $2 = DXR_TEST_WINDOW_RECT
    [ "$PARSE_ONLY" = "1" ] && return 0
    local log="$OUT/$1.log"
    rm -f "${TMPDIR%/}"/displayxr_segments.* /tmp/dxr_leia_woven.png /tmp/dxr_leia_woven_trigger
    (cd "$TEXTURES_DIR" && DXR_TEST_WINDOW_RECT="$2" exec nice -n 19 "$APP") >"$log" 2>&1 &
    local pid=$!
    sleep "$RUN_SECONDS"
    touch "${TMPDIR%/}/displayxr_segments_trigger" /tmp/dxr_leia_woven_trigger
    sleep 3
    kill "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    cp -f /tmp/dxr_leia_woven.png "$OUT/$1.leia_woven.png" 2>/dev/null
    for f in "${TMPDIR%/}"/displayxr_segments.*.png; do [ -f "$f" ] && cp -f "$f" "$OUT/$1.$(basename "$f")"; done
}

# Count "weaver eyes" samples in $1 (optionally only lines matching $2) that
# are tracked: not the SR nominal (0,100,600) for both eyes, both z in 300..1200 mm.
tracked_eye_samples() {
    grep "weaver eyes" "$1" 2>/dev/null | grep -- "${2:-weaver eyes}" |
        sed -n 's/.*L(\([-0-9. ]*\)) R(\([-0-9. ]*\)) mm.*/\1 \2/p' |
        awk '{ nominal = ($1==0 && $2==100 && $3==600 && $4==0 && $5==100 && $6==600);
               if (!nominal && $3>=300 && $3<=1200 && $6>=300 && $6<=1200) n++ }
             END { print n+0 }'
}
eye_samples() { grep "weaver eyes" "$1" 2>/dev/null | grep -c -- "${2:-weaver eyes}"; }

echo "panel top-left: ($PANEL_X, $PANEL_Y) pt   logs: $OUT$([ "$PARSE_ONLY" = 1 ] && echo '   (parse-only)')"

# 1. claims
if sed -n '/Per-display DP claims/,$p' "$OUT/claims.log" 2>/dev/null | grep -q "plug-in='leia-sr'  confidence=VERIFIED"; then
    pass "claims: leia-sr VERIFIED on the panel"
else
    fail "claims: no leia-sr VERIFIED claim (see $OUT/claims.log)"
fi

# 2. selftest
if grep -q "SELF-TEST PASSED" "$OUT/selftest.log" 2>/dev/null && grep -q "active_plugin — id='leia-sr'" "$OUT/selftest.log"; then
    pass "selftest with leia-sr active"
else
    fail "selftest with leia-sr active (see $OUT/selftest.log)"
fi

# 3 + 4. single: fully on the panel (the run also feeds the tracking check)
run_app single "$((PANEL_X + 300)),$((PANEL_Y + 200)),1100,700"
L="$OUT/single.log"
grep -q "first SR weave" "$L" && pass "single: SR weave" || fail "single: no SR weave (see $L)"
grep -q "window is on display .* (the Leia panel)" "$L" && pass "single: window on the panel" \
    || fail "single: window not on the panel"
grep -qE "srWeaverSetPresentOrigin\(.*-> SR_SUCCESS" "$L" && pass "single: present origin accepted" \
    || fail "single: present origin not accepted"
n_all="$(eye_samples "$L")"
n_trk="$(tracked_eye_samples "$L")"
if [ "$n_trk" -gt 0 ]; then
    pass "tracking: $n_trk of $n_all weaver-eye samples tracked (not nominal, z 300..1200 mm)"
else
    fail "tracking: 0 of $n_all weaver-eye samples tracked — needs a face in front of the panel"
fi
grep "weaver eyes" "$L" | tail -3 | sed 's/^.*leia_mac_dp/    leia_mac_dp/'

# 5. straddle, panel = primary: majority on the panel
run_app straddle "$((PANEL_X - 400)),$((PANEL_Y + 150)),800,500"
L="$OUT/straddle.log"
grep -q "\-> 2 segment(s)" "$L" && pass "straddle: two segments" || fail "straddle: not split (see $L)"
grep -q "first SR weave" "$L" && pass "straddle: the Leia session DP weaves the panel segment" \
    || fail "straddle: no Leia weave"

# 6. per-screen: majority + content origin on the built-in, so the panel is a
#    NON-primary segment served by create_dp_metal_for_screen.
run_app perscreen "$((PANEL_X - 600)),$((PANEL_Y + 150)),800,500"
L="$OUT/perscreen.log"
if [ -f "$L" ]; then
    grep -q "\-> 2 segment(s)" "$L" && pass "per-screen: two segments" || fail "per-screen: not split (see $L)"
    grep -q "plug-in 'leia-sr') via create_dp_metal_for_screen" "$L" \
        && pass "per-screen: runtime created the Leia segment DP via create_dp_metal_for_screen" \
        || fail "per-screen: no Leia segment DP from create_dp_metal_for_screen"
    if grep -q "segment weaver for SR display .*routing EXTERNAL, weaver BOUND" "$L"; then
        pass "per-screen: segment weaver bound to the SR display, EXTERNAL routing"
    else
        fail "per-screen: segment weaver not bound + EXTERNAL ($(grep -m1 'segment weaver for SR display' "$L" | sed 's/.*leia_mac_dp: //'))"
    fi
    grep -q "first SR weave (SEGMENT DP)" "$L" && pass "per-screen: the Leia segment DP weaves its segment" \
        || fail "per-screen: the Leia segment DP never wove"
    s_all="$(eye_samples "$L" "\[segment ")"
    s_trk="$(tracked_eye_samples "$L" "\[segment ")"
    if [ "$s_all" -eq 0 ]; then
        echo "NOTE: per-screen: the segment DP logged no eyes (the runtime may not query a non-primary DP's eyes)"
    elif [ "$s_trk" -gt 0 ]; then
        pass "per-screen: $s_trk of $s_all eye samples from the segment DP tracked"
    else
        fail "per-screen: 0 of $s_all segment-DP eye samples tracked — needs a face in front of the panel"
    fi
    grep -E "create_dp_metal_for_screen|segment weaver|segments: window" "$L" | head -5 | sed 's/^/    /'
else
    fail "per-screen: no log at $L (parse-only with no prior per-screen run)"
fi

echo ""
echo "Artifacts: $OUT"
[ "$FAILS" -eq 0 ] && echo "ALL PASS" || echo "$FAILS FAIL(s)"
exit "$FAILS"

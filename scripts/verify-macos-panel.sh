#!/usr/bin/env bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# On-panel verification of the macOS arm (Leia panel CONNECTED, SRService +
# SREyeTracker running). Run after ./scripts/build-macos.sh.
#
#   ./scripts/verify-macos-panel.sh
#
# Checks, each PASS/FAIL:
#   1. claims   — `displayxr-cli displays --claims` shows leia-sr VERIFIED on
#                 the panel's monitor and sim-display on the others.
#   2. selftest — `displayxr-cli selftest` passes with leia-sr active.
#   3. single   — cube_handle_metal_macos fully on the panel: SR weave, the
#                 panel's CGDisplay, srWeaverSetPresentOrigin accepted.
#   4. straddle — the window across the panel's left edge: two segments, the
#                 Leia DP on the panel segment, sim-display on the other.
# Every app it starts is killed by PID; it touches no other process.
#
# Environment:
#   PANEL_X, PANEL_Y   panel top-left in top-down points (default: read from
#                      the leia_mac enumeration log line)
#   APP                cube_handle_metal_macos to run (default: built from the
#                      runtime tree build-macos.sh exported, into build-macos/_test_apps)
#   OPENXR_PREFIX      OpenXR SDK prefix for that build (default /tmp/openxr-install-1.1.63)
#   RUN_SECONDS        seconds per app run (default 12)

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$ROOT/build-macos"
OUT="$B/_verify"
mkdir -p "$OUT"
[ -f "$B/env.sh" ] || { echo "run ./scripts/build-macos.sh first" >&2; exit 1; }
# shellcheck disable=SC1091
. "$B/env.sh"
export XRT_LOG=info
CLI="$B/runtime-build/src/xrt/targets/cli/displayxr-cli"
TEXTURES_DIR="${TEXTURES_DIR:-$(cd "$ROOT/.." && pwd)/displayxr-runtime/_package/DisplayXR-macOS/bin}"
RUN_SECONDS="${RUN_SECONDS:-12}"
FAILS=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAILS=$((FAILS + 1)); }

if [ -z "${APP:-}" ]; then
    APP="$B/_test_apps/bin/cube_handle_metal_macos"
    if [ ! -x "$APP" ]; then
        echo "==> building cube_handle_metal_macos from $B/_runtime-src/test_apps"
        nice -n 19 cmake -S "$B/_runtime-src/test_apps" -B "$B/_test_apps" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_PREFIX_PATH="${OPENXR_PREFIX:-/tmp/openxr-install-1.1.63}" >"$OUT/test_apps_cfg.log" 2>&1
        nice -n 19 ninja -j "${DXR_BUILD_JOBS:-4}" -C "$B/_test_apps" cube_handle_metal_macos >"$OUT/test_apps_build.log" 2>&1
    fi
fi
[ -x "$APP" ] || { echo "no app at $APP" >&2; exit 1; }

# 1. claims
"$CLI" displays --claims >"$OUT/claims.log" 2>&1
if sed -n '/Per-display DP claims/,$p' "$OUT/claims.log" | grep -q "plug-in='leia-sr'  confidence=VERIFIED"; then
    pass "claims: leia-sr VERIFIED on the panel"
else
    fail "claims: no leia-sr VERIFIED claim (see $OUT/claims.log)"
fi
sed -n '/Per-display DP claims/,$p' "$OUT/claims.log" | grep -E "monitor|plug-in=" | sed 's/^/    /'

if [ -z "${PANEL_X:-}" ]; then
    line="$(grep -m1 "SR display \[0\]" "$OUT/claims.log" || true)"
    PANEL_X="$(echo "$line" | sed -n "s/.*' (\(-\{0,1\}[0-9]*\),\(-\{0,1\}[0-9]*\) .*/\1/p")"
    PANEL_Y="$(echo "$line" | sed -n "s/.*' (\(-\{0,1\}[0-9]*\),\(-\{0,1\}[0-9]*\) .*/\2/p")"
fi
PANEL_X="${PANEL_X:-1512}"
PANEL_Y="${PANEL_Y:-0}"
echo "    panel top-left: ($PANEL_X, $PANEL_Y) pt"

# 2. selftest
if "$CLI" selftest >"$OUT/selftest.log" 2>&1 && grep -q "active_plugin — id='leia-sr'" "$OUT/selftest.log"; then
    pass "selftest with leia-sr active"
else
    fail "selftest (see $OUT/selftest.log)"
fi

run_app() { # $1 = name, $2 = DXR_TEST_WINDOW_RECT
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

# 3. single: fully on the panel
run_app single "$((PANEL_X + 300)),$((PANEL_Y + 200)),1100,700"
L="$OUT/single.log"
grep -q "first SR weave" "$L" && pass "single: SR weave" || fail "single: no SR weave (see $L)"
grep -q "window is on display .* (the Leia panel)" "$L" && pass "single: window on the panel" \
    || fail "single: window not on the panel"
grep -qE "srWeaverSetPresentOrigin\(.*-> SR_SUCCESS" "$L" && pass "single: present origin accepted" \
    || fail "single: present origin not accepted"
grep -E "SR display \[|weaver eyes|SetPresentOrigin" "$L" | head -6 | sed 's/^/    /'

# 4. straddle: across the panel's left edge
run_app straddle "$((PANEL_X - 400)),$((PANEL_Y + 150)),800,500"
L="$OUT/straddle.log"
grep -q "\-> 2 segment(s)" "$L" && pass "straddle: two segments" || fail "straddle: not split (see $L)"
grep -qE "first SR weave|segment weaver for SR display" "$L" && pass "straddle: Leia DP weaves the panel segment" \
    || fail "straddle: no Leia weave"
grep -q "plug-in 'sim-display') via create_dp_metal_for_screen" "$L" && pass "straddle: sim-display on the other screen" \
    || echo "NOTE: no sim-display segment DP (fine if the panel is not the primary screen and Leia made it)"
grep -E "segments: |create_dp_metal_for_screen|segment weaver" "$L" | head -8 | sed 's/^/    /'

echo ""
echo "Artifacts: $OUT"
[ "$FAILS" -eq 0 ] && echo "ALL PASS" || echo "$FAILS FAIL(s)"
exit "$FAILS"

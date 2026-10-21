#!/bin/bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# Dry-run test of the macOS .pkg's preinstall / postinstall and of the
# installed uninstall.sh, against a fake root with fake pkgutil / launchctl /
# stat / id on PATH. Installs nothing and touches nothing outside a temp dir,
# so it is safe on a dev Mac and runs on Linux CI (lint.yml) too.
#
#   ./installer/macos/test_pkg_scripts.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
FAILS=0
pass() { echo "  PASS  $*"; }
fail() { echo "  FAIL  $*"; FAILS=$((FAILS + 1)); }

# --- fakes -------------------------------------------------------------------
mkdir -p "$T/bin"
# pkgutil: --pkg-info prints the version in $FAKE_RT_VER (absent when empty);
# --forget logs to $T/forgot.
cat >"$T/bin/pkgutil" <<'EOF'
#!/bin/bash
for a in "$@"; do
    case "$a" in
    --pkg-info) [ -n "${FAKE_RT_VER:-}" ] || exit 1
        printf 'package-id: com.displayxr.runtime\nversion: %s\nvolume: /\n' "$FAKE_RT_VER"; exit 0 ;;
    --forget) echo "$*" >>"$FAKE_LOG"; exit 0 ;;
    esac
done
exit 1
EOF
cat >"$T/bin/stat" <<'EOF'
#!/bin/bash
echo "${FAKE_CONSOLE_USER:-alice}"
EOF
cat >"$T/bin/id" <<'EOF'
#!/bin/bash
echo 501
EOF
# launchctl: print reports running iff $FAKE_SVC_RUNNING=1; kickstart logs.
cat >"$T/bin/launchctl" <<'EOF'
#!/bin/bash
case "$1" in
print) [ "${FAKE_SVC_RUNNING:-0}" = "1" ] || exit 113; printf '\tstate = running\n' ;;
kickstart) echo "kickstart $*" >>"$FAKE_LOG" ;;
esac
exit 0
EOF
chmod +x "$T/bin/"*
export PATH="$T/bin:$PATH" FAKE_LOG="$T/log"

PRE="$T/preinstall"
sed 's/@MIN_RUNTIME_VERSION@/2.31.0/g' "$SCRIPT_DIR/scripts/preinstall" >"$PRE"
PRE_NOFLOOR="$T/preinstall_nofloor"
cp "$SCRIPT_DIR/scripts/preinstall" "$PRE_NOFLOOR" # placeholder left in = no floor
chmod +x "$PRE" "$PRE_NOFLOOR"

expect_rc() { # <want> <desc> <cmd...>
    local want="$1" desc="$2" rc=0
    shift 2
    "$@" >/dev/null 2>&1 || rc=$?
    if [ "$rc" = "$want" ]; then pass "$desc (rc=$rc)"; else fail "$desc (rc=$rc, want $want)"; fi
}

echo "preinstall (floor 2.31.0)"
FAKE_RT_VER="" expect_rc 0 "runtime absent installs" "$PRE" pkg / /
FAKE_RT_VER=2.31.0 expect_rc 0 "runtime == floor installs" "$PRE" pkg / /
FAKE_RT_VER=2.49.0 expect_rc 0 "runtime above floor installs" "$PRE" pkg / /
FAKE_RT_VER=2.100.0 expect_rc 0 "numeric (not lexical) compare: 2.100.0 > 2.31.0" "$PRE" pkg / /
FAKE_RT_VER=2.30.9 expect_rc 1 "runtime below floor refused" "$PRE" pkg / /
FAKE_RT_VER=2.4.1 expect_rc 1 "2.4.1 < 2.31.0 refused" "$PRE" pkg / /
FAKE_RT_VER=0.0.0-dev.abcdef12 expect_rc 0 "dev runtime version not gated" "$PRE" pkg / /
FAKE_RT_VER=1.0.0 expect_rc 0 "no floor baked in: not gated" "$PRE_NOFLOOR" pkg / /

echo "postinstall"
: >"$FAKE_LOG"
FAKE_SVC_RUNNING=1 expect_rc 0 "service running" "$SCRIPT_DIR/scripts/postinstall" pkg / /
grep -q "kickstart -k gui/501/com.displayxr.service" "$FAKE_LOG" && pass "running service restarted" || fail "running service not restarted"
: >"$FAKE_LOG"
FAKE_SVC_RUNNING=0 expect_rc 0 "service not running" "$SCRIPT_DIR/scripts/postinstall" pkg / /
[ ! -s "$FAKE_LOG" ] && pass "stopped service NOT started" || fail "stopped service was started"
: >"$FAKE_LOG"
FAKE_SVC_RUNNING=1 expect_rc 0 "non-root target volume" "$SCRIPT_DIR/scripts/postinstall" pkg / /Volumes/Other
[ ! -s "$FAKE_LOG" ] && pass "no restart for another volume" || fail "restarted for another volume"

echo "uninstall.sh"
R="$T/root"
P="$R/Library/Application Support/DisplayXR"
mkdir -p "$P/lib/displayxr/plugins" "$P/DisplayProcessors" "$P/LeiaSR"
touch "$P/lib/displayxr/plugins/DisplayXR-LeiaSR.dylib" "$P/DisplayProcessors/050-leia-sr.json" \
    "$P/lib/displayxr/plugins/DisplayXR-SimDisplay.dylib" "$P/DisplayProcessors/200-sim-display.json" \
    "$P/lib/openxr_displayxr.dylib"
cp "$SCRIPT_DIR/uninstall.sh" "$P/LeiaSR/uninstall.sh"
: >"$FAKE_LOG"
FAKE_SVC_RUNNING=1 DXR_UNINSTALL_ROOT="$R" expect_rc 0 "uninstall runs" bash "$P/LeiaSR/uninstall.sh"
[ ! -e "$P/lib/displayxr/plugins/DisplayXR-LeiaSR.dylib" ] && [ ! -e "$P/DisplayProcessors/050-leia-sr.json" ] &&
    [ ! -e "$P/LeiaSR" ] && pass "plug-in files + uninstaller removed" || fail "plug-in files left behind"
[ -e "$P/lib/displayxr/plugins/DisplayXR-SimDisplay.dylib" ] && [ -e "$P/DisplayProcessors/200-sim-display.json" ] &&
    [ -e "$P/lib/openxr_displayxr.dylib" ] && pass "runtime + sim-display untouched" || fail "touched runtime files"
grep -q "forget com.displayxr.leia-sr-plugin" "$FAKE_LOG" && pass "receipt forgotten" || fail "receipt not forgotten"
grep -q "kickstart" "$FAKE_LOG" && pass "running service restarted" || fail "running service not restarted"

if [ "$FAILS" -ne 0 ]; then
    echo "$FAILS failure(s)"
    exit 1
fi
echo "all pkg script tests passed"

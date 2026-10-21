#!/bin/bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# Uninstall the DisplayXR Leia SR plug-in (macOS). Installed by the .pkg as
#   /Library/Application Support/DisplayXR/LeiaSR/uninstall.sh
# Run it with:
#   sudo "/Library/Application Support/DisplayXR/LeiaSR/uninstall.sh"
#
# Works with or without the DisplayXR runtime installed, and never touches the
# runtime, the sim-display fallback or the LeiaSR platform. Order mirrors the
# Windows uninstaller: the manifest goes first, so a service that restarts
# finds no plug-in and runs on the fallback display processor.
#
# DXR_UNINSTALL_ROOT (tests only, installer/macos/test_pkg_scripts.sh) prefixes
# every path and skips the root check; stat / id / launchctl / pkgutil come
# from PATH.
set -u

ROOT="${DXR_UNINSTALL_ROOT:-}"
PREFIX="$ROOT/Library/Application Support/DisplayXR"
PKG_ID="com.displayxr.leia-sr-plugin"
AGENT_LABEL="com.displayxr.service"

if [ -z "$ROOT" ] && [ "$(id -u)" != "0" ]; then
    exec sudo "$0" "$@"
fi

echo "=== DisplayXR Leia SR plug-in uninstaller ==="

rm -f "$PREFIX/DisplayProcessors/050-leia-sr.json"
rm -f "$PREFIX/lib/displayxr/plugins/DisplayXR-LeiaSR.dylib"
echo "Removed the leia-sr manifest and DisplayXR-LeiaSR.dylib."

# A running service still has the plug-in mapped: restart it (only if it was
# running) so it comes back on the fallback display processor.
console_user="$(stat -f%Su /dev/console 2>/dev/null || true)"
if [ -n "$console_user" ] && [ "$console_user" != "root" ]; then
    console_uid="$(id -u "$console_user" 2>/dev/null || true)"
    if [ -n "$console_uid" ] &&
        launchctl print "gui/$console_uid/$AGENT_LABEL" 2>/dev/null | grep -q 'state = running'; then
        echo "Restarting the DisplayXR service without the plug-in..."
        launchctl kickstart -k "gui/$console_uid/$AGENT_LABEL" 2>/dev/null || true
    fi
fi

if [ -z "$ROOT" ]; then
    pkgutil --forget "$PKG_ID" >/dev/null 2>&1 || true
else
    pkgutil --volume "$ROOT" --forget "$PKG_ID" >/dev/null 2>&1 || true
fi

# Last: this script's own directory.
rm -rf "$PREFIX/LeiaSR"
echo "=== DisplayXR Leia SR plug-in uninstalled ==="

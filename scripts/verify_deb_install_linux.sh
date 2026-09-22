#!/usr/bin/env bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# Install-verify the plug-in .deb on a CLEAN Ubuntu release. Run it as root
# inside a pristine ubuntu:<release> container — CI's DebInstall matrix does
# this for 22.04, 24.04 and 26.04; locally:
#
#   docker run --rm -v "$PWD:/w" -w /w ubuntu:22.04 \
#     ./scripts/verify_deb_install_linux.sh dist/displayxr-leia-sr_*_amd64.deb \
#                                           runtime-deb/displayxr-runtime_*_amd64.deb
#
# The runtime .deb is passed alongside because the plug-in hard-Depends on it;
# apt installs both from the same command, so an unsatisfiable dependency in
# either one fails here.
#
# Fails if:
#   * apt cannot resolve the package's Depends from that release's archive
#     (installed with --no-install-recommends: Depends alone must be enough —
#     a name that exists on only one release, such as libpipewire-0.3-0t64 on
#     jammy, or an unsatisfiable floor such as `libc6 (>= 2.38)` on 22.04,
#     fails right here);
#   * a Depends / Recommends / Suggests name does not exist on that release
#     (see the vendor-package exemption below);
#   * `ldd -r` on the installed plug-in reports a missing library, symbol or
#     symbol version (what a glibc / libstdc++ floor above the release looks
#     like once the unversioned-Depends hole is closed);
#   * the discovery manifest is missing or does not point at the installed .so;
#   * `displayxr-cli selftest` fails with NO DisplayXR env vars set. With the
#     plug-in installed but no vendor SR runtime present, the plug-in's probe
#     must DECLINE and sim-display must claim — the graceful-fallback contract.
#     A green selftest here proves the runtime still drives apps with the
#     plug-in installed and that the fallback is intact; `ldd -r` above is what
#     proves every soname resolves. (The runtime .deb used is the latest
#     RELEASED one, which may be newer than this branch's pinned RUNTIME_REF;
#     an ABI-mismatched plug-in is rejected by the loader and sim-display
#     claims, which is the same observable outcome as a clean decline. This
#     leg is a PACKAGING check, not an ABI check — the Build job's selftest
#     legs cover ABI.) Headless: no GPU, window or display needed.
set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: $0 <displayxr-leia-sr.deb> [displayxr-runtime.deb]" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "error: run as root in a throwaway container." >&2; exit 2; }

DEBS=()
for d in "$@"; do DEBS+=("$(readlink -f "$d")"); done
. /etc/os-release
echo "==> $PRETTY_NAME: installing ${DEBS[*]##*/}"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends "${DEBS[@]}"

PKG=displayxr-leia-sr
dpkg-query -W -f='==> installed ${Package} ${Version}\n    Depends: ${Depends}\n    Recommends: ${Recommends}\n' "$PKG"

fail=0

# Every Depends / Recommends / Suggests alternative must name a real package on
# this release (apt already proved Depends; this also covers the optional
# fields, where a wrong name degrades silently instead of failing the install).
#
# TWO DELIBERATE EXEMPTIONS, both packages that are correct to name and are not
# in the Ubuntu archive:
#   * the vendor SR runtime — shipped by the vendor's own installer, never by
#     Ubuntu. It is a Recommends precisely so the plug-in installs without it
#     and declines its probe.
#   * displayxr-window-geometry-publisher — a VIRTUAL package the runtime .deb
#     Provides. apt-cache show does not list a pure virtual, so resolve it
#     through the Provides of what is installed instead.
SR_RUNTIME_PKG="${SR_RUNTIME_PKG:-leiasr-runtime}"
# Captured once (and not piped into grep -q, which SIGPIPEs under pipefail).
ALL_PROVIDES="$(dpkg-query -W -f='${Provides}\n' 2>/dev/null | tr ',' '\n' | sed 's/ //g; s/(.*)//' | sed '/^$/d')"
for field in Depends Recommends Suggests; do
    for p in $(dpkg-query -W -f="\${$field}" "$PKG" | tr ',|' '\n\n' | sed 's/(.*)//; s/:any//; s/ //g' | sed '/^$/d'); do
        if [ "$p" = "$SR_RUNTIME_PKG" ]; then
            echo "    $field $p: vendor package, not in the Ubuntu archive (expected)"
        elif apt-cache show "$p" >/dev/null 2>&1; then
            echo "    $field $p: available"
        elif grep -qx "$p" <<<"$ALL_PROVIDES"; then
            echo "    $field $p: satisfied by an installed package's Provides (virtual)"
        else
            echo "error: $field '$p' does not exist on $PRETTY_NAME." >&2
            fail=1
        fi
    done
done

SO=/usr/lib/displayxr/plugins/DisplayXR-LeiaSR.so
MANIFEST=/usr/lib/displayxr/plugins/050-leia-sr.json
[ -f "$SO" ] || { echo "error: $SO not installed." >&2; exit 1; }
[ -f "$MANIFEST" ] || { echo "error: $MANIFEST not installed." >&2; fail=1; }
grep -q "$SO" "$MANIFEST" || { echo "error: $MANIFEST does not point at $SO." >&2; fail=1; }

out="$(ldd -r "$SO" 2>&1)" || true
echo "$out" | sed 's/^/    /'
if grep -E 'not found|undefined symbol' <<<"$out"; then
    echo "error: unresolved dependency in $SO on $PRETTY_NAME." >&2
    fail=1
else
    echo "    ldd -r $SO: ok"
fi

# The desktop-capture libraries are a shipping feature, so they must resolve on
# every release — not just be named in Depends.
for lib in libpipewire-0.3.so.0 libdbus-1.so.3; do
    grep -q "$lib" <<<"$out" || { echo "error: $lib not resolved on $PRETTY_NAME." >&2; fail=1; }
done

if command -v displayxr-cli >/dev/null 2>&1; then
    echo "=== displayxr-cli selftest (env-free; plug-in must decline, sim-display claims) ==="
    unset XR_RUNTIME_JSON XRT_PLUGIN_SEARCH_PATH DXR_LEIA_FORCE_PROBE
    # Captured, not piped: `cmd | grep -q` under `set -o pipefail` fails on the
    # SIGPIPE grep sends after its first match, even though the match succeeded.
    info_out="$(displayxr-cli info 2>&1)" || fail=1
    echo "$info_out"
    displayxr-cli selftest || { echo "error: displayxr-cli selftest failed on $PRETTY_NAME." >&2; fail=1; }
    if grep -qE "active plug-in: id=sim-display\b" <<<"$info_out"; then
        echo "    active plug-in is sim-display: the plug-in declined, as it must without the vendor SR runtime"
    else
        echo "error: expected sim-display to claim (no vendor SR runtime here)." >&2
        fail=1
    fi
else
    echo "error: displayxr-cli not installed — pass the runtime .deb as the second argument." >&2
    fail=1
fi

[ "$fail" = 0 ] || { echo "==> FAIL on $PRETTY_NAME" >&2; exit 1; }
echo "==> PASS on $PRETTY_NAME"

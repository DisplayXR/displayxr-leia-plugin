#!/usr/bin/env bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# Package the Leia SR display-processor plug-in as a Debian package (.deb) —
# Phase 2 of DisplayXR-runtime #781 (Linux packaged installers). Companion to
# the runtime .deb: the runtime .deb ships sim-display as the built-in fallback;
# THIS .deb drops the Leia plug-in into the same discovery dir with a lower
# probe_order (50), so on a box with the SR stack the Leia DP claims the display
# automatically — no env vars, no force-probe.
#
#   SRSDK_ROOT=/path/to/leiasr-sdk ./scripts/package_deb_leia.sh
#   ./scripts/package_deb_leia.sh --stub          # mechanics test, no SR SDK
#   ./scripts/package_deb_leia.sh --no-build       # package an existing build/
#
# Output: dist/displayxr-leia-sr_<ver>_<arch>.deb
#
# Payload (installed layout):
#   /usr/lib/displayxr/plugins/DisplayXR-LeiaSR.so     (the Leia DP plug-in)
#   /usr/lib/displayxr/plugins/050-leia-sr.json        (discovery manifest, probe_order 50)
#
# NO postinst/postrm: the plug-in .deb just drops files into the shared
# DisplayProcessors discovery root (`/usr/lib/displayxr/plugins`, which the
# runtime searches by default — runtime #781 Phase 1). The runtime .deb owns
# the OpenXR ActiveRuntime registration; the SR runtime package owns
# `/etc/leia/sr/1/active_runtime.json`. This package only contributes a DP.
#
# --- The release deployment model (NOT the #781 sketch's "bake rpath") -------
# The srSDK loader linked into the plug-in (static `srSDK::loader`) dlopens
# `libLeiaSR_runtime.so` with search order:
#   /etc/leia/sr/1/active_runtime.json  →  $SR_RUNTIME_PATH  →  plain dlopen.
# The FIRST is registered by Leia's own SR runtime Linux installer (Windows
# parity). So a shipped plug-in must resolve the SR runtime through THAT
# registration — never a build-machine path baked into the ELF. This script
# therefore builds with the dev rpath OFF and, belt-and-suspenders, strips any
# DT_RUNPATH from the .so with patchelf (so it produces a correct release
# artifact even on `main`, before the linux-sdk-rpath-dev-only branch lands the
# -DDXR_LEIA_SDK_DEV_RPATH=OFF option). See docs/leia-linux-sdk-contract.md §7
# and docs/linux-track-b-runbook.md.
#
# The SR runtime package is a `Recommends:` (not `Depends:`): without it the
# plug-in still installs and simply DECLINES its probe (SR runtime absent) so
# sim-display claims — the intended graceful fallback. `apt install` pulls the
# SR runtime by default when the package is available.
#
# ==> Build requires the commercial SR SDK (SRSDK_ROOT) for Track B, which is
#     never on a generic box. Run this on an SR-equipped Linux box (or a
#     container with the SDK unpacked). `--stub` builds the Track A passthrough
#     weaver instead (no SDK) to validate the packaging MECHANICS only — the
#     resulting .deb does no real weaving and still needs DXR_LEIA_FORCE_PROBE.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-deb}"
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
RUNTIME_DIR="${DXR_RUNTIME_SOURCE_DIR:-$(cd "$ROOT/.." && pwd)/displayxr-runtime}"

# The SR runtime Debian package name for `Recommends:`. CONFIRMED against the
# LeiaSR repo (packaging/linux/deb/control.in on the ST-5525-linux-support
# branch: `Package: leiasr-runtime`; installs under /opt/leiasr). Override with
# SR_RUNTIME_PKG=<name> if the release package name changes.
SR_RUNTIME_PKG="${SR_RUNTIME_PKG:-leiasr-runtime}"

# INTEGRATION GAP (as of the ST-5525-linux-support branch): the leiasr-runtime
# .deb bundles libLeiaSR_runtime.so under /opt/leiasr/lib but does NOT register
# /etc/leia/sr/1/active_runtime.json, add an ld.so.conf.d entry for
# /opt/leiasr/lib, or ldconfig it. So the srSDK loader's default resolution
# (/etc/leia/sr/1/active_runtime.json -> $SR_RUNTIME_PATH -> plain dlopen) finds
# nothing as-is. Until the SR .deb registers that path (the correct owner), a
# deployed plug-in needs one of: SR_RUNTIME_PATH=/opt/leiasr/lib/libLeiaSR_runtime.so
# (env, re-introduces config), or a baked rpath to the STABLE install dir
# /opt/leiasr/lib (a fixed deployment path, not a build-machine path — distinct
# from the dev-rpath the release model rejects). Tracked as an SR-side ask; do
# NOT paper over it here without George's call.

WEAVER="sdk"       # Track B (real srSDK). --stub switches to Track A.
NO_BUILD=0
for arg in "$@"; do
    case "$arg" in
    --stub) WEAVER="stub" ;;
    --no-build) NO_BUILD=1 ;;
    *) echo "Unknown option: $arg (supported: --stub --no-build)" >&2; exit 2 ;;
    esac
done

command -v dpkg-deb >/dev/null 2>&1 || { echo "error: dpkg-deb not found — run on a Debian/Ubuntu host/container." >&2; exit 1; }
command -v patchelf >/dev/null 2>&1 || { echo "error: patchelf not found (apt install patchelf) — needed to strip the dev rpath." >&2; exit 1; }

if [ "$WEAVER" = "sdk" ] && [ -z "${SRSDK_ROOT:-}" ]; then
    echo "error: Track B (sdk weaver) needs SRSDK_ROOT pointing at a local SR SDK unpack." >&2
    echo "       Set SRSDK_ROOT=/path/to/leiasr-sdk, or pass --stub for a mechanics-only build." >&2
    exit 1
fi
[ -f "$RUNTIME_DIR/CMakeLists.txt" ] || { echo "error: runtime checkout not found at $RUNTIME_DIR (set DXR_RUNTIME_SOURCE_DIR)." >&2; exit 1; }

find_so() { find "$BUILD_DIR" -name "DisplayXR-LeiaSR.so" -type f 2>/dev/null | head -1; }

if [ -z "$(find_so)" ]; then
    [ "$NO_BUILD" = 0 ] || { echo "error: no build under $BUILD_DIR and --no-build set." >&2; exit 1; }
    echo "==> Configuring release (weaver=$WEAVER, runtime: $RUNTIME_DIR)"
    # DXR_LEIA_SDK_DEV_RPATH=OFF is a no-op on `main` (the option lands with the
    # linux-sdk-rpath-dev-only branch); the patchelf strip below covers both.
    cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DDXR_RUNTIME_SOURCE_DIR="$RUNTIME_DIR" \
        -DDXR_LEIA_LINUX_WEAVER="$WEAVER" \
        -DDXR_LEIA_SDK_DEV_RPATH=OFF
    echo "==> Building DisplayXR-LeiaSR.so"
    cmake --build "$BUILD_DIR" --target DisplayXR-LeiaSR
fi

SO="$(find_so)"
[ -n "$SO" ] || { echo "error: DisplayXR-LeiaSR.so not found after build." >&2; exit 1; }

# Version: git describe → Debian-legal upstream version (same rule as the runtime).
RAW="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo 0.0.0)"
VERSION="$(echo "$RAW" | sed -e 's/^v//' -e 's/-dirty$/+dirty/' -e 's/-\([0-9]\+\)-g/+\1.g/')"
case "$VERSION" in [0-9]*) : ;; *) VERSION="0.0.0+g$VERSION" ;; esac
ARCH="$(dpkg --print-architecture)"
PKG="displayxr-leia-sr"

STAGE="$DIST_DIR/${PKG}_${VERSION}_${ARCH}"
echo "==> Staging $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" "$STAGE/usr/lib/displayxr/plugins"

install -m 0644 "$SO" "$STAGE/usr/lib/displayxr/plugins/DisplayXR-LeiaSR.so"

# Strip any DT_RUNPATH/DT_RPATH so no build-machine SDK path ships (release
# contract: resolve libLeiaSR_runtime.so via /etc/leia/sr/1/active_runtime.json).
patchelf --remove-rpath "$STAGE/usr/lib/displayxr/plugins/DisplayXR-LeiaSR.so"
echo "==> rpath after strip: '$(patchelf --print-rpath "$STAGE/usr/lib/displayxr/plugins/DisplayXR-LeiaSR.so" 2>/dev/null)'"

# Discovery manifest — probe_order 50 (vendor), absolute INSTALLED binary_path,
# NO force-probe (the merged presence probe, leia-plugin #99, claims when the SR
# runtime is reachable and declines otherwise).
STUB_SUFFIX=""; [ "$WEAVER" = "stub" ] && STUB_SUFFIX=" (stub weaver — mechanics only)"
cat > "$STAGE/usr/lib/displayxr/plugins/050-leia-sr.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "leia-sr",
        "display_name": "DisplayXR Leia SR$STUB_SUFFIX",
        "vendor":       "Leia Inc.",
        "version":      "$VERSION",
        "binary_path":  "/usr/lib/displayxr/plugins/DisplayXR-LeiaSR.so",
        "probe_order":  50
    }
}
EOF
chmod 0644 "$STAGE/usr/lib/displayxr/plugins/050-leia-sr.json"

# --- Depends: resolve DT_NEEDED sonames to owning packages (usr-merge-safe:
# `dpkg -S <bare-soname>`, basename-exact, strip :arch). Same approach as the
# runtime's package_deb_linux.sh. displayxr-runtime is a hard prereq. ----------
compute_lib_depends() {
    local sonames so pkg pkgs=""
    sonames="$(objdump -p "$SO" 2>/dev/null | awk '/NEEDED/{print $2}' | sort -u)"
    for so in $sonames; do
        pkg="$(dpkg -S "$so" 2>/dev/null \
               | awk -F': ' -v s="$so" '{n=split($2,a,"/"); if (a[n]==s){p=$1; sub(/:.*/,"",p); print p; exit}}')"
        [ -n "$pkg" ] && pkgs="$pkgs $pkg"
    done
    echo "libc6 $pkgs" | tr ' ' '\n' | sed '/^$/d' | sort -u | paste -sd, - | sed 's/,/, /g'
}
LIB_DEPENDS="$(compute_lib_depends)"
DEPENDS="displayxr-runtime, $LIB_DEPENDS"
echo "==> Depends: $DEPENDS"
echo "==> Recommends: $SR_RUNTIME_PKG   (CONFIRM the SR runtime .deb package name)"

INSTALLED_KB="$(du -sk "$STAGE/usr" | cut -f1)"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Section: libs
Priority: optional
Architecture: $ARCH
Depends: $DEPENDS
Recommends: $SR_RUNTIME_PKG
Installed-Size: $INSTALLED_KB
Maintainer: Leia Inc / The DisplayXR Project <noreply@displayxr.dev>
Homepage: https://github.com/DisplayXR/displayxr-leia-plugin
Description: DisplayXR Leia SR display processor (Linux plug-in)
 The Leia SR display-processor plug-in for the DisplayXR OpenXR runtime. Drops
 into the runtime's built-in plug-in dir (/usr/lib/displayxr/plugins) with a
 vendor probe_order (50), so it claims the display ahead of the built-in
 sim-display fallback whenever the Leia SR runtime is present and reachable.
 .
 Requires the DisplayXR runtime (Depends: displayxr-runtime). The Leia SR
 runtime is a Recommends: without it the plug-in still installs and its probe
 declines, so sim-display drives apps. The plug-in resolves the SR runtime via
 /etc/leia/sr/1/active_runtime.json (registered by the SR runtime installer) —
 no environment variables, no baked build-machine paths.
EOF

mkdir -p "$DIST_DIR"
DEB="$DIST_DIR/${PKG}_${VERSION}_${ARCH}.deb"
if command -v fakeroot >/dev/null 2>&1; then
    fakeroot dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
else
    dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
fi

echo ""
echo "==> $DEB"
dpkg-deb --info "$DEB" | sed 's/^/    /'
echo "    --- contents ---"
dpkg-deb --contents "$DEB" | sed 's/^/    /'
[ "$WEAVER" = "stub" ] && echo "" && echo "NOTE: --stub build — this .deb is for packaging-mechanics validation only, NOT a shippable artifact."

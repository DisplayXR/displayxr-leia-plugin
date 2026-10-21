#!/bin/bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# Build the macOS installer for the Leia SR plug-in:
#   DisplayXRLeiaSRPlugin-<version>.pkg
#
#   ./scripts/build-macos.sh --release --no-test     # 1. Release dylib
#   ./installer/macos/build_pkg.sh                   # 2. package (unsigned)
#
# Options
#   --build-dir <dir>   plug-in build tree (default: build-macos-release)
#   --version <X.Y.Z>   package version (default: see "Version" below)
#   --out <dir>         output directory (default: <build-dir>/_pkg)
#
# Payload (pkgbuild --install-location /, root-owned):
#   /Library/Application Support/DisplayXR/lib/displayxr/plugins/DisplayXR-LeiaSR.dylib
#   /Library/Application Support/DisplayXR/DisplayProcessors/050-leia-sr.json
#   /Library/Application Support/DisplayXR/LeiaSR/uninstall.sh
# The manifest root is one the runtime's POSIX loader searches on macOS
# (target_plugin_build_discovery_roots(), runtime target_component_loader.c),
# and the whole payload sits under the prefix its provenance check classifies
# as `platform-trusted` (target_component_platform_trusted_path()). The dylib
# path is the runtime .pkg's own plug-in dir (next to DisplayXR-SimDisplay).
# The LeiaSR platform is NOT bundled (it has its own .pkg); the SR SDK is
# statically linked (libsrSDK_loader.a), which finds the SR runtime at run time.
#
# Version: --version, else $DXR_PLUGIN_VERSION, else the plug-in identity the
# build baked into the generated manifest (DXR_PLUGIN_GIT_DESC: an exact
# `vX.Y.Z` tag gives X.Y.Z; anything else, e.g. v2.16.7-3-gabc1234, is used
# verbatim minus the `v` and warned about). A `-dirty` identity is refused when
# DXR_RELEASE_BUILD=1 (same rule as CMakeLists.txt, #203).
#
# Runtime floor: MIN_RUNTIME_VERSION derived from DXR_RUNTIME_GIT_TAG_MACOS in
# CMakeLists.txt, exactly as installer/CMakeLists.txt derives the Windows floor
# from DXR_RUNTIME_GIT_TAG. A non-release pin (branch/SHA) gives no floor.
# $DXR_MIN_RUNTIME_VERSION overrides (empty string = no gate).
#
# Signing (scripts/macos_sign_notarize.sh, copied verbatim from the runtime):
# unsigned by default; set DXR_MACOS_APP_IDENTITY (dylib: Developer ID +
# hardened runtime), DXR_MACOS_INSTALLER_IDENTITY (productsign) and
# DXR_NOTARY_PROFILE or DXR_NOTARY_KEY_* (notarize + staple) to sign.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SIGN="$ROOT/scripts/macos_sign_notarize.sh"

BUILD_DIR="$ROOT/build-macos-release"
VERSION="${DXR_PLUGIN_VERSION:-}"
OUT_DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
    --build-dir) BUILD_DIR="$(cd "${2:?}" && pwd)"; shift 2 ;;
    --version) VERSION="${2:?}"; shift 2 ;;
    --out) OUT_DIR="${2:?}"; shift 2 ;;
    *) echo "Unknown option: $1 (supported: --build-dir --version --out)" >&2; exit 2 ;;
    esac
done
OUT_DIR="${OUT_DIR:-$BUILD_DIR/_pkg}"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

die() { echo "error: $*" >&2; exit 1; }

DYLIB="$BUILD_DIR/src/drv_leia_macos/DisplayXR-LeiaSR.dylib"
BUILT_MANIFEST="$BUILD_DIR/src/drv_leia_macos/050-leia-sr.json"
[ -f "$DYLIB" ] || die "$DYLIB not found — run ./scripts/build-macos.sh --release first"
[ -f "$BUILT_MANIFEST" ] || die "$BUILT_MANIFEST not found"

BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:[A-Z]*=//p' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || true)"
if [ "$BUILD_TYPE" != "Release" ]; then
    [ "${DXR_PKG_ALLOW_NON_RELEASE:-0}" = "1" ] ||
        die "$BUILD_DIR is a '${BUILD_TYPE:-unknown}' build; package a Release build (or set DXR_PKG_ALLOW_NON_RELEASE=1)"
    echo "warning: packaging a '$BUILD_TYPE' build (DXR_PKG_ALLOW_NON_RELEASE=1)"
fi

# --- Version ----------------------------------------------------------------
IDENTITY="$(sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$BUILT_MANIFEST" | head -1)"
if [ "${DXR_RELEASE_BUILD:-0}" != "0" ] && [[ "$IDENTITY" == *-dirty ]]; then
    die "refusing a RELEASE package of a dirty build (plug-in identity '$IDENTITY', #203)"
fi
if [ -z "$VERSION" ]; then
    VERSION="${IDENTITY#v}"
    [ -n "$VERSION" ] || die "cannot derive a version (no identity in $BUILT_MANIFEST); pass --version"
fi
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "warning: package version '$VERSION' is not a release X.Y.Z (plug-in identity '$IDENTITY')"
fi

# --- Runtime floor ----------------------------------------------------------
if [ -n "${DXR_MIN_RUNTIME_VERSION+x}" ]; then
    MIN_RUNTIME_VERSION="$DXR_MIN_RUNTIME_VERSION"
    FLOOR_SRC="DXR_MIN_RUNTIME_VERSION"
else
    PIN="$(sed -n 's/^set(DXR_RUNTIME_GIT_TAG_MACOS "\([^"]*\)".*/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
    [ -n "$PIN" ] || die "DXR_RUNTIME_GIT_TAG_MACOS not found in CMakeLists.txt"
    if [[ "$PIN" =~ ^v?([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
        MIN_RUNTIME_VERSION="${BASH_REMATCH[1]}"
    else
        MIN_RUNTIME_VERSION=""
        echo "warning: DXR_RUNTIME_GIT_TAG_MACOS=$PIN is not a release tag: the package has NO runtime floor"
    fi
    FLOOR_SRC="DXR_RUNTIME_GIT_TAG_MACOS=$PIN"
fi
[ -z "$MIN_RUNTIME_VERSION" ] || [[ "$MIN_RUNTIME_VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
    die "runtime floor '$MIN_RUNTIME_VERSION' is not X.Y.Z"

# Minimum macOS = what the dylib was actually built for (LC_BUILD_VERSION minos;
# set it with DXR_MACOS_DEPLOYMENT_TARGET at build time), never a guess.
MIN_MACOS="$(otool -l "$DYLIB" | awk '/LC_BUILD_VERSION/{f=1} f && $1=="minos"{print $2; exit}')"
[ -n "$MIN_MACOS" ] || die "cannot read the dylib's minimum macOS (LC_BUILD_VERSION)"

echo "=== DisplayXR Leia SR plug-in .pkg ==="
echo "  build:          $BUILD_DIR ($BUILD_TYPE)"
echo "  identity:       $IDENTITY"
echo "  version:        $VERSION"
echo "  runtime floor:  ${MIN_RUNTIME_VERSION:-none} ($FLOOR_SRC)"
echo "  minimum macOS:  $MIN_MACOS"
"$SIGN" status | sed 's/^/  /'

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
INSTALL_PREFIX="/Library/Application Support/DisplayXR"
PAYLOAD="$WORK/payload"
P_PREFIX="$PAYLOAD$INSTALL_PREFIX"
mkdir -p "$P_PREFIX/lib/displayxr/plugins" "$P_PREFIX/DisplayProcessors" "$P_PREFIX/LeiaSR"

# --- Dylib: stage, strip build-tree rpaths, guard ----------------------------
P_DYLIB="$P_PREFIX/lib/displayxr/plugins/DisplayXR-LeiaSR.dylib"
cp "$DYLIB" "$P_DYLIB"
chmod 0755 "$P_DYLIB"

[ "$(lipo -archs "$P_DYLIB")" = "arm64" ] || die "expected an arm64-only dylib, got: $(lipo -archs "$P_DYLIB")"
EXPORTS="$(nm -gU "$P_DYLIB" | awk '{print $3}')"
[ "$EXPORTS" = "_xrtPluginNegotiate" ] || die "dylib exports more than _xrtPluginNegotiate (ADR-019): $EXPORTS"

# The plug-in links nothing of the runtime's (static aux libs) and no
# third-party dylib, so it needs no LC_RPATH at all: any absolute rpath is a
# build-tree leftover (e.g. a Homebrew Cellar dir) and goes.
for r in $(otool -l "$P_DYLIB" | awk '/LC_RPATH/{f=1} f && $1=="path"{print $2; f=0}'); do
    case "$r" in
    /*) echo "  removing build-tree LC_RPATH $r"; install_name_tool -delete_rpath "$r" "$P_DYLIB" ;;
    esac
done

# Every load path must resolve on a Mac without Homebrew or this build box.
# Same guard as the runtime's installer/macos/build_installer.sh (BAD_DEPS),
# extended to LC_RPATH and to @rpath references (with no rpath they cannot
# resolve to anything shipped).
BAD="$(otool -L "$P_DYLIB" | tail -n +2 | awk '{print $1}' |
    grep -E '^(/opt/homebrew|/usr/local|/tmp|/private/tmp|/Users|/opt/leiasr)/|^@rpath/' |
    grep -v '^@rpath/DisplayXR-LeiaSR.dylib$' || true)"
BAD_RPATH="$(otool -l "$P_DYLIB" | awk '/LC_RPATH/{f=1} f && $1=="path"{print $2; f=0}' |
    grep -E '^(/opt/homebrew|/usr/local|/tmp|/private/tmp|/Users)/' || true)"
if [ -n "$BAD$BAD_RPATH" ]; then
    die "DisplayXR-LeiaSR.dylib depends on build-box paths:
$BAD $BAD_RPATH
(Build with ./scripts/build-macos.sh --release, which compiles cJSON in instead of linking Homebrew's.)"
fi
echo "  load paths OK:"
otool -L "$P_DYLIB" | tail -n +2 | sed 's/^/    /'

# --- Manifest + uninstaller ------------------------------------------------
cat >"$P_PREFIX/DisplayProcessors/050-leia-sr.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "leia-sr",
        "display_name": "DisplayXR Leia SR (macOS)",
        "vendor":       "Leia Inc.",
        "version":      "$VERSION",
        "binary_path":  "$INSTALL_PREFIX/lib/displayxr/plugins/DisplayXR-LeiaSR.dylib",
        "probe_order":  50
    }
}
EOF
plutil -lint -s "$P_PREFIX/DisplayProcessors/050-leia-sr.json" 2>/dev/null ||
    python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$P_PREFIX/DisplayProcessors/050-leia-sr.json" ||
    die "generated manifest is not valid JSON"
chmod 0644 "$P_PREFIX/DisplayProcessors/050-leia-sr.json"
cp "$SCRIPT_DIR/uninstall.sh" "$P_PREFIX/LeiaSR/uninstall.sh"
chmod 0755 "$P_PREFIX/LeiaSR/uninstall.sh"

# Extended attributes (com.apple.provenance, quarantine, ...) on staged files
# become AppleDouble `._*` entries in the payload; strip them before signing.
xattr -cr "$PAYLOAD"
export COPYFILE_DISABLE=1

# --- Sign the Mach-O (LAST thing that touches it) ----------------------------
"$SIGN" sign-tree "$PAYLOAD"

# --- Component + product ---------------------------------------------------
FLOOR_TEXT="${MIN_RUNTIME_VERSION:-its minimum version}"
subst() {
    sed -e "s#@VERSION@#$VERSION#g" \
        -e "s#@MIN_RUNTIME_VERSION@#$MIN_RUNTIME_VERSION#g" \
        -e "s#@MIN_RUNTIME_VERSION_TEXT@#$FLOOR_TEXT#g" \
        -e "s#@MIN_MACOS@#$MIN_MACOS#g" "$1" >"$2"
}
mkdir -p "$WORK/scripts" "$WORK/resources"
subst "$SCRIPT_DIR/scripts/preinstall" "$WORK/scripts/preinstall"
cp "$SCRIPT_DIR/scripts/postinstall" "$WORK/scripts/postinstall"
chmod 0755 "$WORK/scripts/preinstall" "$WORK/scripts/postinstall"
subst "$SCRIPT_DIR/resources/welcome.html" "$WORK/resources/welcome.html"
cp "$ROOT/LICENSE" "$WORK/resources/LICENSE.txt"
subst "$SCRIPT_DIR/Distribution.xml.in" "$WORK/Distribution.xml"

pkgbuild --root "$PAYLOAD" \
    --scripts "$WORK/scripts" \
    --identifier com.displayxr.leia-sr-plugin \
    --version "$VERSION" \
    --install-location / \
    --ownership recommended \
    "$WORK/leia-sr-plugin.pkg"

# On some hosts every file carries an xattr that cannot be removed
# (com.apple.provenance, set by sandboxed / agent shells), and pkgbuild then
# archives an AppleDouble `._<name>` entry next to EVERY payload path -- which
# the Installer would lay down as real files. When that happens, rebuild the
# component's Payload (cpio odc, root-owned) and Bom (mkbom -i, 0/0) from the
# staged tree, keeping pkgbuild's PackageInfo + scripts. CI and ordinary
# terminals never hit this path.
if pkgutil --payload-files "$WORK/leia-sr-plugin.pkg" | grep -q '/\._'; then
    echo "  pkgbuild archived AppleDouble ._ entries (unremovable xattrs): repacking Payload + Bom"
    C="$WORK/component"
    pkgutil --expand "$WORK/leia-sr-plugin.pkg" "$C"
    mkbom "$PAYLOAD" "$WORK/raw.bom"
    lsbom "$WORK/raw.bom" | awk -F'\t' 'BEGIN{OFS="\t"} {$3="0/0"; print}' >"$WORK/bom.txt"
    rm -f "$C/Bom" "$WORK/raw.bom"
    mkbom -i "$WORK/bom.txt" "$C/Bom"
    (cd "$PAYLOAD" && find . | LC_ALL=C sort | cpio -o --quiet --format odc -R 0:0 | gzip -9) >"$C/Payload"
    nfiles="$(lsbom "$C/Bom" | wc -l | tr -d ' ')"
    perl -pi -e "s/numberOfFiles=\"\\d+\"/numberOfFiles=\"$nfiles\"/" "$C/PackageInfo"
    rm -f "$WORK/leia-sr-plugin.pkg"
    pkgutil --flatten "$C" "$WORK/leia-sr-plugin.pkg"
    ! pkgutil --payload-files "$WORK/leia-sr-plugin.pkg" | grep -q '/\._' || die "AppleDouble entries survived the repack"
fi

OUT_PKG="$OUT_DIR/DisplayXRLeiaSRPlugin-$VERSION.pkg"
rm -f "$OUT_PKG"
productbuild --distribution "$WORK/Distribution.xml" \
    --resources "$WORK/resources" \
    --package-path "$WORK" \
    "$OUT_PKG"

# --- Verify what we built (no install) --------------------------------------
EXPECTED="./Library/Application Support/DisplayXR/DisplayProcessors/050-leia-sr.json
./Library/Application Support/DisplayXR/LeiaSR/uninstall.sh
./Library/Application Support/DisplayXR/lib/displayxr/plugins/DisplayXR-LeiaSR.dylib"
GOT="$(pkgutil --payload-files "$OUT_PKG" | grep -E '\.(json|sh|dylib)$' | sort)"
[ "$GOT" = "$EXPECTED" ] || die "payload mismatch:
$GOT"

# Sign the product, notarize, staple — each only when its credentials are set.
"$SIGN" finalize-pkg "$OUT_PKG"

echo "=== Built: $OUT_PKG ==="
ls -lh "$OUT_PKG"

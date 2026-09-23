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
#   ./scripts/package_deb_leia.sh --allow-no-capture  # permit a capture-less build
#
# Output: dist/displayxr-leia-sr_<ver>_<arch>.deb
#
# ONE package for Ubuntu 22.04, 24.04 and 26.04 — build it on the OLDEST of the
# three (an ubuntu:22.04 container; CI does this) and set DXR_DEB_MAX_GLIBC to
# that release's glibc (2.35), so a package built on anything newer fails loudly
# here instead of shipping. See the Depends section below, and
# scripts/verify_deb_install_linux.sh for the install-side proof.
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
# --- Desktop capture is a SHIPPING feature, so its libraries are Depends ------
# The transparency path's window-excluded desktop capture (runtime#757) links
# libpipewire-0.3 + libdbus-1. CMake treats both as OPTIONAL so a CI image
# without the -dev packages still builds the graceful-decline path — which is
# right for a compile check and wrong for a release: a packaging box that simply
# lacks libpipewire-0.3-dev produces a .deb that silently cannot capture (v2.0.4
# shipped exactly that). So this script REQUIRES both in the built .so's
# DT_NEEDED and fails otherwise; `--allow-no-capture` is the explicit opt-out.
# Once linked they are hard runtime requirements (a missing soname fails the
# plug-in's dlopen outright), and the dpkg-shlibdeps pass below turns them into
# versioned Depends automatically.
#
# The capture additionally needs the DisplayXR GNOME Shell extension (version 2,
# org.displayxr.CaptureExclusion1) — without it the plug-in declines to capture
# and falls back to silhouette intersection. That is a Recommends on the virtual
# package `displayxr-window-geometry-publisher` (never on a concrete package:
# docs/specs/runtime/wayland-window-geometry.md §4 in the runtime), which the
# runtime .deb provides.
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
ALLOW_NO_CAPTURE=0
for arg in "$@"; do
    case "$arg" in
    --stub) WEAVER="stub" ;;
    --no-build) NO_BUILD=1 ;;
    --allow-no-capture) ALLOW_NO_CAPTURE=1 ;;
    *) echo "Unknown option: $arg (supported: --stub --no-build --allow-no-capture)" >&2; exit 2 ;;
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

# Desktop capture gate (see header): the release .so must link both capture libs.
NEEDED="$(objdump -p "$SO" 2>/dev/null | awk '/NEEDED/{print $2}')"
CAPTURE=1
for lib in libpipewire-0.3.so.0 libdbus-1.so.3; do
    echo "$NEEDED" | grep -qx "$lib" || CAPTURE=0
done
if [ "$CAPTURE" = 1 ]; then
    echo "==> Desktop capture: linked (libpipewire-0.3 + libdbus-1)"
elif [ "$ALLOW_NO_CAPTURE" = 1 ]; then
    echo "==> WARNING: desktop capture NOT linked — --allow-no-capture set; this .deb's"
    echo "    transparency will fall back to silhouette intersection."
else
    echo "error: $SO does not link libpipewire-0.3 + libdbus-1, so the .deb would ship" >&2
    echo "       without desktop capture (transparency degraded to silhouette intersection)." >&2
    echo "       Install libpipewire-0.3-dev + libdbus-1-dev and rebuild (delete $BUILD_DIR" >&2
    echo "       first — CMake caches the failed pkg-config probe), or pass --allow-no-capture." >&2
    exit 1
fi

# Version: git describe → Debian-legal upstream version (same rule as the runtime).
# --match 'v[0-9]*' so the plug-in version derives ONLY from canonical release
# tags — the sr-sdk-v<...> SDK-pin tags share this repo and would otherwise be
# picked up by `git describe` as the nearest tag (they are the newest tags).
RAW="$(git -C "$ROOT" describe --tags --always --dirty --match 'v[0-9]*' 2>/dev/null || echo 0.0.0)"
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
STAGED_SO="$STAGE/usr/lib/displayxr/plugins/DisplayXR-LeiaSR.so"
patchelf --remove-rpath "$STAGED_SO"
echo "==> rpath after strip: '$(patchelf --print-rpath "$STAGED_SO" 2>/dev/null)'"

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

# --- Depends: ONE .deb for Ubuntu 22.04, 24.04 and 26.04 --------------------
# The v2.7.0 package installed on 24.04 only, and for TWO independent reasons.
# Both are checked here rather than trusted to the build host (the runtime made
# the same fix in its #1656 / PR #1659):
#
#   1. The glibc / libstdc++ floor is the BUILD host's. v2.7.0 was built on the
#      24.04 runner, needed GLIBC_2.38 / GLIBCXX_3.4.31, and declared an
#      UNVERSIONED `libc6` — so apt happily installed it on 22.04 and the
#      runtime then failed to dlopen the plug-in. Release artifacts are
#      therefore built on the OLDEST supported release (an ubuntu:22.04
#      container), and `dpkg-shlibdeps` turns the symbol versions the .so
#      actually references into VERSIONED Depends, so a package built on a
#      newer host says `libc6 (>= 2.38)` and apt REFUSES it on 22.04 instead of
#      installing something that cannot load.
#      DXR_DEB_MAX_GLIBC (CI sets 2.35 = Ubuntu 22.04 on the jobs that build
#      there) makes a floor above the oldest supported release a hard error.
#
#   2. A system library's PACKAGE NAME is not stable across releases. v2.7.0
#      declared `libpipewire-0.3-0t64`, which simply does not exist on 22.04
#      (jammy has `libpipewire-0.3-0`). So every DT_NEEDED soname must be on
#      STABLE_SONAMES below: the rule is "the package name dpkg-shlibdeps
#      derives ON THE OLDEST SUPPORTED RELEASE resolves on all three". A newly
#      linked library fails the build here instead of silently narrowing the
#      releases the package installs on, and CI's DebInstall matrix
#      (scripts/verify_deb_install_linux.sh) is what turns each entry from a
#      claim into a check.
STABLE_SONAMES=(
    libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 ld-linux-x86-64.so.2
    libstdc++.so.6 libgcc_s.so.1
    libvulkan.so.1                  # libvulkan1
    libcjson.so.1                   # libcjson1
    libdbus-1.so.3                  # libdbus-1-3
    libxcb.so.1 libxcb-randr.so.0   # libxcb1, libxcb-randr0
    # libpipewire-0.3-0 on 22.04; RENAMED to libpipewire-0.3-0t64 on 24.04 and
    # 26.04 by the time_t transition. It still qualifies, but ONLY in the
    # oldest-release direction: the t64 package carries
    # `Provides: libpipewire-0.3-0 (= <version>)`, so the 22.04-derived
    # `libpipewire-0.3-0 (>= 0.3.x)` is satisfied on 24.04/26.04 through that
    # versioned Provides. The reverse has no fallback — a 24.04-built package
    # names libpipewire-0.3-0t64 and is uninstallable on jammy, which is
    # exactly what v2.7.0 shipped. DebInstall proves both directions.
    libpipewire-0.3.so.0
)

command -v dpkg-shlibdeps >/dev/null 2>&1 || { echo "error: dpkg-shlibdeps not found — install dpkg-dev." >&2; exit 1; }

bad=""
for so in $(objdump -p "$STAGED_SO" | awk '/NEEDED/{print $2}' | sort -u); do
    ok=0
    for s in "${STABLE_SONAMES[@]}"; do [ "$so" = "$s" ] && ok=1 && break; done
    [ "$ok" = 1 ] || bad="$bad $so"
done
if [ -n "$bad" ]; then
    echo "error: DT_NEEDED on system libraries not known to resolve under one package name" >&2
    echo "       across Ubuntu 22.04/24.04/26.04:$bad" >&2
    echo "       Drop the dependency, link it statically, or add it to STABLE_SONAMES once" >&2
    echo "       CI's DebInstall matrix proves the 22.04-derived name resolves on all three." >&2
    exit 1
fi

# dpkg-shlibdeps wants a debian/control to read; give it a throwaway one. It
# resolves each soname through the linker search path and the owning package's
# shlibs/symbols files, so the result carries real version floors.
SHLIBS_TMP="$(mktemp -d)"
mkdir -p "$SHLIBS_TMP/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$PKG" "$PKG" >"$SHLIBS_TMP/debian/control"
LIB_DEPENDS="$(cd "$SHLIBS_TMP" && dpkg-shlibdeps -O "$STAGED_SO" | sed -n 's/^shlibs:Depends=//p')"
rm -rf "$SHLIBS_TMP"
[ -n "$LIB_DEPENDS" ] || { echo "error: dpkg-shlibdeps produced no Depends." >&2; exit 1; }
DEPENDS="displayxr-runtime, $LIB_DEPENDS"
# cube-hw finding (B): the SR runtime bundles copies of common .so's under
# /opt/leiasr/lib. On a box whose linker path reaches them, the owning package
# comes out as the vendor SR package — which must stay a Recommends, never a
# hard Depend (the plug-in installs and declines its probe without it).
if echo "$DEPENDS" | grep -q leiasr; then
    echo "error: Depends names the vendor SR package: $DEPENDS" >&2
    echo "       (an SR runtime's bundled lib shadowed a system one on this host's linker path)" >&2
    exit 1
fi
echo "==> Depends: $DEPENDS"

GLIBC_FLOOR="$(objdump -T "$STAGED_SO" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
GLIBCXX_FLOOR="$(objdump -T "$STAGED_SO" | grep -o 'GLIBCXX_[0-9.]*' | sed 's/GLIBCXX_//' | sort -uV | tail -1)"
echo "==> glibc floor: GLIBC_$GLIBC_FLOOR, GLIBCXX_${GLIBCXX_FLOOR:-none}"
if [ -n "${DXR_DEB_MAX_GLIBC:-}" ] &&
    [ "$(printf '%s\n%s\n' "$GLIBC_FLOOR" "$DXR_DEB_MAX_GLIBC" | sort -V | tail -1)" != "$DXR_DEB_MAX_GLIBC" ]; then
    echo "error: the plug-in needs GLIBC_$GLIBC_FLOOR, above DXR_DEB_MAX_GLIBC=$DXR_DEB_MAX_GLIBC" >&2
    echo "       (the oldest supported release). Build the .deb on that release —" >&2
    echo "       CI does this in an ubuntu:22.04 container." >&2
    exit 1
fi

# The GNOME Shell extension that makes window-excluded capture possible is
# satisfied by the runtime .deb (or any vendor package shipping the publisher).
RECOMMENDS="$SR_RUNTIME_PKG"
[ "$CAPTURE" = 1 ] && RECOMMENDS="$RECOMMENDS, displayxr-window-geometry-publisher"
echo "==> Recommends: $RECOMMENDS   (CONFIRM the SR runtime .deb package name)"

INSTALLED_KB="$(du -sk "$STAGE/usr" | cut -f1)"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Section: libs
Priority: optional
Architecture: $ARCH
Depends: $DEPENDS
Recommends: $RECOMMENDS
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
 .
 Transparent apps get the desktop behind their window from a GNOME/Mutter
 screen capture that excludes the app's own windows. That needs the DisplayXR
 GNOME Shell extension window-geometry@displayxr.org (version 2 or later),
 enabled for the user; without it transparency falls back to silhouette
 intersection.
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

# Explicit success: the trailing test above returns 1 on an sdk build, which
# would otherwise become the script's exit code (cube-hw finding E).
exit 0

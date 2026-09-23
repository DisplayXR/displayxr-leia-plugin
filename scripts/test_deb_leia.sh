#!/usr/bin/env bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# Acceptance test for the Leia SR plug-in .deb (Phase 2 of runtime #781).
#
# Two modes:
#
#   --stub   (default, runs anywhere with Docker; NO SR SDK needed)
#            Builds the Track A stub-weaver plug-in .deb IN AN ubuntu:22.04
#            BUILDER (the oldest supported release — a package's glibc floor is
#            its build host's), then installs the RUNTIME .deb + this plug-in
#            .deb together in a pristine container of EVERY supported release
#            (22.04, 24.04, 26.04; override with DXR_TEST_RELEASES) and asserts
#            COEXISTENCE + discovery ordering:
#              * with DXR_LEIA_FORCE_PROBE=1  -> active plug-in = leia-sr (probe_order 50)
#              * without it                    -> stub declines -> sim-display claims
#            This validates the packaging MECHANICS (build, patchelf rpath strip,
#            versioned Depends, dpkg, install-alongside, probe_order) on every
#            supported release — NOT real weaving. It is the local twin of CI's
#            DebStub + DebInstall jobs; scripts/verify_deb_install_linux.sh is
#            the shared verifier both use.
#
#   --sdk    (SR-equipped box only) Builds the real Track B plug-in .deb from
#            $SRSDK_ROOT. Real-weave / claim-over-sim acceptance needs the SR
#            runtime + hardware and is validated on that box, not here.
#
# Requires a displayxr-runtime checkout next to this repo (or DXR_RUNTIME_SOURCE_DIR)
# so both the runtime .deb and this plug-in build from one source of truth.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME_DIR="${DXR_RUNTIME_SOURCE_DIR:-$(cd "$ROOT/.." && pwd)/displayxr-runtime}"
# Build in the OLDEST supported release, exactly as CI's DebStub job does: a
# package built on 24.04 needs GLIBC_2.38 and cannot run on 22.04.
IMAGE="${DXR_DEB_BUILDER_IMAGE:-displayxr-deb-builder:ubuntu2204}"
RELEASES="${DXR_TEST_RELEASES:-22.04 24.04 26.04}"

MODE="stub"
for arg in "$@"; do
    case "$arg" in
    --stub) MODE="stub" ;;
    --sdk) MODE="sdk" ;;
    *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

command -v docker >/dev/null 2>&1 || { echo "error: docker not found" >&2; exit 1; }
[ -f "$RUNTIME_DIR/CMakeLists.txt" ] || { echo "error: runtime checkout not found at $RUNTIME_DIR" >&2; exit 1; }
[ -f "$RUNTIME_DIR/scripts/package_deb_linux.sh" ] || { echo "error: runtime package_deb_linux.sh missing (needs runtime #781 Phase 1)" >&2; exit 1; }

if [ "$MODE" = "sdk" ]; then
    echo "==> Track B (--sdk): build the real plug-in .deb on an SR box."
    [ -n "${SRSDK_ROOT:-}" ] || { echo "error: SRSDK_ROOT unset (Track B needs the SR SDK)." >&2; exit 1; }
    SRSDK_ROOT="$SRSDK_ROOT" DXR_RUNTIME_SOURCE_DIR="$RUNTIME_DIR" "$ROOT/scripts/package_deb_leia.sh"
    echo "==> Built. Real-weave / claim-over-sim acceptance runs on the SR box (hardware)."
    exit 0
fi

# --- Builder image with patchelf (extends the runtime builder if present) ----
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "==> builder image $IMAGE not found — build it via the runtime's scripts/test_deb_linux.sh first." >&2
    echo "    (that script builds an ubuntu:22.04 builder; override with DXR_DEB_BUILDER_IMAGE)" >&2
    exit 1
fi

# 1. Build the runtime .deb + the stub plug-in .deb (repos bind-mounted; both
#    build out-of-tree so no host build/ cache collides). patchelf added at run.
#    DXR_DEB_MAX_GLIBC mirrors CI: the builder IS the oldest supported release,
#    so a floor above it means something dragged a newer toolchain in.
echo "==> Building runtime .deb + stub plug-in .deb in $IMAGE"
docker run --rm \
    -e DXR_DEB_MAX_GLIBC=2.35 \
    -v "$RUNTIME_DIR":/runtime \
    -v "$ROOT":/plugin \
    "$IMAGE" bash -c '
    set -e
    export DEBIAN_FRONTEND=noninteractive
    # libpipewire-0.3-dev + libdbus-1-dev: package_deb_leia.sh refuses a .deb
    # without the desktop-capture libs linked (the runtime builder image lacks them).
    # dpkg-dev: dpkg-shlibdeps, which derives the versioned Depends.
    apt-get update -qq && apt-get install -y -qq patchelf libpipewire-0.3-dev libdbus-1-dev dpkg-dev >/dev/null
    git config --global --add safe.directory /runtime
    git config --global --add safe.directory /plugin
    echo "--- runtime .deb ---"
    ( cd /runtime && BUILD_DIR=/root/rt-build ./scripts/package_deb_linux.sh )
    echo "--- plug-in stub .deb ---"
    ( cd /plugin && BUILD_DIR=/root/pl-build DXR_RUNTIME_SOURCE_DIR=/runtime DIST_DIR=/plugin/dist \
        ./scripts/package_deb_leia.sh --stub )
'

RT_DEB="$(ls -t "$RUNTIME_DIR"/dist/displayxr-runtime_*_*.deb 2>/dev/null | head -1 || true)"
PL_DEB="$(ls -t "$ROOT"/dist/displayxr-leia-sr_*_*.deb 2>/dev/null | head -1 || true)"
[ -n "$RT_DEB" ] && [ -n "$PL_DEB" ] || { echo "error: missing one of the .debs (runtime='$RT_DEB' plugin='$PL_DEB')" >&2; exit 1; }
echo "==> runtime: $(basename "$RT_DEB")"
echo "==> plugin:  $(basename "$PL_DEB")"

# 2. Every supported release, pristine: install BOTH and check them there with
#    the SAME verifier CI runs (scripts/verify_deb_install_linux.sh), then the
#    force-probe half of the ordering contract, which is specific to this test.
for release in $RELEASES; do
    echo ""
    echo "############ ubuntu:$release ############"
    docker run --rm \
        -v "$ROOT":/w -w /w \
        -v "$(dirname "$RT_DEB")":/rt:ro \
        -v "$(dirname "$PL_DEB")":/pl:ro \
        "ubuntu:$release" bash -c '
        set -e
        ./scripts/verify_deb_install_linux.sh "/pl/'"$(basename "$PL_DEB")"'" "/rt/'"$(basename "$RT_DEB")"'"

        echo "=== force-probe: leia-sr claims at probe_order 50 over sim-display ==="
        out_forced="$(DXR_LEIA_FORCE_PROBE=1 displayxr-cli info 2>&1)"
        echo "$out_forced" | grep -E "active plug-in|probe_order" || true
        echo "$out_forced" | grep -q "id=leia-sr" || { echo "FAIL: leia-sr did not claim under force-probe"; exit 1; }
        echo "$out_forced" | grep -q "probe_order=50" || { echo "FAIL: leia-sr not at probe_order 50"; exit 1; }
        echo ""
        echo "ACCEPTANCE PASS (stub mechanics) — plug-in .deb installs beside the runtime .deb,"
        echo "drops into /usr/lib/displayxr/plugins, and is discovered at probe_order 50."
    '
done

echo ""
echo "==> PASS on: $RELEASES"

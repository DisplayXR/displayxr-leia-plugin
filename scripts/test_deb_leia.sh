#!/usr/bin/env bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# Acceptance test for the Leia SR plug-in .deb (Phase 2 of runtime #781).
#
# Two modes:
#
#   --stub   (default, runs anywhere with Docker; NO SR SDK needed)
#            Builds the Track A stub-weaver plug-in .deb, then in a pristine
#            ubuntu:24.04 installs the RUNTIME .deb + this plug-in .deb together
#            and asserts COEXISTENCE + discovery ordering:
#              * with DXR_LEIA_FORCE_PROBE=1  -> active plug-in = leia-sr (probe_order 50)
#              * without it                    -> stub declines -> sim-display claims
#            This validates the packaging MECHANICS (build, patchelf rpath strip,
#            Depends, dpkg, install-alongside, probe_order) — NOT real weaving.
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
IMAGE="displayxr-deb-builder:ubuntu2404"   # same builder image the runtime test uses

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
    exit 1
fi

# 1. Build the runtime .deb + the stub plug-in .deb (repos bind-mounted; both
#    build out-of-tree so no host build/ cache collides). patchelf added at run.
echo "==> Building runtime .deb + stub plug-in .deb in $IMAGE"
docker run --rm \
    -v "$RUNTIME_DIR":/runtime \
    -v "$ROOT":/plugin \
    "$IMAGE" bash -c '
    set -e
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq && apt-get install -y -qq patchelf >/dev/null
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

# 2. Pristine ubuntu:24.04: install BOTH, assert coexistence + probe ordering.
docker run --rm \
    -v "$(dirname "$RT_DEB")":/rt:ro \
    -v "$(dirname "$PL_DEB")":/pl:ro \
    ubuntu:24.04 bash -c '
    set -e
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    echo "=== install runtime + leia plug-in together (zero env vars) ==="
    apt-get install -y -qq "/rt/'"$(basename "$RT_DEB")"'" "/pl/'"$(basename "$PL_DEB")"'"

    echo "=== both DPs present in the shared discovery dir ==="
    ls -1 /usr/lib/displayxr/plugins/

    echo "=== default (no force-probe): stub declines -> sim-display claims ==="
    out_default="$(displayxr-cli info 2>&1)"; echo "$out_default" | grep -E "active plug-in|Selected|:: Display processor" || true
    echo "$out_default" | grep -q "id=sim-display" || { echo "FAIL: expected sim-display to claim by default"; exit 1; }
    echo "ok — sim-display claims by default (graceful fallback)"

    echo "=== force-probe: leia-sr claims at probe_order 50 over sim-display ==="
    out_forced="$(DXR_LEIA_FORCE_PROBE=1 displayxr-cli info 2>&1)"; echo "$out_forced" | grep -E "active plug-in|probe_order" || true
    echo "$out_forced" | grep -q "id=leia-sr" || { echo "FAIL: leia-sr did not claim under force-probe"; exit 1; }
    echo "$out_forced" | grep -q "probe_order=50" || { echo "FAIL: leia-sr not at probe_order 50"; exit 1; }
    echo ""
    echo "ACCEPTANCE PASS (stub mechanics) — plug-in .deb coexists with the runtime .deb,"
    echo "drops into /usr/lib/displayxr/plugins, and is discovered at probe_order 50."
'

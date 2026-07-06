#!/usr/bin/env bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# Build + validate the Linux arm of the Leia plug-in (Track A: stub weaver).
#
#   ./scripts/build-linux.sh [--no-test] [--clean]
#
# Environment:
#   DXR_RUNTIME_SOURCE_DIR  Local displayxr-runtime checkout (default:
#                           ../displayxr-runtime next to this repo). The
#                           runtime is consumed via add_subdirectory — no
#                           FetchContent, no network.
#
# What it does:
#   1. Configure + build DisplayXR-LeiaSR.so and the runtime's displayxr-cli
#      from one build tree.
#   2. Assert the .so exports exactly one symbol (xrtPluginNegotiate).
#   3. Stage the plug-in + a generated 050-leia-sr.json manifest into
#      build/_plugins and run `displayxr-cli info` + `selftest` against it
#      via XRT_PLUGIN_SEARCH_PATH with DXR_LEIA_FORCE_PROBE=1 (the Track A
#      stub has no hardware probe — see src/drv_leia_linux/leia_plugin_linux.c).
#
# Debian/Ubuntu deps: see .github/workflows/build-linux.yml (same list).

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build"
RUNTIME_DIR="${DXR_RUNTIME_SOURCE_DIR:-$(cd "$ROOT/.." && pwd)/displayxr-runtime}"

RUN_TEST=1
for arg in "$@"; do
    case "$arg" in
    --no-test) RUN_TEST=0 ;;
    --clean) rm -rf "$BUILD_DIR" ;;
    *)
        echo "Unknown option: $arg (supported: --no-test --clean)" >&2
        exit 2
        ;;
    esac
done

if [ ! -f "$RUNTIME_DIR/CMakeLists.txt" ]; then
    echo "error: runtime checkout not found at $RUNTIME_DIR" >&2
    echo "       clone DisplayXR/displayxr-runtime there, or set DXR_RUNTIME_SOURCE_DIR." >&2
    exit 1
fi

echo "==> Configuring (runtime: $RUNTIME_DIR)"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DDXR_RUNTIME_SOURCE_DIR="$RUNTIME_DIR"

echo "==> Building plug-in"
cmake --build "$BUILD_DIR"

echo "==> Building displayxr-cli (runtime target, explicit — EXCLUDE_FROM_ALL)"
cmake --build "$BUILD_DIR" --target cli

SO="$BUILD_DIR/src/drv_leia_linux/DisplayXR-LeiaSR.so"
[ -f "$SO" ] || { echo "error: $SO not built" >&2; exit 1; }

echo "==> Asserting single-export discipline (#496 / ADR-019)"
SYMS="$(nm -D --defined-only "$SO" | awk '{print $NF}')"
if [ "$SYMS" != "xrtPluginNegotiate" ]; then
    echo "error: unexpected exported symbols:" >&2
    nm -D --defined-only "$SO" >&2
    exit 1
fi
echo "    OK: only xrtPluginNegotiate is exported"

CLI="$(find "$BUILD_DIR/runtime-build/src/xrt/targets/cli" -maxdepth 1 -name displayxr-cli -type f 2>/dev/null | head -1)"
[ -n "$CLI" ] || { echo "error: displayxr-cli not found under $BUILD_DIR/runtime-build" >&2; exit 1; }

echo "==> Staging plug-in + manifest"
PLUGIN_DIR="$BUILD_DIR/_plugins"
mkdir -p "$PLUGIN_DIR"
cp "$SO" "$PLUGIN_DIR/"
# binary_path must be ABSOLUTE — the loader does no relative resolution.
cat >"$PLUGIN_DIR/050-leia-sr.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "leia-sr",
        "display_name": "DisplayXR Leia SR (Linux, stub weaver)",
        "vendor":       "Leia Inc.",
        "version":      "dev",
        "binary_path":  "$PLUGIN_DIR/DisplayXR-LeiaSR.so",
        "probe_order":  50
    }
}
EOF

if [ "$RUN_TEST" = "1" ]; then
    export XRT_PLUGIN_SEARCH_PATH="$PLUGIN_DIR"
    echo "==> displayxr-cli info (DXR_LEIA_FORCE_PROBE=1)"
    DXR_LEIA_FORCE_PROBE=1 "$CLI" info || true
    echo "==> displayxr-cli selftest (DXR_LEIA_FORCE_PROBE=1)"
    DXR_LEIA_FORCE_PROBE=1 "$CLI" selftest
    echo "    selftest PASSED"
fi

echo ""
echo "Done."
echo "  Plug-in:  $SO"
echo "  Manifest: $PLUGIN_DIR/050-leia-sr.json"
echo "  Run an app against it with:"
echo "    export XRT_PLUGIN_SEARCH_PATH=$PLUGIN_DIR"
echo "    export DXR_LEIA_FORCE_PROBE=1"

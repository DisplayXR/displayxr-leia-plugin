#!/usr/bin/env bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# Build + validate the macOS arm of the Leia plug-in (srSDK Metal weaver).
#
#   ./scripts/build-macos.sh [--no-test] [--clean]
#
# Environment:
#   DXR_RUNTIME_SOURCE_DIR  Local displayxr-runtime checkout (default:
#                           ../displayxr-runtime). Consumed via add_subdirectory;
#                           the runtime dylib, displayxr-cli and the sim-display
#                           plug-in are built from the SAME tree, so the plug-in
#                           and the runtime it is tested against share headers.
#   SRSDK_ROOT              LeiaSR macOS install tree (include/sr + lib/libsrSDK_loader.a).
#   LEIASR_MACOS_DIR        Or: a LeiaSR-macos checkout with a build tree
#                           (default ../LeiaSR-macos). Headers come from
#                           modules/srSDK/include, the loader from
#                           build/macos/sr/libsrSDK_loader.a — the packaged
#                           build/macos/install can lag the build.
#
# Output (build-macos/):
#   _plugins/DisplayXR-LeiaSR.dylib + 050-leia-sr.json
#   _plugins/DisplayXR-SimDisplay.dylib + 200-sim-display.json (the fallback,
#     so `displayxr-cli displays --claims` shows the claim resolution)
#   openxr_displayxr-dev.json (XR_RUNTIME_JSON for an app)
#   env.sh (everything an app run needs: XR_RUNTIME_JSON, XRT_PLUGIN_SEARCH_PATH,
#     SR_RUNTIME_PATH / LEIASR_* for a dev SR tree)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT/build-macos"
PARENT="$(cd "$ROOT/.." && pwd)"
RUNTIME_DIR="${DXR_RUNTIME_SOURCE_DIR:-$PARENT/displayxr-runtime}"
LEIASR_MACOS_DIR="${LEIASR_MACOS_DIR:-$PARENT/LeiaSR-macos}"

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

[ -f "$RUNTIME_DIR/CMakeLists.txt" ] || { echo "error: runtime checkout not found at $RUNTIME_DIR" >&2; exit 1; }

SR_ARGS=()
SR_RUNTIME_DYLIB=""
if [ -n "${SRSDK_ROOT:-}" ]; then
    SR_ARGS+=("-DSRSDK_ROOT=$SRSDK_ROOT")
    SR_RUNTIME_DYLIB="$SRSDK_ROOT/lib/libLeiaSR_runtime.dylib"
elif [ -f "$LEIASR_MACOS_DIR/build/macos/sr/libsrSDK_loader.a" ]; then
    SR_ARGS+=("-DSRSDK_INCLUDE_DIR=$LEIASR_MACOS_DIR/modules/srSDK/include"
              "-DSRSDK_LOADER_LIBRARY=$LEIASR_MACOS_DIR/build/macos/sr/libsrSDK_loader.a")
    SR_RUNTIME_DYLIB="$LEIASR_MACOS_DIR/build/macos/sr/libLeiaSR_runtime.dylib"
else
    echo "error: no LeiaSR macOS SDK — set SRSDK_ROOT or LEIASR_MACOS_DIR" >&2
    exit 1
fi

echo "==> Configuring (runtime: $RUNTIME_DIR)"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DDXR_RUNTIME_SOURCE_DIR="$RUNTIME_DIR" \
    "${SR_ARGS[@]}"

echo "==> Building plug-in + runtime dylib + displayxr-cli + sim-display (one tree)"
cmake --build "$BUILD_DIR" --target DisplayXR-LeiaSR openxr_displayxr cli drv_sim_display_plugin

DYLIB="$BUILD_DIR/src/drv_leia_macos/DisplayXR-LeiaSR.dylib"
echo "==> Export check"
EXPORTS="$(nm -gU "$DYLIB" | awk '{print $3}')"
if [ "$EXPORTS" != "_xrtPluginNegotiate" ]; then
    echo "error: $DYLIB exports more than xrtPluginNegotiate:" >&2
    echo "$EXPORTS" >&2
    exit 1
fi
echo "    OK: only xrtPluginNegotiate is exported"

RT_BUILD="$BUILD_DIR/runtime-build"
RUNTIME_DYLIB="$(find "$RT_BUILD/src/xrt/targets/openxr" -maxdepth 1 -name 'openxr_displayxr.dylib' | head -1)"
SIM_DYLIB="$(find "$RT_BUILD/src/xrt/drivers" -maxdepth 1 -name 'DisplayXR-SimDisplay.dylib' | head -1)"
CLI="$(find "$RT_BUILD/src/xrt/targets/cli" -maxdepth 1 -name displayxr-cli -type f | head -1)"
for f in "$RUNTIME_DYLIB" "$SIM_DYLIB" "$CLI"; do
    [ -n "$f" ] && [ -f "$f" ] || { echo "error: missing runtime artifact under $RT_BUILD" >&2; exit 1; }
done

echo "==> Staging plug-ins + manifests"
PLUGIN_DIR="$BUILD_DIR/_plugins"
mkdir -p "$PLUGIN_DIR"
cp -f "$DYLIB" "$PLUGIN_DIR/"
cp -f "$SIM_DYLIB" "$PLUGIN_DIR/"
# binary_path must be ABSOLUTE — the loader does no relative resolution.
sed "s#\"binary_path\": *\"[^\"]*\"#\"binary_path\":  \"$PLUGIN_DIR/DisplayXR-LeiaSR.dylib\"#" \
    "$BUILD_DIR/src/drv_leia_macos/050-leia-sr.json" >"$PLUGIN_DIR/050-leia-sr.json"
cat >"$PLUGIN_DIR/200-sim-display.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "sim-display",
        "display_name": "DisplayXR Sim Display",
        "vendor":       "DisplayXR",
        "version":      "dev",
        "binary_path":  "$PLUGIN_DIR/DisplayXR-SimDisplay.dylib",
        "probe_order":  200
    }
}
EOF
cat >"$BUILD_DIR/openxr_displayxr-dev.json" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "DisplayXR Runtime (leia-plugin dev build)",
        "library_path": "$RUNTIME_DYLIB"
    }
}
EOF

cat >"$BUILD_DIR/env.sh" <<EOF
# source me: runs an OpenXR app against this tree's runtime + the Leia macOS plug-in.
export XR_RUNTIME_JSON="$BUILD_DIR/openxr_displayxr-dev.json"
export XRT_PLUGIN_SEARCH_PATH="$PLUGIN_DIR"
export DYLD_LIBRARY_PATH="$(dirname "$RUNTIME_DYLIB")\${DYLD_LIBRARY_PATH:+:\$DYLD_LIBRARY_PATH}"
# Dev SR tree (nothing installed to /opt/leiasr): the static loader resolves the
# SR runtime from SR_RUNTIME_PATH; LEIASR_* must match the running SRService.
export SR_RUNTIME_PATH="$SR_RUNTIME_DYLIB"
if [ -f "\$HOME/.leiasr-fpc/env.local" ]; then
    . "\$HOME/.leiasr-fpc/env.local"
    export LEIASR_DATA_DIR LEIASR_RUNTIME_DIR
    export DYLD_LIBRARY_PATH="\$DYLD_LIBRARY_PATH:\${LEIASR_DYLD:-}"
fi
EOF

if [ "$RUN_TEST" = "1" ]; then
    # shellcheck disable=SC1091
    . "$BUILD_DIR/env.sh"
    echo "==> displayxr-cli info"
    "$CLI" info || true
    echo "==> displayxr-cli displays --claims"
    "$CLI" displays --claims || true
    echo "==> displayxr-cli selftest"
    "$CLI" selftest || true
fi

echo ""
echo "Done."
echo "  Plug-in:  $DYLIB"
echo "  Staged:   $PLUGIN_DIR"
echo "  CLI:      $CLI"
echo "  Run an app:  . $BUILD_DIR/env.sh && <app>"

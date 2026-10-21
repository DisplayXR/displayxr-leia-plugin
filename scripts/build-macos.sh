#!/usr/bin/env bash
# Copyright 2026, Leia Inc / DisplayXR
# SPDX-License-Identifier: Apache-2.0
#
# Build + validate the macOS arm of the Leia plug-in (srSDK Metal weaver).
#
#   ./scripts/build-macos.sh [--no-test] [--clean] [--release]
#
#   --release  Release build in build-macos-release/ (Debug otherwise, in
#              build-macos/), with the BUNDLED cJSON compiled in (Homebrew's
#              cjson is hidden from find_package) so the dylib has no
#              Homebrew load path. This is the build installer/macos/build_pkg.sh
#              packages. DXR_MACOS_DEPLOYMENT_TARGET is passed as
#              CMAKE_OSX_DEPLOYMENT_TARGET; --release defaults it to 14.0,
#              the floor the runtime's own CI-built .pkg ships with (its
#              Mach-Os carry minos 14.0). Unset, a build targets the HOST
#              macOS, and the .pkg (whose os-version min is read from the
#              dylib) would refuse every older Mac.
#
# Environment:
#   DXR_RUNTIME_SOURCE_DIR  Local displayxr-runtime checkout (default:
#                           ../displayxr-runtime). Consumed via add_subdirectory;
#                           the runtime dylib, displayxr-cli and the sim-display
#                           plug-in are built from the SAME tree, so the plug-in
#                           and the runtime it is tested against share headers.
#   SRSDK_ROOT              LeiaSR macOS install tree (include/sr + lib/libsrSDK_loader.a).
#   LEIASR_MACOS_DIR        Or: a LeiaSR-macos checkout (default ../LeiaSR-macos).
#                           Preferred: its packaged install tree
#                           build/macos/install/{include,lib/libsrSDK_loader.a}
#                           + lib/libLeiaSR_runtime.dylib (SR_RUNTIME_PATH).
#                           Fallback when no install tree exists: headers from
#                           modules/srSDK/include, loader + runtime from
#                           build/macos/sr. LEIASR_DATA_DIR is always
#                           build/macos/sr (where build.py stages the inis).
#
# Output (build-macos/, or build-macos-release/ with --release):
#   _plugins/DisplayXR-LeiaSR.dylib + 050-leia-sr.json
#   _plugins/DisplayXR-SimDisplay.dylib + 200-sim-display.json (the fallback,
#     so `displayxr-cli displays --claims` shows the claim resolution)
#   openxr_displayxr-dev.json (XR_RUNTIME_JSON for an app)
#   env.sh (everything an app run needs: XR_RUNTIME_JSON, XRT_PLUGIN_SEARCH_PATH,
#     SR_RUNTIME_PATH / LEIASR_* for a dev SR tree)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_TYPE=Debug
BUILD_DIR="$ROOT/build-macos"
for arg in "$@"; do
    if [ "$arg" = "--release" ]; then
        BUILD_TYPE=Release
        BUILD_DIR="$ROOT/build-macos-release"
    fi
done
PARENT="$(cd "$ROOT/.." && pwd)"
# Runtime source for the macOS arm. Its per-screen slots (create_dp_metal_for_screen,
# xrt_display_processor_metal::set_present_origin / snap_window_rect) and macOS
# monitor enumeration landed on runtime main (#1872/#1873/#1875) after the
# DXR_RUNTIME_GIT_TAG_MACOS pin; until a tag carries them, build against
# runtime main (fetched from origin). Unless DXR_RUNTIME_SOURCE_DIR names a checkout, that ref is
# exported READ-ONLY (git archive, no worktree, no checkout change) from
# ../displayxr-runtime into build-macos/_runtime-src. DXR_RUNTIME_DEV_REF=""
# uses ../displayxr-runtime as is.
DXR_RUNTIME_DEV_REF="${DXR_RUNTIME_DEV_REF-main}"
if [ -n "${DXR_RUNTIME_SOURCE_DIR:-}" ]; then
    RUNTIME_DIR="$DXR_RUNTIME_SOURCE_DIR"
elif [ -n "$DXR_RUNTIME_DEV_REF" ]; then
    RUNTIME_DIR="$BUILD_DIR/_runtime-src"
else
    RUNTIME_DIR="$PARENT/displayxr-runtime"
fi
LEIASR_MACOS_DIR="${LEIASR_MACOS_DIR:-$PARENT/LeiaSR-macos}"

RUN_TEST=1
for arg in "$@"; do
    case "$arg" in
    --no-test) RUN_TEST=0 ;;
    --clean) rm -rf "$BUILD_DIR" ;;
    --release) ;; # handled above (selects BUILD_DIR before --clean runs)
    *)
        echo "Unknown option: $arg (supported: --no-test --clean --release)" >&2
        exit 2
        ;;
    esac
done

if [ -z "${DXR_RUNTIME_SOURCE_DIR:-}" ] && [ -n "$DXR_RUNTIME_DEV_REF" ]; then
    RT_REPO="$PARENT/displayxr-runtime"
    git -C "$RT_REPO" fetch -q origin "$DXR_RUNTIME_DEV_REF"
    RT_SHA="$(git -C "$RT_REPO" rev-parse FETCH_HEAD)"
    if [ "$(cat "$RUNTIME_DIR/.dxr-runtime-sha" 2>/dev/null)" != "$RT_SHA" ]; then
        echo "==> Exporting runtime $DXR_RUNTIME_DEV_REF @ ${RT_SHA:0:9} -> $RUNTIME_DIR (dev override)"
        rm -rf "$RUNTIME_DIR" && mkdir -p "$RUNTIME_DIR"
        git -C "$RT_REPO" archive "$RT_SHA" | tar -x -C "$RUNTIME_DIR"
        echo "$RT_SHA" >"$RUNTIME_DIR/.dxr-runtime-sha"
    fi
fi
[ -f "$RUNTIME_DIR/CMakeLists.txt" ] || { echo "error: runtime checkout not found at $RUNTIME_DIR" >&2; exit 1; }

SR_ARGS=()
SR_RUNTIME_DYLIB=""
if [ -n "${SRSDK_ROOT:-}" ]; then
    SR_ARGS+=("-DSRSDK_ROOT=$SRSDK_ROOT")
    SR_RUNTIME_DYLIB="$SRSDK_ROOT/lib/libLeiaSR_runtime.dylib"
elif [ -f "$LEIASR_MACOS_DIR/build/macos/install/lib/libsrSDK_loader.a" ]; then
    SR_ARGS+=("-DSRSDK_INCLUDE_DIR=$LEIASR_MACOS_DIR/build/macos/install/include"
              "-DSRSDK_LOADER_LIBRARY=$LEIASR_MACOS_DIR/build/macos/install/lib/libsrSDK_loader.a")
    SR_RUNTIME_DYLIB="$LEIASR_MACOS_DIR/build/macos/install/lib/libLeiaSR_runtime.dylib"
elif [ -f "$LEIASR_MACOS_DIR/build/macos/sr/libsrSDK_loader.a" ]; then
    SR_ARGS+=("-DSRSDK_INCLUDE_DIR=$LEIASR_MACOS_DIR/modules/srSDK/include"
              "-DSRSDK_LOADER_LIBRARY=$LEIASR_MACOS_DIR/build/macos/sr/libsrSDK_loader.a")
    SR_RUNTIME_DYLIB="$LEIASR_MACOS_DIR/build/macos/sr/libLeiaSR_runtime.dylib"
else
    echo "error: no LeiaSR macOS SDK — set SRSDK_ROOT or LEIASR_MACOS_DIR" >&2
    exit 1
fi

CFG_ARGS=()
if [ "$BUILD_TYPE" = "Release" ]; then
    # A shipped dylib must not depend on Homebrew's libcjson (the installed
    # runtime does not ship one): hide it from EVERY find_package(cJSON) --
    # the runtime's and displayxr-mcp's -- so both compile their bundled copy.
    # (-DXRT_HAVE_SYSTEM_CJSON=OFF alone is not enough: the mcp dep still finds
    # Homebrew's cJSON and its headers then shadow the runtime's cJSON.c.)
    CFG_ARGS+=("-DCMAKE_DISABLE_FIND_PACKAGE_cJSON=ON" "-DDISPLAYXR_MCP_USE_VENDORED_CJSON=ON")
fi
if [ "$BUILD_TYPE" = "Release" ] && [ -z "${DXR_MACOS_DEPLOYMENT_TARGET+x}" ]; then
    DXR_MACOS_DEPLOYMENT_TARGET=14.0
fi
if [ -n "${DXR_MACOS_DEPLOYMENT_TARGET:-}" ]; then
    CFG_ARGS+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=$DXR_MACOS_DEPLOYMENT_TARGET")
fi

echo "==> Configuring $BUILD_TYPE (runtime: $RUNTIME_DIR)"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DDXR_RUNTIME_SOURCE_DIR="$RUNTIME_DIR" \
    "${SR_ARGS[@]}" ${CFG_ARGS[@]+"${CFG_ARGS[@]}"}

echo "==> Building plug-in + runtime dylib + displayxr-cli + sim-display (one tree)"
cmake --build "$BUILD_DIR" -j "${DXR_BUILD_JOBS:-4}" --target DisplayXR-LeiaSR openxr_displayxr cli drv_sim_display_plugin

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
if [ -f "\$HOME/.leiasr-fpc/env.local" ]; then
    . "\$HOME/.leiasr-fpc/env.local"
    export LEIASR_RUNTIME_DIR
    export DYLD_LIBRARY_PATH="\$DYLD_LIBRARY_PATH:\${LEIASR_DYLD:-}"
fi
# Set AFTER env.local, which may point SR_RUNTIME_PATH elsewhere.
export SR_RUNTIME_PATH="\${DXR_SR_RUNTIME_PATH:-$SR_RUNTIME_DYLIB}"
export LEIASR_DATA_DIR="\${DXR_LEIASR_DATA_DIR:-$LEIASR_MACOS_DIR/build/macos/sr}"
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

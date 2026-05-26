#!/usr/bin/env bash
# ============================================================
# DisplayXR Leia CNSDK Plug-in — Android Build Helper
# ============================================================
# Wraps the multi-arg cmake invocation needed to produce
# libdxrp050_leia_cnsdk.so for Android arm64-v8a. Without this
# script, a bring-up dev has to remember:
#   - NDK toolchain file path
#   - Ninja binary path inside the SDK's bundled cmake
#   - CNSDK_ROOT (extracted release tree, not source checkout)
#   - DXR_RUNTIME_SOURCE_DIR (sibling runtime checkout)
#   - Eigen3_DIR (gradle-fetched stub config)
#   - ABI + platform settings
# All of those drift across machines; this script auto-resolves
# them with sensible defaults and explicit env-var overrides.
#
# Usage: scripts/build-android.sh [target]
#   target = dxrp050_leia_cnsdk (default) — only the plug-in .so
#          = clean                       — wipe build-android/
#
# Required:
#   ANDROID_SDK_ROOT or ANDROID_HOME — Android SDK install dir
#                                      (must contain ndk/<ver>/ and
#                                      cmake/<ver>/bin/ninja.exe).
#
# Optional env (auto-detected with defaults):
#   ANDROID_NDK_VERSION   — NDK to use (default: 26.3.11579264)
#   CNSDK_ROOT            — extracted CNSDK 0.7.28 release tree
#                            (default: ../openxr-3d-display/cnsdk)
#   DXR_RUNTIME_SOURCE_DIR — local runtime checkout
#                            (default: ../openxr-3d-display)
#   EIGEN3_DIR            — Eigen3Config.cmake location. If unset,
#                            the script points at the gradle-fetched
#                            Eigen under the runtime checkout's
#                            openxr_android/build/intermediates/.
#                            That dir only exists AFTER a successful
#                            runtime APK build; if you're building
#                            the plug-in standalone (no runtime APK
#                            built yet), set EIGEN3_DIR explicitly.
#
# Output: build-android/src/drv_leia_android/libdxrp050_leia_cnsdk.so
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET="${1:-dxrp050_leia_cnsdk}"

# Resolve Android SDK
: "${ANDROID_SDK_ROOT:=${ANDROID_HOME:-}}"
if [ -z "${ANDROID_SDK_ROOT}" ] || [ ! -d "${ANDROID_SDK_ROOT}" ]; then
    echo "ERROR: ANDROID_SDK_ROOT not set or doesn't exist."
    echo "  Set ANDROID_SDK_ROOT (or ANDROID_HOME) to your Android SDK install dir."
    exit 1
fi
echo "ANDROID_SDK_ROOT: ${ANDROID_SDK_ROOT}"

# Resolve NDK
: "${ANDROID_NDK_VERSION:=26.3.11579264}"
NDK_DIR="${ANDROID_SDK_ROOT}/ndk/${ANDROID_NDK_VERSION}"
if [ ! -d "${NDK_DIR}" ]; then
    echo "ERROR: NDK ${ANDROID_NDK_VERSION} not installed at ${NDK_DIR}."
    echo "  Install via Android Studio SDK Manager or 'sdkmanager \"ndk;${ANDROID_NDK_VERSION}\"'."
    exit 1
fi
TOOLCHAIN_FILE="${NDK_DIR}/build/cmake/android.toolchain.cmake"
echo "NDK: ${NDK_DIR}"

# Resolve Ninja (bundled with Android SDK's cmake)
NINJA="$(find "${ANDROID_SDK_ROOT}/cmake" -name ninja.exe -o -name ninja 2>/dev/null | head -1)"
if [ -z "${NINJA}" ]; then
    echo "ERROR: ninja not found under ${ANDROID_SDK_ROOT}/cmake."
    echo "  Install via Android Studio SDK Manager (Tools > Settings > SDK > SDK Tools > CMake)."
    exit 1
fi
echo "Ninja:  ${NINJA}"

# Resolve runtime source dir
: "${DXR_RUNTIME_SOURCE_DIR:=${REPO}/../openxr-3d-display}"
if [ ! -f "${DXR_RUNTIME_SOURCE_DIR}/CMakeLists.txt" ]; then
    echo "ERROR: DXR_RUNTIME_SOURCE_DIR=${DXR_RUNTIME_SOURCE_DIR} doesn't look like a runtime checkout."
    echo "  Point it at a local clone of DisplayXR/displayxr-runtime."
    exit 1
fi
echo "Runtime: ${DXR_RUNTIME_SOURCE_DIR}"

# Resolve CNSDK
: "${CNSDK_ROOT:=${DXR_RUNTIME_SOURCE_DIR}/cnsdk}"
if [ ! -f "${CNSDK_ROOT}/share/cmake/CNSDK/CNSDKConfig.cmake" ]; then
    echo "ERROR: CNSDK_ROOT=${CNSDK_ROOT} missing share/cmake/CNSDK/CNSDKConfig.cmake."
    echo "  Extract CNSDK 0.7.28 release zip and set CNSDK_ROOT to the extracted dir."
    echo "  Source: https://github.com/LeiaInc/leiainc.github.io/tree/master/CNSDK/cnsdk-android-0.7.28.zip"
    exit 1
fi
echo "CNSDK:  ${CNSDK_ROOT}"

# Resolve Eigen3 dir
: "${EIGEN3_DIR:=${DXR_RUNTIME_SOURCE_DIR}/src/xrt/targets/openxr_android/build/intermediates/eigen/eigen-3.4.0/cmake}"
if [ ! -f "${EIGEN3_DIR}/Eigen3Config.cmake" ]; then
    echo "ERROR: EIGEN3_DIR=${EIGEN3_DIR} missing Eigen3Config.cmake."
    echo "  Either (a) build the runtime APK first to materialize the gradle-fetched Eigen at the default path, or"
    echo "         (b) set EIGEN3_DIR explicitly to a dir containing Eigen3Config.cmake."
    exit 1
fi
echo "Eigen3: ${EIGEN3_DIR}"

# Handle clean target
if [ "${TARGET}" = "clean" ]; then
    echo "Wiping build-android/"
    rm -rf "${REPO}/build-android"
    exit 0
fi

# Configure
cd "${REPO}"
if [ ! -f build-android/CMakeCache.txt ]; then
    echo
    echo "=== Configuring ==="
    cmake -S . -B build-android -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
        -DCMAKE_MAKE_PROGRAM="${NINJA}" \
        -DANDROID_ABI=arm64-v8a \
        -DANDROID_PLATFORM=android-29 \
        -DCNSDK_ROOT="${CNSDK_ROOT}" \
        -DDXR_RUNTIME_SOURCE_DIR="${DXR_RUNTIME_SOURCE_DIR}" \
        -DEigen3_DIR="${EIGEN3_DIR}"
fi

# Build
echo
echo "=== Building ${TARGET} ==="
cmake --build build-android --target "${TARGET}"

# Report
echo
SO=build-android/src/drv_leia_android/libdxrp050_leia_cnsdk.so
if [ -f "${SO}" ]; then
    echo "=== Built: $(pwd)/${SO} ==="
    ls -l "${SO}"
    echo
    echo "Drop into the runtime APK's jniLibs/<ABI>/:"
    echo "  cp ${SO} ${DXR_RUNTIME_SOURCE_DIR}/src/xrt/targets/openxr_android/src/main/jniLibs/arm64-v8a/"
fi

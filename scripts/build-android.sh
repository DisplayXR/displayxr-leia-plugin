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
#          = install-runtime-jnilibs     — build .so + copy plug-in
#                                            .so + CNSDK transitive
#                                            .so files into the
#                                            runtime APK's jniLibs/
#                                            (the runtime APK then
#                                             needs `assembleInProcessDebug --rerun-tasks`)
#
# Required (one of):
#   ANDROID_NDK_HOME / ANDROID_NDK_ROOT — a standalone NDK dir (must contain
#                            build/cmake/android.toolchain.cmake). This is what
#                            CI uses: nttld/setup-ndk installs the NDK OUTSIDE
#                            any SDK, so requiring an SDK would reject exactly
#                            the layout the runtime's own Android CI produces.
#   ANDROID_SDK_ROOT / ANDROID_HOME — an Android SDK install dir; the NDK is
#                            then taken from ndk/<ANDROID_NDK_VERSION>/.
#
# Optional env (auto-detected with defaults):
#   ANDROID_NDK_VERSION   — NDK to use (default: 26.3.11579264). Must match the
#                            NDK the runtime APK is built with, or the plug-in
#                            links against a different libc++/sysroot than the
#                            APK it gets dropped into.
#   CNSDK_ROOT            — extracted CNSDK 0.10.54+ release tree
#                            (default: <runtime checkout>/cnsdk)
#   DXR_RUNTIME_SOURCE_DIR — local runtime checkout
#                            (default: ../displayxr-runtime; legacy
#                             ../openxr-3d-display probed as a fallback)
#   EIGEN3_DIR            — Eigen3Config.cmake location. If unset, a probe list
#                            is tried: distro/homebrew locations first, then
#                            the gradle-fetched Eigen under the runtime
#                            checkout. Eigen is HEADER-ONLY, so a host package
#                            cross-compiles to Android fine — which is why the
#                            distro path is preferred and the old "build the
#                            runtime APK first" precondition is gone.
#   NINJA                 — ninja binary. Default: search the SDK's cmake/ dir,
#                            then PATH. CI has /usr/bin/ninja and no SDK.
#   CMAKE_BUILD_TYPE      — default Debug, PINNED, see #195. RelWithDebInfo
#                            SIGSEGVs on device (vulkan.adreno.so, first
#                            vkCreateRenderPass). Do NOT raise this default
#                            until #195 is fixed.
#   ANDROID_ABI           — default arm64-v8a.
#   ANDROID_PLATFORM      — default android-29 (matches the runtime's minSdk).
#   ANDROID_STL           — default c++_static. Pinned deliberately: a shared
#                            STL would add libc++_shared.so to DT_NEEDED, i.e.
#                            drop an extra .so into someone else's APK.
#
# Output: build-android/src/drv_leia_android/libdxrp050_leia_cnsdk.so
# ============================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET="${1:-dxrp050_leia_cnsdk}"

# Resolve the NDK.
#
# Two supported shapes, explicit NDK first. nttld/setup-ndk (what CI and the
# runtime's own build-android.yml use) installs a STANDALONE NDK outside any
# SDK, so insisting on ANDROID_SDK_ROOT would reject the one layout CI can
# actually produce. Dev boxes with Android Studio keep working unchanged.
: "${ANDROID_NDK_VERSION:=26.3.11579264}"
NDK_DIR=""
for _cand in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}"; do
    if [ -n "${_cand}" ] && [ -f "${_cand}/build/cmake/android.toolchain.cmake" ]; then
        NDK_DIR="${_cand}"
        break
    fi
done
if [ -z "${NDK_DIR}" ]; then
    : "${ANDROID_SDK_ROOT:=${ANDROID_HOME:-}}"
    if [ -z "${ANDROID_SDK_ROOT}" ] || [ ! -d "${ANDROID_SDK_ROOT}" ]; then
        echo "ERROR: no usable NDK."
        echo "  Set ANDROID_NDK_HOME (or ANDROID_NDK_ROOT) to a standalone NDK, or"
        echo "  set ANDROID_SDK_ROOT (or ANDROID_HOME) to an Android SDK containing"
        echo "  ndk/${ANDROID_NDK_VERSION}/."
        exit 1
    fi
    echo "ANDROID_SDK_ROOT: ${ANDROID_SDK_ROOT}"
    NDK_DIR="${ANDROID_SDK_ROOT}/ndk/${ANDROID_NDK_VERSION}"
fi
TOOLCHAIN_FILE="${NDK_DIR}/build/cmake/android.toolchain.cmake"
# Fail at RESOLUTION with a clear message, not 200 lines into a CMake trace.
if [ ! -f "${TOOLCHAIN_FILE}" ]; then
    echo "ERROR: ${TOOLCHAIN_FILE} not found."
    echo "  NDK_DIR resolved to '${NDK_DIR}', which does not look like an NDK."
    echo "  Install via 'sdkmanager \"ndk;${ANDROID_NDK_VERSION}\"' or point"
    echo "  ANDROID_NDK_HOME at a standalone NDK."
    exit 1
fi
echo "NDK: ${NDK_DIR}"

# Resolve Ninja. Explicit override, then the SDK's bundled cmake, then PATH.
# CI installs ninja-build from apt and has no ${ANDROID_SDK_ROOT}/cmake at all,
# which the old SDK-only search treated as a fatal error.
if [ -z "${NINJA:-}" ] && [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -d "${ANDROID_SDK_ROOT}/cmake" ]; then
    NINJA="$(find "${ANDROID_SDK_ROOT}/cmake" -name ninja.exe -o -name ninja 2>/dev/null | head -1)"
fi
: "${NINJA:=$(command -v ninja || true)}"
if [ -z "${NINJA}" ] || [ ! -x "${NINJA}" ]; then
    echo "ERROR: ninja not found."
    echo "  Set NINJA=/path/to/ninja, install it (apt install ninja-build /"
    echo "  brew install ninja), or install CMake via the Android SDK Manager."
    exit 1
fi
echo "Ninja:  ${NINJA}"

# Resolve runtime source dir — probe current + legacy sibling clone names.
# "displayxr-runtime" is the repo name today; "openxr-3d-display" is a legacy
# local clone name kept as a fallback. Final default is the current name.
if [ -z "${DXR_RUNTIME_SOURCE_DIR:-}" ]; then
    for _d in displayxr-runtime openxr-3d-display; do
        if [ -f "${REPO}/../${_d}/CMakeLists.txt" ]; then
            DXR_RUNTIME_SOURCE_DIR="${REPO}/../${_d}"
            break
        fi
    done
    : "${DXR_RUNTIME_SOURCE_DIR:=${REPO}/../displayxr-runtime}"
fi
if [ ! -f "${DXR_RUNTIME_SOURCE_DIR}/CMakeLists.txt" ]; then
    echo "ERROR: DXR_RUNTIME_SOURCE_DIR=${DXR_RUNTIME_SOURCE_DIR} doesn't look like a runtime checkout."
    echo "  Point it at a local clone of DisplayXR/displayxr-runtime."
    exit 1
fi
echo "Runtime: ${DXR_RUNTIME_SOURCE_DIR}"

# Resolve CNSDK.
#
# CNSDK is BYO: the plug-in links its loader shim (CNSDK::leiaCore ->
# libleiaCore-loader.so), which at runtime dlopens libleiaCore-impl.so out of
# the ON-DEVICE package. So this is a build-time dependency only, the same way
# the Windows arm build-depends on the SR SDK while the SR runtime is installed
# separately. Never redistribute the SDK itself.
: "${CNSDK_ROOT:=${DXR_RUNTIME_SOURCE_DIR}/cnsdk}"
if [ ! -f "${CNSDK_ROOT}/share/cmake/CNSDK/CNSDKConfig.cmake" ]; then
    echo "ERROR: CNSDK_ROOT=${CNSDK_ROOT} missing share/cmake/CNSDK/CNSDKConfig.cmake."
    echo "  Extract a CNSDK 0.10.54+ android release tree and set CNSDK_ROOT to it."
    echo "  Source (private; needs LeiaInc org read access):"
    echo "    gh release download <tag> -R LeiaInc/CNSDK -p 'cnsdk-android-*.zip'"
    echo "  NOTE: the public leiainc.github.io copy is 0.7.28 and NO LONGER WORKS —"
    echo "  0.10.x moved to the loader architecture this plug-in compiles against."
    exit 1
fi
if [ -f "${CNSDK_ROOT}/VERSION.txt" ]; then
    echo "CNSDK:  ${CNSDK_ROOT} ($(tr -d '[:space:]' < "${CNSDK_ROOT}/VERSION.txt"))"
else
    echo "CNSDK:  ${CNSDK_ROOT}"
fi

# Resolve Eigen3 dir.
#
# Eigen is HEADER-ONLY, so a host package cross-compiles to Android fine; the
# distro/homebrew configs are tried before the gradle-fetched copy so building
# the plug-in no longer requires having built the runtime APK first. An explicit
# Eigen3_DIR is also what makes find_package work at all under the NDK
# toolchain, which sets CMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY.
if [ -z "${EIGEN3_DIR:-}" ]; then
    for _e in /usr/share/eigen3/cmake \
              /usr/local/share/eigen3/cmake \
              /opt/homebrew/share/eigen3/cmake \
              "${DXR_RUNTIME_SOURCE_DIR}/src/xrt/targets/openxr_android/build/intermediates/eigen/eigen-3.4.0/cmake"; do
        if [ -f "${_e}/Eigen3Config.cmake" ]; then
            EIGEN3_DIR="${_e}"
            break
        fi
    done
fi
if [ -z "${EIGEN3_DIR:-}" ] || [ ! -f "${EIGEN3_DIR}/Eigen3Config.cmake" ]; then
    echo "ERROR: no Eigen3Config.cmake found."
    echo "  Install Eigen (apt install libeigen3-dev / brew install eigen), or"
    echo "  set EIGEN3_DIR explicitly to a dir containing Eigen3Config.cmake."
    exit 1
fi
echo "Eigen3: ${EIGEN3_DIR}"

# Handle clean target
if [ "${TARGET}" = "clean" ]; then
    echo "Wiping build-android/"
    rm -rf "${REPO}/build-android"
    exit 0
fi

# install-runtime-jnilibs: build the plug-in .so AND copy it (plus
# the CNSDK transitive .so deps) into the runtime APK's jniLibs/.
# Internally just delegates the build step to the default target,
# then does the copies.
INSTALL_JNILIBS=false
if [ "${TARGET}" = "install-runtime-jnilibs" ]; then
    INSTALL_JNILIBS=true
    TARGET=dxrp050_leia_cnsdk
fi

# Configure
cd "${REPO}"
if [ ! -f build-android/CMakeCache.txt ]; then
    echo
    echo "=== Configuring ==="
    # ANDROID_STL is pinned, not left to the NDK default: a shared STL adds
    # libc++_shared.so to DT_NEEDED, i.e. drops an extra .so into whichever APK
    # this plug-in is bundled into. --exclude-libs,ALL (see the drv_leia_android
    # CMakeLists) already keeps the static STL out of .dynsym.
    #
    # CMAKE_BUILD_TYPE IS PINNED TO Debug ON PURPOSE (#195). This script passed
    # NO build type at all until 2026-08-26, so every Android plug-in ever built
    # was effectively -O0 / no NDEBUG / no --gc-sections, and ALL on-device
    # validation this plug-in has had was in that configuration. Setting it to
    # RelWithDebInfo -- correct in principle, a shipped .so should not be -O0 --
    # immediately exposed a latent defect: SIGSEGV in vulkan.adreno.so at the
    # first vkCreateRenderPass (ensure_weave_rp_and_depth), on the first
    # weave-ready frame, reproduced in-process AND out-of-process on LPD-20W.
    #
    # -Wl,--no-gc-sections does NOT help -- measured, so section GC is
    # exonerated and the defect lives in the -O2/-DNDEBUG codegen half (UB, most
    # likely upstream of the fault site given inlining).
    #
    # So Debug is the status quo ante and the only configuration verified
    # weaving on hardware. DO NOT raise this default to fix the "ships
    # unoptimised" wart until #195 is closed: an unoptimised weaver is a much
    # cheaper problem than one that crashes every weave client at startup.
    cmake -S . -B build-android -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
        -DCMAKE_MAKE_PROGRAM="${NINJA}" \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}" \
        -DANDROID_ABI="${ANDROID_ABI:-arm64-v8a}" \
        -DANDROID_PLATFORM="${ANDROID_PLATFORM:-android-29}" \
        -DANDROID_STL="${ANDROID_STL:-c++_static}" \
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
# HARD failure, not `exit 0`. This used to report success when the .so was
# missing, which behind CI turns a broken build green.
if [ ! -f "${SO}" ]; then
    echo "ERROR: build reported success but ${SO} was not produced."
    exit 1
fi
echo "=== Built: $(pwd)/${SO} ==="
ls -l "${SO}"

# Install into runtime APK's jniLibs/ when requested.
if [ "${INSTALL_JNILIBS}" = "true" ]; then
    JNI_DIR="${DXR_RUNTIME_SOURCE_DIR}/src/xrt/targets/openxr_android/src/main/jniLibs/arm64-v8a"
    mkdir -p "${JNI_DIR}"
    # Clean stale .so so a variant switch (e.g. faceTrackingInApp →
    # faceTrackingService) doesn't leave the old in-app engine + SNPE libs
    # lingering in the runtime APK.
    rm -f "${JNI_DIR}"/*.so
    echo
    echo "=== Installing into ${JNI_DIR} ==="

    # Plug-in .so itself.
    cp -f "${SO}" "${JNI_DIR}/"
    echo "  + $(basename "${SO}")"

    # CNSDK 0.10.x transitive .so deps. The loader (libleiaCore-loader.so) is
    # what the plug-in links/dlopens; libleiaSDK-jni.so is the Java<->native
    # bridge the loader/Java side dlopens to reach the on-device Leia service.
    # Both ship in the SDK's lib/<ABI>/. Face tracking runs IN the device's
    # licensed service (selected via set_face_tracking_runtime(IN_SERVICE)),
    # so none of the 0.7.28 in-app engine libs (blink/license_utils/SNPE) are
    # needed. Matching the device's CNSDK 0.10.54 is what makes the service
    # connection succeed.
    CNSDK_LIB_DIR="${CNSDK_ROOT}/lib/arm64-v8a"
    for libname in libleiaCore-loader.so libleiaSDK-jni.so; do
        if [ -f "${CNSDK_LIB_DIR}/${libname}" ]; then
            cp -f "${CNSDK_LIB_DIR}/${libname}" "${JNI_DIR}/"
            echo "  + ${libname}"
        else
            echo "WARNING: ${CNSDK_LIB_DIR}/${libname} not found — plug-in will fail to dlopen on device. Is CNSDK_ROOT a 0.10.x SDK?"
        fi
    done

    # NOTE: SNPE (Qualcomm's DSP inference engine) is intentionally NOT
    # bundled — it was only needed by the in-app face-tracking engine
    # (libblink). With the faceTrackingInService variant, inference runs in
    # the device's Leia system service, so the app/runtime needs none of it.

    echo
    echo "Now build the runtime APK with the jniLibs picked up:"
    echo "  cd ${DXR_RUNTIME_SOURCE_DIR} && ./gradlew :src:xrt:targets:openxr_android:assembleInProcessDebug --rerun-tasks"
else
    echo
    echo "Drop into the runtime APK's jniLibs/<ABI>/ (or re-run this script with"
    echo "the 'install-runtime-jnilibs' target to do this + CNSDK transitive .so deps in one step):"
    echo "  cp ${SO} ${DXR_RUNTIME_SOURCE_DIR}/src/xrt/targets/openxr_android/src/main/jniLibs/arm64-v8a/"
fi

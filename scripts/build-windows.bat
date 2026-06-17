@echo off
setlocal enabledelayedexpansion

:: ============================================================
:: DisplayXR Leia SR Plug-in — Local Windows Build Script
:: ============================================================
:: Usage: scripts\build-windows.bat [generate|build|installer|all] [config]
::   generate   - CMake configure only
::   build      - Build the plug-in DLL + install
::   installer  - Build the NSIS installer (depends on build)
::   all        - Everything (default)
::   config     - Release (default) | RelWithDebInfo | Debug. Use RelWithDebInfo
::                to debug the plug-in (PDBs + Release CRT for the SR SDK).
::
:: Required environment (auto-fetched if missing):
::   LEIASR_SDKROOT       — extracted LeiaSR-SDK-*-win64 dir
::
:: Optional environment:
::   DXR_RUNTIME_SOURCE_DIR — path to a local displayxr-runtime checkout.
::                            If unset, probes the sibling ..\displayxr-runtime
::                            (and legacy ..\openxr-3d-display) automatically.
::                            If none is found, the plug-in's CMake falls back
::                            to FetchContent against the displayxr-runtime tag
::                            pinned in CMakeLists.txt (slow first build).
::
:: Runtime-side build deps (vcpkg, Vulkan SDK, OpenXR loader, VS 2022,
:: ninja, gh) must be installed and configured the same way as for the
:: runtime build — this script assumes the user can already do
:: `scripts\build_windows.bat all` in the runtime checkout. See the
:: runtime's docs/getting-started/building.md for the install matrix.
:: ============================================================

set SCRIPT_DIR=%~dp0
set REPO=%SCRIPT_DIR%..\
set SR_TAG=1.35.0.2011
set TARGET=%~1
if "%TARGET%"=="" set TARGET=all

REM Build config (2nd arg). Default Release. Use RelWithDebInfo to debug the
REM plug-in: it gives PDBs while keeping the Release CRT the SR SDK requires
REM (the SR SDK ships Release-only, so a pure Debug plug-in CRT-mismatches it).
set CONFIG=%~2
if "%CONFIG%"=="" set CONFIG=Release

:: --- Resolve runtime source dir ---
:: Probe the sibling runtime checkout by its known directory names. The repo is
:: "displayxr-runtime" today; "openxr-3d-display" is the legacy clone name kept
:: as a fallback. Without this, DXR_RUNTIME_SOURCE_DIR stays empty, the
:: vcpkg-toolchain block below is skipped, and the runtime's find_package
:: (Eigen3) fails ("Could not find a package configuration file ... Eigen3").
if "%DXR_RUNTIME_SOURCE_DIR%"=="" (
    for %%D in (displayxr-runtime openxr-3d-display) do (
        if not defined DXR_RUNTIME_SOURCE_DIR if exist "%REPO%..\%%D\CMakeLists.txt" (
            set DXR_RUNTIME_SOURCE_DIR=%REPO%..\%%D
            echo Detected local runtime at !DXR_RUNTIME_SOURCE_DIR!
        )
    )
    if not defined DXR_RUNTIME_SOURCE_DIR (
        echo Local runtime checkout not found next to %REPO% ^(displayxr-runtime / openxr-3d-display^)
        echo CMake will FetchContent the runtime from GitHub ^(slow first build^).
    )
)

:: --- MSVC env ---
:: Locate any VS 2022 edition via vswhere (the official MS way) so Community /
:: Professional / Enterprise / BuildTools all work without hand-editing. Falls
:: back to probing the known editions if vswhere is absent.
echo === Setting up MSVC environment ===
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if not defined VCVARS if exist "C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS (
    echo ERROR: VS 2022 with the C++ workload not found.
    exit /b 1
)
call "%VCVARS%" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: vcvars64.bat failed: %VCVARS%
    exit /b 1
)

:: --- SR SDK ---
if "%LEIASR_SDKROOT%"=="" (
    set LEIASR_SDKROOT=%REPO%LeiaSR-SDK-%SR_TAG%-win64
)
set SR_SDK_MARKER=%LEIASR_SDKROOT%\lib\cmake\srDirectX\srDirectXConfig.cmake
set REPO_NOSLASH=%REPO:~0,-1%

if not exist "%SR_SDK_MARKER%" (
    echo === Downloading Leia SR SDK %SR_TAG% from this repo's release ===
    gh release download sr-sdk-v%SR_TAG% -R DisplayXR/displayxr-leia-plugin -p "LeiaSR-SDK-%SR_TAG%-win64.zip" -D "%REPO_NOSLASH%"
    if %ERRORLEVEL% NEQ 0 (
        echo ERROR: Failed to download SR SDK. Run: gh auth login
        exit /b 1
    )
    powershell -Command "Expand-Archive -Path '%REPO%LeiaSR-SDK-%SR_TAG%-win64.zip' -DestinationPath '%REPO_NOSLASH%' -Force"
    if not exist "%SR_SDK_MARKER%" (
        echo ERROR: SR SDK extract did not produce %SR_SDK_MARKER%
        exit /b 1
    )
    del "%REPO%LeiaSR-SDK-%SR_TAG%-win64.zip" 2>nul

    echo === Downloading Vulkan weaver extras ===
    gh release download sr-sdk-v%SR_TAG% -R DisplayXR/displayxr-leia-plugin -p "SimulatedRealityVulkanBeta.lib" -D "%LEIASR_SDKROOT%\lib"
    gh release download sr-sdk-v%SR_TAG% -R DisplayXR/displayxr-leia-plugin -p "vkweaver.h" -D "%LEIASR_SDKROOT%\include\sr\weaver"
    gh release download sr-sdk-v%SR_TAG% -R DisplayXR/displayxr-leia-plugin -p "SimulatedRealityVulkanBeta.dll" -D "%LEIASR_SDKROOT%\bin"
    echo SR SDK ready.
)

echo.
echo === Dependencies ready ===
echo   LEIASR_SDKROOT=%LEIASR_SDKROOT%
echo   DXR_RUNTIME_SOURCE_DIR=%DXR_RUNTIME_SOURCE_DIR%
echo.

:: --- Skip-to targets ---
if "%TARGET%"=="build" if exist "%REPO%build\build.ninja" goto :do_build
if "%TARGET%"=="installer" if exist "%REPO%build\build.ninja" goto :do_installer

:: ============================================================
:: CMake Generate
:: ============================================================
:: The plug-in's CMake add_subdirectory(runtime) triggers the runtime's
:: own CMake — which needs vcpkg (for Eigen3 etc.), Vulkan SDK, and the
:: OpenXR loader. We inherit those from the runtime sibling's already-
:: built dev tree (the user's `scripts\build_windows.bat all` has set
:: them up). Required env: VULKAN_SDK pointing at the installed SDK.
echo === CMake Generate ===
set CMAKE_ARGS=-S "%REPO%." -B "%REPO%build" -G "Ninja Multi-Config" ^
  -DCMAKE_PREFIX_PATH="%LEIASR_SDKROOT%" ^
  -DCMAKE_INSTALL_PREFIX="%REPO%_package"

if not "%DXR_RUNTIME_SOURCE_DIR%"=="" (
    set CMAKE_ARGS=!CMAKE_ARGS! -DDXR_RUNTIME_SOURCE_DIR="%DXR_RUNTIME_SOURCE_DIR%"

    REM Inherit the runtime sibling's vcpkg toolchain + OpenXR loader
    REM so add_subdirectory of the runtime resolves Eigen3, cjson,
    REM OpenXR, etc. matching the runtime's own build script settings.
    if exist "%DXR_RUNTIME_SOURCE_DIR%\vcpkg\scripts\buildsystems\vcpkg.cmake" (
        set CMAKE_ARGS=!CMAKE_ARGS! -DCMAKE_TOOLCHAIN_FILE="%DXR_RUNTIME_SOURCE_DIR%\vcpkg\scripts\buildsystems\vcpkg.cmake" -DX_VCPKG_APPLOCAL_DEPS_INSTALL=ON -DVCPKG_MANIFEST_DIR="%DXR_RUNTIME_SOURCE_DIR%"
        echo Using runtime sibling's vcpkg toolchain + vcpkg.json.
    ) else (
        echo WARN: runtime vcpkg not found at %DXR_RUNTIME_SOURCE_DIR%\vcpkg
        echo       Run scripts\build_windows.bat all in the runtime first.
    )

    if exist "%DXR_RUNTIME_SOURCE_DIR%\openxr_sdk\x64\lib\openxr_loader.lib" (
        set CMAKE_ARGS=!CMAKE_ARGS! -DOpenXR_ROOT="%DXR_RUNTIME_SOURCE_DIR%\openxr_sdk"
    )
)

cmake !CMAKE_ARGS!
if %ERRORLEVEL% NEQ 0 (
    echo CMake generate FAILED
    exit /b 1
)
if "%TARGET%"=="generate" goto :end

:do_build
echo === Build ===
cmake --build "%REPO%build" --config !CONFIG! --target install
if %ERRORLEVEL% NEQ 0 (
    echo Build FAILED
    exit /b 1
)
if "%TARGET%"=="build" goto :end

:do_installer
echo === Build Installer ===
cmake --build "%REPO%build" --config !CONFIG! --target installer
if %ERRORLEVEL% NEQ 0 (
    echo Installer build FAILED
    exit /b 1
)

:end
echo.
echo === Done ===
echo Outputs:
echo   Plug-in DLL:  %REPO%_package\bin\plugins\DisplayXR-LeiaSR.dll
echo   Installer:    %REPO%_package\DisplayXRLeiaSRSetup-*.exe
endlocal

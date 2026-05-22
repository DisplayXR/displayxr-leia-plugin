@echo off
setlocal enabledelayedexpansion

:: ============================================================
:: DisplayXR Leia SR Plug-in — Local Windows Build Script
:: ============================================================
:: Usage: scripts\build-windows.bat [generate|build|installer|all]
::   generate   - CMake configure only
::   build      - Build the plug-in DLL + install
::   installer  - Build the NSIS installer (depends on build)
::   all        - Everything (default)
::
:: Required environment (auto-fetched if missing):
::   LEIASR_SDKROOT       — extracted LeiaSR-SDK-*-win64 dir
::
:: Optional environment:
::   DXR_RUNTIME_SOURCE_DIR — path to a local displayxr-runtime checkout.
::                            Defaults to ..\openxr-3d-display.
::                            If unset and the default isn't found, the
::                            plug-in's CMake falls back to FetchContent
::                            against the displayxr-runtime tag pinned in
::                            CMakeLists.txt (slow first build).
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

:: --- Resolve runtime source dir ---
if "%DXR_RUNTIME_SOURCE_DIR%"=="" (
    if exist "%REPO%..\openxr-3d-display\CMakeLists.txt" (
        set DXR_RUNTIME_SOURCE_DIR=%REPO%..\openxr-3d-display
        echo Detected local runtime at !DXR_RUNTIME_SOURCE_DIR!
    ) else (
        echo Local runtime checkout not found at %REPO%..\openxr-3d-display
        echo CMake will FetchContent the runtime from GitHub ^(slow first build^).
    )
)

:: --- MSVC env ---
echo === Setting up MSVC environment ===
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: VS 2022 not found.
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
cmake --build "%REPO%build" --config Release --target install
if %ERRORLEVEL% NEQ 0 (
    echo Build FAILED
    exit /b 1
)
if "%TARGET%"=="build" goto :end

:do_installer
echo === Build Installer ===
cmake --build "%REPO%build" --config Release --target installer
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

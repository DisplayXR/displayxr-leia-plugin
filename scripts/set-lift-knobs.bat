@echo off
setlocal

:: ============================================================
:: Set the Leia lift demo knobs in the registry (run ELEVATED).
:: ============================================================
:: Usage: scripts\set-lift-knobs.bat           write Backend=directml, InteractiveMin=0.4.4
::        scripts\set-lift-knobs.bat --clear   delete HKLM\SOFTWARE\DisplayXR\Leia\Lift
::
:: The lift reads each knob from the env var first (DXR_LEIA_LIFT_*), else from
:: this key, else its default. The registry survives displayxr-service.exe
:: respawns (tray relaunch, HKLM Run at logon, crash restart), which start with
:: the logon environment and would otherwise drop env-only knobs silently.
:: InteractiveMin=0.4.4 is ONLY for the NeurD 0.4.4 internal-interactive package;
:: on a stock NeurD 0.4.4 it crashes the service. See docs\lift-neurd.md.
:: Restart displayxr-service.exe afterwards for the change to take effect.

set "KEY=HKLM\SOFTWARE\DisplayXR\Leia\Lift"

net session >nul 2>&1
if errorlevel 1 (
    echo ERROR: run this from an elevated ^(Administrator^) prompt.
    exit /b 1
)

if /i "%~1"=="--clear" (
    reg query "%KEY%" /reg:64 >nul 2>&1
    if errorlevel 1 (
        echo %KEY% not present - nothing to clear.
        exit /b 0
    )
    reg delete "%KEY%" /f /reg:64 || exit /b 1
    echo Deleted %KEY%.
    exit /b 0
)

if not "%~1"=="" (
    echo Usage: %~nx0 [--clear]
    exit /b 2
)

reg add "%KEY%" /v Backend /t REG_SZ /d directml /f /reg:64 || exit /b 1
reg add "%KEY%" /v InteractiveMin /t REG_SZ /d 0.4.4 /f /reg:64 || exit /b 1
echo Wrote %KEY%: Backend=directml InteractiveMin=0.4.4
reg query "%KEY%" /reg:64
echo Restart displayxr-service.exe for the lift to pick these up.
exit /b 0

@echo off
rem DUGco AlphaGenSAM - installer for Nuke (Windows).
rem Copies the OFX plug-in to %LOCALAPPDATA%\DUGco\OFX and adds that folder to your
rem OFX_PLUGIN_PATH (your user account only; no admin rights needed). Restart Nuke after.
setlocal EnableDelayedExpansion
set "HERE=%~dp0"
set "DEST=%LOCALAPPDATA%\DUGco\OFX"
set "BUNDLE_DIR=%HERE%"

rem Double-clicked inside the zip? Windows then runs this file alone from a temp folder.
if /i not "%HERE:.zip\=%"=="%HERE%" (
    echo.
    echo  This installer is being run from INSIDE the zip file.
    echo  Please unzip it first: right-click the zip, choose "Extract All...",
    echo  then open the extracted folder and double-click install.bat there.
    echo.
    pause
    exit /b 1
)

if not exist "%BUNDLE_DIR%AlphaGenSAM.ofx.bundle\Contents\Win64\AlphaGenSAM.ofx" (
    rem Run from a clone of the repository: use a local build if there is one ...
    for %%D in ("%HERE%..\build\" "%HERE%..\staging\") do (
        if exist "%%~DAlphaGenSAM.ofx.bundle\Contents\Win64\AlphaGenSAM.ofx" if not defined FOUND (
            set "BUNDLE_DIR=%%~D"
            set "FOUND=1"
            echo Using the plug-in built in %%~D
        )
    )
)
if not exist "%BUNDLE_DIR%AlphaGenSAM.ofx.bundle\Contents\Win64\AlphaGenSAM.ofx" (
    rem ... otherwise offer to download the release
    echo.
    echo  The plug-in isn't in this folder. That happens when you download the source code
    echo  ^(git clone, or "Code - Download ZIP" on GitHub^) instead of the release zip.
    echo.
    if not exist "%HERE%fetch_release.ps1" (
        echo  Get the ready-to-install zip from:
        echo    https://github.com/dughogan/nuke-alphagen-ofx/releases
        pause
        exit /b 1
    )
    choice /c YN /m " Download the latest release from GitHub and install it now"
    if errorlevel 2 (
        echo  Get it from https://github.com/dughogan/nuke-alphagen-ofx/releases
        pause
        exit /b 1
    )
    set "GOT="
    for /f "usebackq delims=" %%L in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%fetch_release.ps1"`) do set "GOT=%%L"
    if not defined GOT (
        echo  Download failed. Get it from https://github.com/dughogan/nuke-alphagen-ofx/releases
        pause
        exit /b 1
    )
    set "BUNDLE_DIR=!GOT!\"
    if not exist "!BUNDLE_DIR!AlphaGenSAM.ofx.bundle\Contents\Win64\AlphaGenSAM.ofx" (
        echo  Download failed. Get it from https://github.com/dughogan/nuke-alphagen-ofx/releases
        pause
        exit /b 1
    )
)

tasklist /fi "imagename eq Nuke*" 2>nul | find /i "Nuke" >nul && (
    echo Please close Nuke first, then run this again.
    pause
    exit /b 1
)
echo Installing DUGco AlphaGenSAM to %DEST% ...
if not exist "%DEST%" mkdir "%DEST%"
robocopy "%BUNDLE_DIR%AlphaGenSAM.ofx.bundle" "%DEST%\AlphaGenSAM.ofx.bundle" /MIR /NJH /NJS /NFL /NDL >nul
if errorlevel 8 (
    echo Copy failed.
    pause
    exit /b 1
)
rem add DEST to the user's OFX_PLUGIN_PATH once (PowerShell keeps long values intact)
powershell -NoProfile -Command ^
  "$d = '%DEST%'; $v = [Environment]::GetEnvironmentVariable('OFX_PLUGIN_PATH','User');" ^
  "$parts = @($v -split ';' | Where-Object { $_ });" ^
  "if ($parts -notcontains $d) { [Environment]::SetEnvironmentVariable('OFX_PLUGIN_PATH', (($parts + $d) -join ';'), 'User'); 'Added to OFX_PLUGIN_PATH.' } else { 'OFX_PLUGIN_PATH already set.' }"
echo.
echo Done. Start Nuke and find it under the DUGco menu (Nodes toolbar or Tab: "AlphaGenSAM").
echo.
echo Using Comfy Cloud (the default): open the node's "ComfyUI Backend" section, press
echo "Get an API Key", create a key on the Comfy platform, then press "Set API Key..." and paste it.
echo.
pause

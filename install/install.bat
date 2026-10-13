@echo off
rem DUGco AlphaGenSAM - installer for Nuke (Windows).
rem Copies the OFX plug-in to %LOCALAPPDATA%\DUGco\OFX and adds that folder to your
rem OFX_PLUGIN_PATH (your user account only; no admin rights needed). Restart Nuke after.
setlocal
set "HERE=%~dp0"
set "DEST=%LOCALAPPDATA%\DUGco\OFX"
if not exist "%HERE%AlphaGenSAM.ofx.bundle\Contents\Win64\AlphaGenSAM.ofx" (
    echo Can't find AlphaGenSAM.ofx.bundle next to this installer.
    pause
    exit /b 1
)
tasklist /fi "imagename eq Nuke*" 2>nul | find /i "Nuke" >nul && (
    echo Please close Nuke first, then run this again.
    pause
    exit /b 1
)
echo Installing DUGco AlphaGenSAM to %DEST% ...
if not exist "%DEST%" mkdir "%DEST%"
robocopy "%HERE%AlphaGenSAM.ofx.bundle" "%DEST%\AlphaGenSAM.ofx.bundle" /MIR /NJH /NJS /NFL /NDL >nul
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

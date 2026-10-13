@echo off
rem DUGco AlphaGenSAM - removes the plug-in and its OFX_PLUGIN_PATH entry.
rem Your API key (Documents\DUGco\comfy_cloud_api_key.txt) is left alone; delete it yourself if you want.
setlocal
set "DEST=%LOCALAPPDATA%\DUGco\OFX"
tasklist /fi "imagename eq Nuke*" 2>nul | find /i "Nuke" >nul && (
    echo Please close Nuke first, then run this again.
    pause
    exit /b 1
)
if exist "%DEST%\AlphaGenSAM.ofx.bundle" rmdir /s /q "%DEST%\AlphaGenSAM.ofx.bundle"
powershell -NoProfile -Command ^
  "$d = '%DEST%'; $v = [Environment]::GetEnvironmentVariable('OFX_PLUGIN_PATH','User');" ^
  "$parts = @($v -split ';' | Where-Object { $_ -and $_ -ne $d });" ^
  "[Environment]::SetEnvironmentVariable('OFX_PLUGIN_PATH', $(if ($parts) { $parts -join ';' } else { $null }), 'User'); 'Removed.'"
echo DUGco AlphaGenSAM removed. Restart Nuke.
pause

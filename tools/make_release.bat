@echo off
rem Packs a release folder + zip: the plug-in bundle, install/uninstall scripts and the README.
rem   tools\make_release.bat 0.1.0
setlocal
set "ROOT=%~dp0.."
set "VER=%~1"
if "%VER%"=="" set "VER=dev"
set "NAME=DUGco_AlphaGenSAM_%VER%_win64"
set "OUT=%ROOT%\dist\%NAME%"
call "%ROOT%\build.bat" staging || exit /b 1
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"
robocopy "%ROOT%\staging\AlphaGenSAM.ofx.bundle" "%OUT%\AlphaGenSAM.ofx.bundle" /MIR /NJH /NJS /NFL /NDL >nul
copy /y "%ROOT%\install\install.bat" "%OUT%\" >nul
copy /y "%ROOT%\install\uninstall.bat" "%OUT%\" >nul
copy /y "%ROOT%\README.md" "%OUT%\" >nul
copy /y "%ROOT%\LICENSE" "%OUT%\" >nul
mkdir "%OUT%\example"
copy /y "%ROOT%\example\diner_plate.mp4" "%OUT%\example\" >nul
copy /y "%ROOT%\example\dugco_alphagensam_example.nk" "%OUT%\example\" >nul
powershell -NoProfile -Command "Compress-Archive -Force -Path '%OUT%\*' -DestinationPath '%ROOT%\dist\%NAME%.zip'"
echo Release: %ROOT%\dist\%NAME%.zip

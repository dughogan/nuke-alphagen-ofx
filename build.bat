@echo off
rem Builds AlphaGenSAM.ofx.bundle (and the headless selftest) with the VS 2022 Build Tools (x64).
setlocal
set ROOT=%~dp0
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set OUT=%ROOT%build\AlphaGenSAM.ofx.bundle\Contents\Win64
rem "build.bat staging" builds the plug-in elsewhere (while Nuke holds the real one open)
rem (outside build\, which is on OFX_PLUGIN_PATH: Nuke scans it recursively)
if /I "%1"=="staging" set OUT=%ROOT%staging\AlphaGenSAM.ofx.bundle\Contents\Win64
set OBJ=%ROOT%build\obj
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"
set CFLAGS=/nologo /std:c++17 /O2 /EHsc /MT /W3 /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%vendor\openfx\include" /Fo"%OBJ%\\"
set LIBS=winhttp.lib windowscodecs.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib shlwapi.lib
set COMMON="%ROOT%src\comfy.cpp" "%ROOT%src\media.cpp" "%ROOT%src\engine.cpp"

if /I "%1"=="selftest" goto selftest
cl %CFLAGS% /LD "%ROOT%src\AlphaGenSAM.cpp" %COMMON% /Fe"%OUT%\AlphaGenSAM.ofx" /link %LIBS% opengl32.lib user32.lib shell32.lib gdi32.lib || exit /b 1
del "%OUT%\AlphaGenSAM.exp" "%OUT%\AlphaGenSAM.lib" 2>nul

:selftest
cl %CFLAGS% "%ROOT%src\selftest.cpp" %COMMON% /Fe"%ROOT%build\agsam_selftest.exe" /link %LIBS% || exit /b 1
echo Built %OUT%\AlphaGenSAM.ofx and build\agsam_selftest.exe

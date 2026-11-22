@echo off
rem Build the COMMUNITY build (PSP2I_PROD): no command line, data found from the exe's folder, icon from
rem asset\icon.ico. The result is copied into build\prod\ together with SDL3.dll and the FFmpeg DLLs by the
rem build itself. It never touches build\prod\GameData.
rem
rem   build_prod.bat           regenerate build\gen, compile + copy into build\prod\ (the same as
rem                            "build_prod.bat regen"; double-clicking does this and waits at the end)
rem   build_prod.bat quick     compile + copy only (build\gen already current)
rem
rem Uses its own build tree (build\windows-prod), separate from the port build. Never test inside build\prod\.
setlocal
set "MODE=%~1"
if "%MODE%"=="" set "MODE=regen"
set "ROOT=%~dp0"
cd /d "%ROOT%"

if not exist "%ROOT%build\tmp" mkdir "%ROOT%build\tmp"
set "TEMP=%ROOT%build\tmp"
set "TMP=%ROOT%build\tmp"

set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if /i "%MODE%"=="regen" (
    echo == Rebuilding the recompiler ==
    "%CMAKE%" -S psprecomp -B build\recomp
    if errorlevel 1 goto failed
    "%CMAKE%" --build build\recomp --config Release --target allegrexrecomp
    if errorlevel 1 goto failed
    set "EBOOT=%ROOT%EBOOT.BIN"
    if not exist "%ROOT%EBOOT.BIN" if exist "%ROOT%build\dist\EBOOT.BIN" set "EBOOT=%ROOT%build\dist\EBOOT.BIN"
    echo == Regenerating build\gen from EBOOT.BIN ==
    if not exist "build\gen" mkdir "build\gen"
    build\recomp\tools\allegrexrecomp\Release\allegrexrecomp.exe emit "%EBOOT%" build\gen recomp --hooks port\hooks.txt --fixes port\fixes.txt --switches port\switches.txt
    if errorlevel 1 goto failed
)

echo == Building the community build ==
"%CMAKE%" -S port -B build\windows-prod -DPSP2I_PROD=ON
if errorlevel 1 goto failed
"%CMAKE%" --build build\windows-prod --config Release --parallel --target psp2i
if errorlevel 1 goto failed

echo.
echo Done: %ROOT%build\prod\psp2i.exe
if "%~1"=="" pause
exit /b 0

:failed
echo.
echo BUILD FAILED. (If the copy into build\prod failed, close psp2i.exe first.)
if "%~1"=="" pause
exit /b 1

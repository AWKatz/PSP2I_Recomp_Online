@echo off
rem Build the PORT (my build playground): build\windows\Release\psp2i.exe.
rem
rem   build_port.bat           compile only (what you want most of the time)
rem   build_port.bat regen     first rebuild the recompiler and regenerate build\gen from EBOOT.BIN
rem                            (needed after changing port\hooks.txt, port\fixes.txt or the recompiler)
rem
rem Nothing here copies into build\dist or prod; see build_finalbuild.bat / build_prod.bat.
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

rem Temp files on F: (C: runs out of space).
if not exist "%ROOT%build\tmp" mkdir "%ROOT%build\tmp"
set "TEMP=%ROOT%build\tmp"
set "TMP=%ROOT%build\tmp"

set "CMAKE=cmake"
where cmake >nul 2>nul
if errorlevel 1 set "CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if /i "%~1"=="regen" (
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

echo == Building the port ==
if not exist "build\windows\CMakeCache.txt" "%CMAKE%" -S port -B build\windows
if errorlevel 1 goto failed
"%CMAKE%" --build build\windows --config Release --parallel --target psp2i
if errorlevel 1 goto failed

echo.
echo Done: %ROOT%build\windows\Release\psp2i.exe
exit /b 0

:failed
echo.
echo BUILD FAILED.
exit /b 1

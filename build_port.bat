@echo off
rem Build PSP2i for Windows: build\windows\Release\psp2i.exe (with online play) and psp2i_server.exe.
rem
rem   build_port.bat           compile (what you want most of the time)
rem   build_port.bat regen     first build the recompiler and generate build\gen from EBOOT.BIN
rem                            (needed the first time, and after changing port\hooks.txt,
rem                            port\fixes.txt or the recompiler)
rem   build_port.bat offline   the game without online play, in build\windows-offline\Release
rem
rem Needs Visual Studio (C++ workload, which includes CMake) or CMake on the PATH.
rem Nothing here copies into build\dist or prod; see build_finalbuild.bat / build_prod.bat.
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

rem Temp files inside build\ (C: can run out of space).
if not exist "%ROOT%build\tmp" mkdir "%ROOT%build\tmp"
set "TEMP=%ROOT%build\tmp"
set "TMP=%ROOT%build\tmp"

rem ---- CMake: on the PATH, else the one inside Visual Studio (any edition or version) ----
set "CMAKE="
where cmake >nul 2>nul
if not errorlevel 1 set "CMAKE=cmake"
if defined CMAKE goto have_cmake
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_cmake
for /f "usebackq delims=" %%P in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%P"
if not defined VSDIR goto no_cmake
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" goto no_cmake
:have_cmake
echo Using CMake: %CMAKE%

rem ---- options ----
set "REGEN="
set "OUT=build\windows"
set "OPTS=-DENABLE_MMO_NET=ON -DBUILD_DEDICATED_SERVER=ON"
set "TARGETS=psp2i psp2i_server"
for %%A in (%*) do if /i "%%~A"=="regen" set "REGEN=1"
for %%A in (%*) do if /i "%%~A"=="offline" set "OUT=build\windows-offline"
if /i "%OUT%"=="build\windows-offline" set "OPTS=-DENABLE_MMO_NET=OFF -DBUILD_DEDICATED_SERVER=OFF"
if /i "%OUT%"=="build\windows-offline" set "TARGETS=psp2i"
if not defined REGEN if not exist "build\gen\recomp_funcs.c" set "REGEN=1"
if not defined REGEN goto build

rem ---- the game code, generated from EBOOT.BIN ----
set "EBOOT=%ROOT%EBOOT.BIN"
if not exist "%EBOOT%" if exist "%ROOT%build\dist\EBOOT.BIN" set "EBOOT=%ROOT%build\dist\EBOOT.BIN"
if not exist "%EBOOT%" goto no_eboot
echo == Building the recompiler ==
"%CMAKE%" -S psprecomp -B build\recomp
if errorlevel 1 goto failed
"%CMAKE%" --build build\recomp --config Release --target allegrexrecomp
if errorlevel 1 goto failed
set "RECOMP=build\recomp\tools\allegrexrecomp\Release\allegrexrecomp.exe"
if not exist "%RECOMP%" set "RECOMP=build\recomp\tools\allegrexrecomp\allegrexrecomp.exe"
if not exist "%RECOMP%" goto no_recomp
echo == Generating build\gen from %EBOOT% ==
if not exist "build\gen" mkdir "build\gen"
"%RECOMP%" emit "%EBOOT%" build\gen recomp --hooks port\hooks.txt --fixes port\fixes.txt --switches port\switches.txt
if errorlevel 1 goto failed

:build
echo == Building the game (%OUT%) ==
"%CMAKE%" -S port -B %OUT% %OPTS%
if errorlevel 1 goto failed
"%CMAKE%" --build %OUT% --config Release --parallel --target %TARGETS%
if errorlevel 1 goto failed
echo.
echo Done: %ROOT%%OUT%\Release\psp2i.exe
exit /b 0

:no_cmake
echo.
echo CMake was not found. Install Visual Studio with the "Desktop development with C++" workload
echo (it includes CMake), or install CMake from https://cmake.org and add it to the PATH.
goto failed
:no_eboot
echo.
echo EBOOT.BIN was not found. Put your decrypted EBOOT.BIN in %ROOT%
goto failed
:no_recomp
echo.
echo The recompiler was built, but allegrexrecomp.exe was not found under build\recomp\tools\allegrexrecomp\.
goto failed
:failed
echo.
echo BUILD FAILED.
exit /b 1

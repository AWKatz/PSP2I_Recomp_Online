@echo off
rem Build the port and hand it to the user for testing: copies psp2i.exe and psp2i.pdb into build\dist\.
rem
rem   build_finalbuild.bat           port build, then copy
rem   build_finalbuild.bat regen     regenerate build\gen first (see build_port.bat)
rem   build_finalbuild.bat server    also publish the SEGA test server into build\dist\sega_server
rem                                  (close the running server first; it locks its files)
rem
rem Close the game first: a running psp2i.exe cannot be overwritten.
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

set "REGEN="
set "SERVER="
if /i "%~1"=="regen" set "REGEN=regen"
if /i "%~1"=="server" set "SERVER=1"
if /i "%~2"=="server" set "SERVER=1"
if /i "%~2"=="regen" set "REGEN=regen"

call "%ROOT%build_port.bat" %REGEN%
if errorlevel 1 goto failed

echo == Copying to build\dist ==
if not exist "%ROOT%build\dist" mkdir "%ROOT%build\dist"
copy /y "%ROOT%build\windows\Release\psp2i.exe" "%ROOT%build\dist\" >nul
if errorlevel 1 goto locked
copy /y "%ROOT%build\windows\Release\psp2i.pdb" "%ROOT%build\dist\" >nul
if errorlevel 1 goto locked
rem ATRAC music decoder (LGPL, its own DLL) with its licence files, and SDL3 (fetched by CMake).
if exist "%ROOT%build\windows\Release\psp2i_atrac.dll" (
    copy /y "%ROOT%build\windows\Release\psp2i_atrac.dll" "%ROOT%build\dist\" >nul
    if errorlevel 1 goto locked
    xcopy /y /q /i "%ROOT%build\windows\Release\licenses" "%ROOT%build\dist\licenses" >nul
)
if exist "%ROOT%build\windows\Release\SDL3.dll" copy /y "%ROOT%build\windows\Release\SDL3.dll" "%ROOT%build\dist\" >nul

if defined SERVER (
    echo == Publishing the SEGA server ==
    if not exist "%ROOT%port\tools\sega_server\SegaServer.csproj" (
        echo port\tools\sega_server is missing.
        goto failed
    )
    pushd port\tools\sega_server
    dotnet publish -c Release -o "%ROOT%build\dist\sega_server"
    if errorlevel 1 (popd & goto failed)
    popd
)

echo.
echo Done: build\dist\psp2i.exe is up to date.
exit /b 0

:locked
echo.
echo Could not copy into build\dist. Is psp2i.exe still running?
:failed
echo.
echo BUILD FAILED.
exit /b 1

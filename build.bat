@echo off
setlocal

rem Builds the Switch NRO with devkitPro's MSYS2 CMake. The devkitPro toolchain
rem rejects native Windows CMake (such as the one bundled with CLion), so this
rem always calls the MSYS2 one regardless of what is first on PATH.
rem
rem Usage: build.bat [debug^|release]

set "DKP_ROOT=C:\devkitPro"
set "DKP_CMAKE=%DKP_ROOT%\msys2\usr\bin\cmake.exe"

if not exist "%DKP_CMAKE%" (
    echo error: %DKP_CMAKE% not found.
    echo Install it from the devkitPro MSYS2 shell: pacman -S cmake
    exit /b 1
)

set "PATH=%DKP_ROOT%\msys2\usr\bin;%DKP_ROOT%\tools\bin;%PATH%"
set "DEVKITPRO=/opt/devkitpro"

set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=debug"

cd /d "%~dp0"

"%DKP_CMAKE%" --preset %PRESET% || exit /b 1
"%DKP_CMAKE%" --build --preset %PRESET% -j %NUMBER_OF_PROCESSORS% || exit /b 1

echo.
echo Built build\%PRESET%\boz.nro

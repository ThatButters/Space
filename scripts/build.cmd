@echo off
rem Usage: scripts\build.cmd [debug|release] [--run]
setlocal EnableDelayedExpansion
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=release"

rem Locate the MSVC environment via vswhere.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
for /f "usebackq tokens=* delims=" %%i in (`"!VSWHERE!" -latest -products * -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo Visual Studio Build Tools not found
    exit /b 1
)
call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul

rem vcpkg: prefer VCPKG_ROOT, fall back to a sibling checkout.
if not defined VCPKG_ROOT set "VCPKG_ROOT=%~dp0..\..\vcpkg"
if not exist "!VCPKG_ROOT!\scripts\buildsystems\vcpkg.cmake" (
    echo vcpkg not found at !VCPKG_ROOT!
    exit /b 1
)

rem Vulkan SDK: the installer sets VULKAN_SDK system-wide; pick it up if this shell predates it.
if not defined VULKAN_SDK (
    for /d %%d in ("C:\VulkanSDK\*") do set "VULKAN_SDK=%%~d"
)
if defined VULKAN_SDK set "PATH=!VULKAN_SDK!\Bin;!PATH!"

pushd "%~dp0.."
cmake --preset %PRESET%
if errorlevel 1 goto :fail
cmake --build --preset %PRESET%
if errorlevel 1 goto :fail
popd
if "%~2"=="--run" start "" "%~dp0..\build\%PRESET%\Space.exe"
exit /b 0

:fail
popd
exit /b 1

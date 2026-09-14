@echo off
rem Opt Space.exe out of Windows Auto HDR (per-app graphics setting, HKCU only, reversible).
rem Windows sometimes decides a windowed Vulkan app is a game and re-tonemaps its SDR output, which
rem mangles dark scenes. Native HDR output (the default when the display is HDR) avoids this too;
rem this script covers the SDR fallback. Usage: scripts\autohdr_optout.cmd [on|off]
setlocal
set "KEY=HKCU\Software\Microsoft\DirectX\UserGpuPreferences"
set "VALUE=AutoHDREnable=2096;"
if /i "%~1"=="on" set "VALUE=AutoHDREnable=2097;"
for %%p in ("%~dp0..\build\release\Space.exe" "%~dp0..\build\debug\Space.exe") do (
    for %%f in ("%%~p") do (
        reg add "%KEY%" /v "%%~ff" /t REG_SZ /d "%VALUE%" /f >nul && echo %VALUE% for %%~ff
    )
)
endlocal

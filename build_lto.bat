@echo off
setlocal
rem Manual build only. Defaults: 90%% CPU cap and 90%% RAM-sized memory budget.
rem Optional: build_lto.bat -CpuPercent 80 -RamPercent 75
set "GS_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if defined PROCESSOR_ARCHITEW6432 set "GS_POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%GS_POWERSHELL%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build_lto.ps1" %*
set "GS_RESULT=%ERRORLEVEL%"
echo.
if "%GS_RESULT%"=="0" (echo Build completed.) else (echo Build failed. Exit code: %GS_RESULT%)
pause
exit /b %GS_RESULT%

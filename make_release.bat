@echo off
setlocal
rem Opens the release window (scripts\release_tool.ps1): build the player
rem release for Windows, Linux or both, with or without the test build.
rem Windows: clean player folder + zip with no game code: engine from
rem build_lto.bat (run that first), launcher, builder, translator and a
rem trimmed compiler. Linux: the same in GSRecomp-Release\linux, built in WSL
rem Ubuntu-24.04. The test builds the game from the ROM using only the
rem release's own tools.
rem With options it runs in this console instead, no window:
rem   make_release.bat [-NoTest] [-NoWindows] [-NoLinux]
set "GS_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if defined PROCESSOR_ARCHITEW6432 set "GS_POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
if "%~1"=="" (
    start "" "%GS_POWERSHELL%" -NoLogo -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%~dp0scripts\release_tool.ps1"
    exit /b 0
)
"%GS_POWERSHELL%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\make_release.ps1" %*
set "GS_RESULT=%ERRORLEVEL%"
echo.
if "%GS_RESULT%"=="0" (echo Release completed.) else (echo Release failed. Exit code: %GS_RESULT%)
pause
exit /b %GS_RESULT%

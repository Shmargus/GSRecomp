@echo off
setlocal
rem Builds the combined player release for both games.
rem With no options, opens the existing release window.
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
set "skip_pause="
for %%A in (%*) do if /I "%%~A"=="-CheckOnly" set "skip_pause=1"
if not defined skip_pause pause
exit /b %GS_RESULT%

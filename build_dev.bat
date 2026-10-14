@echo off
setlocal
set "skip_pause="
for %%A in (%*) do if /I "%%~A"=="-CheckOnly" set "skip_pause=1"
set "GS_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if defined PROCESSOR_ARCHITEW6432 set "GS_POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%GS_POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build_dev.ps1" %*
set "build_result=%ERRORLEVEL%"
if not defined skip_pause pause
exit /b %build_result%

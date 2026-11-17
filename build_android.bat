@echo off
rem Builds the Android launcher app (scripts\android\build_apk.py):
rem Releases\android\GoldenSunRecompiled.apk. Copy it to the phone to install.
setlocal
python "%~dp0scripts\android\build_apk.py" %*
set "build_result=%ERRORLEVEL%"
pause
exit /b %build_result%

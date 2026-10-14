@echo off
rem Rebuilds only GoldenSunLauncher.exe, for launcher-only changes. The games are not touched.
call "%~dp0build_dev.bat" -Game None %*

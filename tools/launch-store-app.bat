@echo off
rem Runs launch-store-app.ps1 without changing PowerShell's script policy.
rem Usage: launch-store-app.bat <part of the app name> [aam|explorer|shell]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch-store-app.ps1" %*
pause

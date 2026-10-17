@echo off
rem Runs list-windows.ps1 without changing PowerShell's script policy.
rem Usage: list-windows.bat [seconds to wait first, default 5]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0list-windows.ps1" %*
pause

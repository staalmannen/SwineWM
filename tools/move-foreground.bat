@echo off
rem Runs move-foreground.ps1 without changing PowerShell's script policy.
rem Usage: move-foreground.bat [seconds] [x] [y] [width] [height]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0move-foreground.ps1" %*
pause

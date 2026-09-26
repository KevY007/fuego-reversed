@echo off
rem Double-click / cmd wrapper for scripts\bootstrap.ps1 (see that file for options).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0bootstrap.ps1" %*
exit /b %errorlevel%

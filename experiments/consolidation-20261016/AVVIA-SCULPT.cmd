@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0AVVIA-SCULPT.ps1" %*
if errorlevel 1 pause

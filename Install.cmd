@echo off
setlocal
title BUSMUST INCA J2534 - Install
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Manage-SupportPackage.ps1" -Action Install
if errorlevel 1 (echo Installation failed. See the message above.) else (echo Installation completed. Select BUSMUST - BMJ2534 in INCA and open its configuration button.)
pause

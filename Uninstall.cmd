@echo off
setlocal EnableExtensions DisableDelayedExpansion
title BUSMUST INCA J2534 - Uninstall
set "uninstallScript=%~dp0Manage-SupportPackage.ps1"
setlocal EnableDelayedExpansion
(
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "!uninstallScript!" -Action Uninstall
    set "uninstallResult=!errorlevel!"
    if !uninstallResult! neq 0 (echo Uninstall failed. See the message above.) else (echo Uninstall completed. See above for any modified files preserved.)
    pause
    exit !uninstallResult!
)

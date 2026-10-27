@echo off
setlocal EnableExtensions DisableDelayedExpansion
title BUSMUST INCA J2534 - Uninstall
set "BUSMUST_SETUP_ROOT=%~dp0"
setlocal EnableDelayedExpansion
(
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { $root=$env:BUSMUST_SETUP_ROOT; foreach($relative in @('Manage-SupportPackage.ps1','scripts/Register-BMJ2534.ps1')) { $path=Join-Path $root $relative; if(-not (Test-Path -LiteralPath $path -PathType Leaf)){throw ('Missing installation script: '+$relative)}; Unblock-File -LiteralPath $path }; & (Join-Path $root 'Manage-SupportPackage.ps1') -Action Uninstall; if(-not $?) {exit 1} } catch { [Console]::Error.WriteLine($_.Exception.Message); [Console]::Error.WriteLine('Setup could not complete. If Windows reports an organization restriction, ask your IT administrator to permit this package.'); exit 1 }"
    set "uninstallResult=!errorlevel!"
    if !uninstallResult! neq 0 (echo Uninstall failed. See the message above.) else (echo Uninstall completed. See above for any modified files preserved.)
    pause
    exit !uninstallResult!
)

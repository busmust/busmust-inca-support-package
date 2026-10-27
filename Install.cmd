@echo off
setlocal EnableExtensions DisableDelayedExpansion
title BUSMUST INCA J2534 - Install
set "BUSMUST_SETUP_ROOT=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; try { $root=$env:BUSMUST_SETUP_ROOT; foreach($relative in @('Manage-SupportPackage.ps1','scripts/Register-BMJ2534.ps1')) { $path=Join-Path $root $relative; if(-not (Test-Path -LiteralPath $path -PathType Leaf)){throw ('Missing installation script: '+$relative)}; Unblock-File -LiteralPath $path }; & (Join-Path $root 'Manage-SupportPackage.ps1') -Action Install; if(-not $?) {exit 1} } catch { [Console]::Error.WriteLine($_.Exception.Message); [Console]::Error.WriteLine('Setup could not complete. See the specific error above.'); exit 1 }"
set "installResult=%errorlevel%"
if %installResult% neq 0 (echo Installation failed. See the message above.) else (echo Installation completed. Select BUSMUST - BMJ2534 in INCA and open its configuration button.)
pause
exit /b %installResult%

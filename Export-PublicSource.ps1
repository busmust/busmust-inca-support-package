# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 BUSMUST Tech Co.,Ltd.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
# An explicit allowlist prevents internal workflow, credentials, evidence and
# build output from entering a future public source repository.
$files=@(
    '.gitignore','.gitattributes','LICENSE','NOTICE','THIRD_PARTY.md','README.md','README.zh-CN.md',
    '先看这里_BUSMUST设备接入INCA指南.pdf',
    'Build-Native.ps1','Build-SupportPackage.ps1','Export-PublicSource.ps1',
    'tests/README.md','tests/Run-Regression.ps1','tests/NativeRegression.vcxproj',
    'tests/mock_bmapi.cpp','tests/native_regression.cpp',
    'Install.cmd','Uninstall.cmd','Manage-SupportPackage.ps1',
    'native/J2534/README.md','native/J2534/Register-BMJ2534.ps1',
    'native/J2534/BMJ2534.BMAPI.props','native/J2534/BMJ2534.Version.props','native/J2534/BMJ2534.def','native/J2534/BMJ2534.vcxproj',
    'native/J2534/BMJ2534Endpoint.vcxproj','native/J2534/BMJ2534Config.cpp',
    'native/J2534/BMJ2534Config.vcxproj','native/J2534/BMJ2534Config.rc',
    'native/J2534/BMJ2534Config.ico','native/J2534/BMJ2534Config.manifest',
    'native/J2534/bmj2534.cpp','native/J2534/bmj2534.h','native/J2534/bmj2534.rc',
    'native/J2534/bmj2534_log.cpp','native/J2534/bmj2534_log.h',
    'native/J2534/bmj2534_backend.cpp','native/J2534/bmj2534_backend.h',
    'native/J2534/bmj2534_endpoint.cpp','native/J2534/bmj2534_private.h',
    'native/J2534/bmj2534_isotp.cpp','native/J2534/bmj2534_isotp.h',
    'native/J2534/third_party/op2j2534/LICENSE','native/J2534/third_party/op2j2534/NOTICE',
    'native/J2534/third_party/op2j2534/j2534.h','native/J2534/third_party/isotp-c/LICENSE',
    'native/J2534/third_party/isotp-c/isotp.c','native/J2534/third_party/isotp-c/isotp.h',
    'native/J2534/third_party/isotp-c/isotp_config.h','native/J2534/third_party/isotp-c/isotp_defines.h',
    'native/J2534/third_party/isotp-c/isotp_user.h'
)
$source=[IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\','/')
$output=[IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
if($output -eq [IO.Path]::GetPathRoot($output).TrimEnd('\','/') -or $output -eq $source -or
   $output.StartsWith($source+'\',[StringComparison]::OrdinalIgnoreCase) -or
   @($OutputDirectory -split '[\\/]'|Where-Object {$_ -eq '..'}).Count) {throw 'Use a dedicated output directory outside the checkout.'}
function Assert-NoReparse([string]$Path) {
    $cursor=[IO.Path]::GetFullPath($Path)
    while($cursor) {
        if(Test-Path -LiteralPath $cursor) {
            if((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse points are not public export paths.'}
        }
        $parent=Split-Path -Parent $cursor
        if($parent -eq $cursor){break};$cursor=$parent
    }
}
Assert-NoReparse $output
if((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count){throw 'Preserve existing output.'}
$volume=New-Object IO.DriveInfo([IO.Path]::GetPathRoot($output))
if($volume.AvailableFreeSpace -lt 20GB -or $volume.AvailableFreeSpace/[double]$volume.TotalSize -lt 0.05){throw 'Output volume needs 20GB and 5% free space.'}
foreach($file in $files) {
    $path=Join-Path $source $file;Assert-NoReparse $path
    if(-not (Test-Path -LiteralPath $path -PathType Leaf)){throw "Allowlisted source is missing: $file"}
}
foreach($file in $files) {
    $destination=Join-Path $output $file
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force|Out-Null
    Copy-Item -LiteralPath (Join-Path $source $file) -Destination $destination
    if((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne
       (Get-FileHash -LiteralPath (Join-Path $source $file) -Algorithm SHA256).Hash){throw 'Public export readback failed.'}
}
if($volume.AvailableFreeSpace -lt 20GB -or $volume.AvailableFreeSpace/[double]$volume.TotalSize -lt 0.05){throw 'Output volume fell below its threshold.'}
[pscustomobject]@{Result='EXPORTED_NOT_PUBLISHED';Directory=$output;Files=$files.Count}|ConvertTo-Json

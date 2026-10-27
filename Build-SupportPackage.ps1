# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 BUSMUST Tech Co.,Ltd.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RuntimeRoot,
    [string]$NativeComponentRoot,
    [Parameter(Mandatory)][string]$LicensingRoot,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$CreateZip
)
$ErrorActionPreference='Stop'
if(-not $NativeComponentRoot){$NativeComponentRoot=Join-Path $PSScriptRoot 'native/J2534'}
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Management/Microsoft.PowerShell.Management.psd1') -ErrorAction Stop
$output=[IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
if($output.Length -lt 8 -or $output.StartsWith(($PSScriptRoot+'\'),[StringComparison]::OrdinalIgnoreCase) -or $output -eq $PSScriptRoot) { throw 'Use a dedicated output directory outside the source checkout.' }
if((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count) { throw 'Preserve existing package output.' }
$volume=New-Object IO.DriveInfo([IO.Path]::GetPathRoot($output))
if($volume.AvailableFreeSpace -lt 20GB -or $volume.AvailableFreeSpace/[double]$volume.TotalSize -lt 0.05) { throw 'Package output volume needs 20GB and 5% free space.' }
$zip=$output+'.zip'
if($CreateZip -and (Test-Path -LiteralPath $zip)){throw 'Preserve the existing ZIP.'}
foreach($folder in @('x64','Win32')) {
    $runtime=Join-Path $RuntimeRoot $folder
    $core=if($folder -eq 'x64'){'BMJ2534.dll'}else{'BMJ25332.dll'}
    foreach($name in @($core,'BMJ2534Endpoint.dll','BMJ2534Config.exe','BMAPI.dll')) {
        if(-not (Test-Path -LiteralPath (Join-Path $runtime $name) -PathType Leaf)){throw "Missing matching runtime file: $folder/$name"}
    }
}
foreach($required in @('licenses/BMAPI/LICENSE')) {
    if(-not (Test-Path -LiteralPath (Join-Path $LicensingRoot $required) -PathType Leaf)){throw "Missing distribution material: $required"}
}
foreach($required in @('op2j2534/LICENSE','op2j2534/NOTICE','isotp-c/LICENSE')) {
    $nativeMaterial=Join-Path $NativeComponentRoot ('third_party/'+$required)
    $licensingMaterial=Join-Path $LicensingRoot ('licenses/'+$required)
    if(-not (Test-Path -LiteralPath $nativeMaterial -PathType Leaf) -and
       -not (Test-Path -LiteralPath $licensingMaterial -PathType Leaf)) {throw "Missing upstream distribution material: $required"}
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
function Copy-Payload([string]$Source,[string]$Relative) {
    $destination=[IO.Path]::GetFullPath((Join-Path $output $Relative))
    if(-not $destination.StartsWith($output+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Package path escapes output directory.'}
    $parent=Split-Path -Parent $destination
    if(-not (Test-Path -LiteralPath $parent)){New-Item -ItemType Directory -Path $parent -Force | Out-Null}
    Copy-Item -LiteralPath $Source -Destination $destination -ErrorAction Stop
}
$documents=@('README.md','README.zh-CN.md','先看这里_BUSMUST设备接入INCA指南.pdf')
foreach($file in $documents) {
    if(-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $file) -PathType Leaf)){throw "Missing required document: $file"}
}
foreach($file in @('Manage-SupportPackage.ps1','Install.cmd','Uninstall.cmd','LICENSE','NOTICE','THIRD_PARTY.md')+$documents) { Copy-Payload (Join-Path $PSScriptRoot $file) $file }
Copy-Payload (Join-Path $NativeComponentRoot 'Register-BMJ2534.ps1') 'scripts/Register-BMJ2534.ps1'
foreach($component in @('op2j2534','isotp-c')) {
    foreach($name in @('LICENSE','NOTICE')) {
        $source=Join-Path $NativeComponentRoot ('third_party/'+$component+'/'+$name)
        if(-not (Test-Path -LiteralPath $source -PathType Leaf)) {$source=Join-Path $LicensingRoot ('licenses/'+$component+'/'+$name)}
        if(Test-Path -LiteralPath $source -PathType Leaf){Copy-Payload $source ('licenses/'+$component+'/'+$name)}
    }
}
foreach($folder in @('x64','Win32')) {
    $core=if($folder -eq 'x64'){'BMJ2534.dll'}else{'BMJ25332.dll'}
    $destinationFolder=if($folder -eq 'Win32'){'x86'}else{'x64'}
    foreach($name in @($core,'BMJ2534Endpoint.dll','BMJ2534Config.exe','BMAPI.dll')) { Copy-Payload (Join-Path $RuntimeRoot ($folder+'/'+$name)) ('runtime/'+$destinationFolder+'/'+$name) }
}
$legalFiles=@('licenses/BMAPI/LICENSE')
foreach($relative in $legalFiles) {
    $source=Join-Path $LicensingRoot $relative
    if(-not (Test-Path -LiteralPath $source -PathType Leaf)){throw "Missing distribution material: $relative"}
    $cursor=Get-Item -LiteralPath $source
    while($cursor -and $cursor.FullName -ne [IO.Path]::GetPathRoot($cursor.FullName)) {
        if($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse points are not distribution materials.'}
        $cursor=if($cursor -is [IO.FileInfo]){$cursor.Directory}else{$cursor.Parent}
    }
    Copy-Payload $source $relative
}
$files=@(Get-ChildItem -LiteralPath $output -Recurse -File | Sort-Object FullName | ForEach-Object {
    [pscustomobject]@{Path=$_.FullName.Substring($output.Length+1).Replace('\','/');Sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
})
[pscustomobject]@{Schema=1;Owner='BUSMUST.INCA.J2534';Files=$files} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'payload.json') -Encoding utf8
if($CreateZip) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($output,$zip,[IO.Compression.CompressionLevel]::Optimal,$false)
}
if($volume.AvailableFreeSpace -lt 20GB -or $volume.AvailableFreeSpace/[double]$volume.TotalSize -lt 0.05){throw 'Output volume fell below its free-space threshold.'}
[pscustomobject]@{Result='CANDIDATE_ASSEMBLED_PENDING_ACCEPTANCE';Directory=$output;Files=$files.Count;Zip=$(if($CreateZip){$zip}else{$null});ManifestSha256=(Get-FileHash -LiteralPath (Join-Path $output 'payload.json') -Algorithm SHA256).Hash} | ConvertTo-Json

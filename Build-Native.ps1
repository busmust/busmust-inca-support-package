# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 BUSMUST Tech Co.,Ltd.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BMAPIIncludeDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateSet('Win32','x64')][string[]]$Architecture = @('Win32','x64'),
    [string]$MSBuildPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$utf8 = New-Object Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8
$sdk = (Resolve-Path -LiteralPath $BMAPIIncludeDirectory).Path
$versionFile = Join-Path $PSScriptRoot 'native/J2534/BMJ2534.Version.props'
[xml]$versionDocument = Get-Content -LiteralPath $versionFile -Raw
$productVersion = [string]$versionDocument.Project.PropertyGroup.BMJProductVersion
if ($productVersion -notmatch '^\d+\.\d+\.\d+\.\d+$' -or @($productVersion.Split('.') | Where-Object { [long]$_ -gt 65535 }).Count) { throw 'BMJ2534 product version requires four numeric components in 0..65535.' }
foreach ($name in @('bmapi.h','bm_usb_def.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $sdk $name) -PathType Leaf)) { throw "Missing BMAPI SDK header: $name" }
}
$output = [IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
$checkout = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd('\','/')
if ($output -eq $checkout -or $output.StartsWith($checkout+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Build outside the source checkout.' }
if ((Test-Path -LiteralPath $output) -and @(Get-ChildItem -LiteralPath $output -Force).Count) { throw 'Preserve previous build output; choose an empty directory.' }
$volume = New-Object IO.DriveInfo([IO.Path]::GetPathRoot($output))
if ($volume.AvailableFreeSpace -lt 20GB -or $volume.AvailableFreeSpace/[double]$volume.TotalSize -lt 0.05) { throw 'Output volume needs 20GB and 5% free space.' }
if (-not $MSBuildPath) {
    $command = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($command) { $MSBuildPath = $command.Source }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Install Visual Studio MSBuild with the v140_xp tools, or pass -MSBuildPath.' }
        $found = @(& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe')
        if ($LASTEXITCODE -ne 0 -or $found.Count -eq 0) { throw 'MSBuild was not found.' }
        $MSBuildPath = $found[0]
    }
}
$MSBuildPath = (Resolve-Path -LiteralPath $MSBuildPath).Path
New-Item -ItemType Directory -Path $output -Force | Out-Null
$records = @()
foreach ($arch in @($Architecture | Select-Object -Unique)) {
    foreach ($project in @('BMJ2534','BMJ2534Endpoint','BMJ2534Config')) {
        $destination = Join-Path $output $arch
        $intermediate = Join-Path $output ('obj/'+$arch+'/'+$project)
        New-Item -ItemType Directory -Path $destination,$intermediate -Force | Out-Null
        $log = Join-Path $output ($arch+'-'+$project+'.log')
        $arguments = @((Join-Path $PSScriptRoot ('native/J2534/'+$project+'.vcxproj')),
            '/nologo','/t:Rebuild','/m:2','/p:Configuration=Release',('/p:Platform='+$arch),
            ('/p:BMAPIIncludeDirectory='+$sdk),
            ('/p:OutDir='+$destination+'\'),('/p:IntDir='+$intermediate+'\'),
            '/fileLogger',('/fileLoggerParameters:LogFile='+$log+';Encoding=UTF-8;Verbosity=normal'))
        & $MSBuildPath @arguments
        if ($LASTEXITCODE -ne 0) { throw "Native build failed: $arch/$project; see $log" }
        $name = if ($project -eq 'BMJ2534' -and $arch -eq 'Win32') { 'BMJ25332.dll' }
                elseif ($project -eq 'BMJ2534Config') { 'BMJ2534Config.exe' }
                else { $project+'.dll' }
        $artifact = Join-Path $destination $name
        if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) { throw "Build output missing: $artifact" }
        $records += [pscustomobject]@{ Architecture=$arch; File=$name; Sha256=(Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash; Log=$log }
    }
}
if ($volume.AvailableFreeSpace -lt 20GB -or $volume.AvailableFreeSpace/[double]$volume.TotalSize -lt 0.05) { throw 'Output volume fell below its free-space threshold.' }
[pscustomobject]@{ Result='BUILT_PENDING_ACCEPTANCE'; BMJ2534Version=$productVersion; BMJ2534VersionSourceSha256=(Get-FileHash -LiteralPath $versionFile -Algorithm SHA256).Hash; BMAPIHeaderSha256=(Get-FileHash -LiteralPath (Join-Path $sdk 'bmapi.h') -Algorithm SHA256).Hash; Artifacts=$records } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'native-build.json') -Encoding utf8
Write-Output ('Native Release build complete: '+$output)

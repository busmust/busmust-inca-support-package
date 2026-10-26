# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BMAPIIncludeDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$MSBuildPath
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$source=Split-Path -Parent $PSScriptRoot
$output=[IO.Path]::GetFullPath($OutputDirectory).TrimEnd('\','/')
# Build-Native validates output ownership, free space, SDK inputs and both arches.
$arguments=@{BMAPIIncludeDirectory=$BMAPIIncludeDirectory;OutputDirectory=$output}
if($MSBuildPath){$arguments.MSBuildPath=$MSBuildPath}
& (Join-Path $source 'Build-Native.ps1') @arguments
if(-not $MSBuildPath) {
    $command=Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if($command){$MSBuildPath=$command.Source}
    else {
        $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $MSBuildPath=@(& $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe')[0]
    }
}
$MSBuildPath=(Resolve-Path -LiteralPath $MSBuildPath).Path
$sdk=(Resolve-Path -LiteralPath $BMAPIIncludeDirectory).Path
$results=@()
foreach($arch in @('Win32','x64')) {
    $runtime=Join-Path $output $arch
    foreach($role in @('Mock','Probe')) {
        $intermediate=Join-Path $output ('test-obj/'+$arch+'/'+$role)
        $log=Join-Path $output ($arch+'-'+$role+'.log')
        $mock=if($role -eq 'Mock'){'true'}else{'false'}
        & $MSBuildPath (Join-Path $PSScriptRoot 'NativeRegression.vcxproj') '/nologo' '/t:Rebuild' '/m:2' '/p:Configuration=Release' ("/p:Platform=$arch") ("/p:BuildMock=$mock") ("/p:BMAPIIncludeDirectory=$sdk") ("/p:OutDir=$runtime\") ("/p:IntDir=$intermediate\") '/fileLogger' ("/fileLoggerParameters:LogFile=$log;Encoding=UTF-8;Verbosity=normal")
        if($LASTEXITCODE -ne 0){throw "Test build failed: $arch/$role; see $log"}
    }
    $stdout=Join-Path $output ($arch+'-regression.txt')
    $stderr=Join-Path $output ($arch+'-regression-error.txt')
    # No visible console is needed for automated tests.
    $process=Start-Process -FilePath (Join-Path $runtime 'NativeRegression.exe') -WorkingDirectory $runtime -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
    if(-not $process.WaitForExit(60000)) {
        $process.Kill();$process.WaitForExit();throw "Exact mock-only regression child timed out: $arch; logs retained."
    }
    if($process.ExitCode -ne 0){throw "Regression failed: $arch; see $stdout and $stderr"}
    $results += [pscustomobject]@{Architecture=$arch;Result='PASS';Stdout=$stdout;Stderr=$stderr}
}
[pscustomobject]@{Scope='Software mock only; no physical/INCA acceptance';Results=$results}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $output 'regression-result.json') -Encoding utf8
Write-Output ('Both architectures passed: '+$output)

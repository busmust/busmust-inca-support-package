# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Install','Remove','Check')][string]$Action,
    [Parameter(Mandatory)][ValidateSet('x86','x64')][string]$Architecture,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][guid]$InstallationId
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
if($InstallationId -eq [guid]::Empty){throw 'An installation ownership ID is required.'}
$runtime=[IO.Path]::GetFullPath($RuntimeDirectory)
$library=Join-Path $runtime 'BMJ2534Endpoint.dll'
$config=Join-Path $runtime 'BMJ2534Config.exe'
$core=if($Architecture -eq 'x86'){'BMJ25332.dll'}else{'BMJ2534.dll'}
$provider='BUSMUST - BMJ2534'
$keyPath='SOFTWARE\PassThruSupport.04.04\'+$provider
$view=if($Architecture -eq 'x64'){[Microsoft.Win32.RegistryView]::Registry64}else{[Microsoft.Win32.RegistryView]::Registry32}
function Get-PeMachine([string]$Path) {
    $stream=[IO.File]::OpenRead($Path)
    try {
        $reader=[IO.BinaryReader]::new($stream)
        if($reader.ReadUInt16() -ne 0x5A4D){throw "Invalid PE file: $Path"}
        $stream.Position=0x3c;$offset=$reader.ReadUInt32();$stream.Position=$offset
        if($reader.ReadUInt32() -ne 0x4550){throw "Invalid PE signature: $Path"}
        return $reader.ReadUInt16()
    } finally {$stream.Dispose()}
}
if($Action -eq 'Install') {
    $machine=if($Architecture -eq 'x64'){0x8664}else{0x14c}
    foreach($file in @($library,$config,(Join-Path $runtime $core),(Join-Path $runtime 'BMAPI.dll'))) {
        if(-not (Test-Path -LiteralPath $file -PathType Leaf) -or (Get-PeMachine $file) -ne $machine) {
            throw "Missing or wrong-architecture runtime: $file"
        }
    }
}
$values=@{Vendor='BUSMUST';Name=$provider;FunctionLibrary=$library;ConfigApplication=$config;BUSMUSTInstallationId=$InstallationId.ToString()}
$capabilities=@{CAN=1;ISO15765=1;CAN_FD_PS=0x1000D;ISO15765_FD_PS=0x1000C;J1850PWM=0;J1850VPW=0;ISO9141=0;ISO14230=0;SCI_A_ENGINE=0;SCI_A_TRANS=0;SCI_B_ENGINE=0;SCI_B_TRANS=0}
$base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
try {
    $existing=$base.OpenSubKey($keyPath,$false)
    if($existing) {
        try {
            if($existing.GetValue('BUSMUSTInstallationId') -ne $InstallationId.ToString() -or
               $existing.GetValue('Vendor') -notin @($null,'BUSMUST') -or
               $existing.GetValue('FunctionLibrary') -notin @($null,$library) -or
               $existing.GetValue('ConfigApplication') -notin @($null,$config)) {
                throw 'Preserve a provider belonging to another or modified installation.'
            }
        } finally {$existing.Dispose()}
    }
    if($Action -eq 'Remove') {
        $base.DeleteSubKeyTree($keyPath,$false)
        $check=$base.OpenSubKey($keyPath,$false)
        if($check){$check.Dispose();throw 'Provider removal readback failed.'}
        [pscustomobject]@{Action=$Action;Architecture=$Architecture;Provider=$provider};return
    }
    if($Action -eq 'Install') {
        $key=$base.CreateSubKey($keyPath,$true)
        try {
            $key.SetValue('BUSMUSTInstallationId',$InstallationId.ToString(),[Microsoft.Win32.RegistryValueKind]::String);$key.Flush()
            foreach($entry in $values.GetEnumerator()){$key.SetValue($entry.Key,$entry.Value,[Microsoft.Win32.RegistryValueKind]::String)}
            foreach($entry in $capabilities.GetEnumerator()){$key.SetValue($entry.Key,$entry.Value,[Microsoft.Win32.RegistryValueKind]::DWord)}
            $key.Flush()
        } finally {$key.Dispose()}
    }
    $changed=$false;$check=$base.OpenSubKey($keyPath,$false)
    try {
        if(-not $check){$changed=$true}
        else {
            foreach($entry in $values.GetEnumerator()) {
                if($check.GetValue($entry.Key) -ne $entry.Value -or $check.GetValueKind($entry.Key) -ne [Microsoft.Win32.RegistryValueKind]::String){$changed=$true}
            }
            foreach($entry in $capabilities.GetEnumerator()) {
                if($check.GetValue($entry.Key) -ne $entry.Value -or $check.GetValueKind($entry.Key) -ne [Microsoft.Win32.RegistryValueKind]::DWord){$changed=$true}
            }
        }
    } finally {if($check){$check.Dispose()}}
    if($Action -eq 'Install' -and $changed){throw 'Provider registration readback failed.'}
    [pscustomobject]@{Action=$Action;Architecture=$Architecture;Provider=$provider;FunctionLibrary=$library;ConfigApplication=$config;ChangesRequired=$changed}
} finally {$base.Dispose()}

# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 BUSMUST Tech Co.,Ltd.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Install','Uninstall','Plan')][string]$Action,
    [string]$InstallRoot,
    [switch]$NoElevation,
    [string]$ElevationRequestId,
    [string]$ElevationResultPath
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Management/Microsoft.PowerShell.Management.psd1') -ErrorAction Stop
if(-not $InstallRoot) {
    # Installed entry points operate on their own instance. A malformed local
    # ownership record must fail validation, never fall back to another root.
    if((Test-Path -LiteralPath (Join-Path $PSScriptRoot 'install-state.json')) -or
       (Test-Path -LiteralPath (Join-Path $PSScriptRoot 'pending-install.json'))) {
        $InstallRoot=$PSScriptRoot
    } else {
        $programFiles=if($env:ProgramW6432){$env:ProgramW6432}else{$env:ProgramFiles}
        $InstallRoot=Join-Path $programFiles 'BUSMUST/INCA-J2534'
    }
}
$install=[IO.Path]::GetFullPath($InstallRoot).TrimEnd('\','/')
if($install -eq [IO.Path]::GetPathRoot($install).TrimEnd('\','/') -or @($InstallRoot -split '[\\/]'|Where-Object {$_ -eq '..'}).Count) { throw 'Select a dedicated installation directory without parent traversal.' }
$statePath=Join-Path $install 'install-state.json'
$pendingPath=Join-Path $install 'pending-install.json'
$owner='BUSMUST.INCA.J2534'
function Assert-NoReparse([string]$Path) {
    $cursor=[IO.Path]::GetFullPath($Path)
    while($cursor) {
        if(Test-Path -LiteralPath $cursor) {
            if((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse points are not installation targets: $cursor" }
        }
        $parent=Split-Path -Parent $cursor
        if($parent -eq $cursor){break};$cursor=$parent
    }
}
function Child-Path([string]$Root,[string]$Relative) {
    if([string]::IsNullOrWhiteSpace($Relative) -or [IO.Path]::IsPathRooted($Relative) -or
        $Relative -match '[:\x00-\x1f]' -or @($Relative -split '[\\/]' | Where-Object {$_ -in @('','..','.')}).Count) { throw 'Invalid package-relative file path.' }
    $path=[IO.Path]::GetFullPath((Join-Path $Root $Relative))
    if(-not $path.StartsWith(([IO.Path]::GetFullPath($Root).TrimEnd('\')+'\'),[StringComparison]::OrdinalIgnoreCase)) { throw 'File escapes installation directory.' }
    Assert-NoReparse $path
    return $path
}
function Read-Json([string]$Path) { Get-Content -LiteralPath $Path -Raw -Encoding utf8 | ConvertFrom-Json }
function Write-JsonAtomic([string]$Path,$Value) {
    $temporary=$Path+'.writing'
    # Before the first ownership record is published, keep a partial write out
    # of the new installation directory so an interrupted first install retries.
    $initial=($Path -eq $pendingPath -and -not (Test-Path -LiteralPath $statePath) -and
              -not (Test-Path -LiteralPath $pendingPath))
    if($initial) {$temporary=$install+'.initial-'+[guid]::NewGuid().ToString('N')+'.writing'}
    try {
        $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding utf8
        if(Test-Path -LiteralPath $Path) { [IO.File]::Replace($temporary,$Path,[NullString]::Value) }
        else { [IO.File]::Move($temporary,$Path) }
    } finally {
        if($initial -and (Test-Path -LiteralPath $temporary)){Remove-Item -LiteralPath $temporary -Force}
    }
}
function File-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Test-DllPayload([string]$Relative) { [IO.Path]::GetExtension($Relative) -ieq '.dll' }
function Test-InstallPayload([string]$Relative) {
    $Relative -eq 'payload.json' -or $Relative -like 'runtime/*' -or $Relative -in @('Manage-SupportPackage.ps1','Install.cmd','Uninstall.cmd','scripts/Register-BMJ2534.ps1')
}
function Assert-Unlocked([string]$Path) {
    $handle=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    $handle.Dispose()
}
function Read-State {
    $committed=$null;$pending=$null
    foreach($path in @($statePath,$pendingPath)) {
        if(-not (Test-Path -LiteralPath $path)){continue}
        $record=Read-Json $path
        if($record.Schema -notin @(1,2) -or $record.Owner -ne $owner -or [IO.Path]::GetFullPath($record.InstallRoot) -ne $install -or [guid]$record.InstallationId -eq [guid]::Empty) { throw 'The installation ownership record does not match this directory.' }
        if($path -eq $pendingPath){$pending=$record}else{$committed=$record}
    }
    if(-not $pending){return $committed}
    if($committed -and $committed.InstallationId -ne $pending.InstallationId){throw 'Pending and committed installation owners differ.'}
    $previous=@()
    if($pending.PSObject.Properties['PreviousFiles']){$previous+=@($pending.PreviousFiles)}
    if($committed){$previous+=@($committed.Files)}
    $pending|Add-Member -NotePropertyName PreviousFiles -NotePropertyValue $previous -Force
    return $pending
}
function Test-OwnedFileHash($State,[string]$Relative,[string]$Hash) {
    if(-not $State){return $false}
    $records=@($State.Files)
    if($State.PSObject.Properties['PreviousFiles']){$records+=@($State.PreviousFiles)}
    return @($records|Where-Object {$_.Path -eq $Relative -and $_.Sha256 -eq $Hash}).Count -gt 0
}
function Native-Registration([string]$Operation,[string]$Architecture,[guid]$InstallationId) {
    $folder=if($Architecture -eq 'x64'){'x64'}else{'x86'}
    & (Join-Path $PSScriptRoot 'scripts/Register-BMJ2534.ps1') -Action $Operation -Architecture $Architecture -RuntimeDirectory (Join-Path $install ('runtime/'+$folder)) -InstallationId $InstallationId
}
function Remove-LegacyRegistrations($State) {
    if(-not $State -or -not $State.PSObject.Properties['Bindings']) {return}
    foreach($binding in @($State.Bindings)) {
        if($binding.Uid -notmatch '^[0-9A-Fa-f]{24}$' -or $binding.Port -lt 0 -or $binding.Port -gt 65535) {throw 'Invalid legacy ownership binding.'}
        $uid=$binding.Uid.ToUpperInvariant();$port=[int]$binding.Port
        foreach($arch in @('x64','x86')) {
            $view=if($arch -eq 'x64'){[Microsoft.Win32.RegistryView]::Registry64}else{[Microsoft.Win32.RegistryView]::Registry32}
            $folder=if($arch -eq 'x64'){'x64'}else{'Win32'}
            $expected=Child-Path $install ('runtime/'+$folder+'/BMJ2534Endpoint_'+$uid+'_'+$port+'.dll')
            $keyPath='SOFTWARE\PassThruSupport.04.04\BUSMUST BM '+$uid+' CH'+$port
            $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
            try {
                $key=$base.OpenSubKey($keyPath,$false)
                if(-not $key){continue}
                try {
                    if($key.GetValue('BUSMUSTInstallationId') -ne $State.InstallationId -or $key.GetValue('Vendor') -ne 'BUSMUST' -or
                       $key.GetValue('FunctionLibrary') -ne $expected -or $key.GetValue('BUSMUSTBinding') -ne ($uid+':'+$port)) {
                        throw 'Preserve a legacy provider whose ownership or path changed.'
                    }
                } finally {$key.Dispose()}
                $base.DeleteSubKeyTree($keyPath,$false)
            } finally {$base.Dispose()}
        }
    }
}
function Expected-Shortcuts {
    $folder=Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'BUSMUST'
    @(
        [pscustomobject]@{Path=(Join-Path $folder 'BUSMUST J2534 Configuration.lnk');Target=(Join-Path $install 'runtime/x64/BMJ2534Config.exe');Arguments=''},
        [pscustomobject]@{Path=(Join-Path $folder 'Uninstall BUSMUST J2534.lnk');Target=(Join-Path $install 'Uninstall.cmd');Arguments=''}
    )
}
function Shortcut-Plan($State) {
    $records=if($State -and $State.PSObject.Properties['Shortcuts']){@($State.Shortcuts)}else{@()}
    foreach($link in @(Expected-Shortcuts)) {
        Assert-NoReparse $link.Path
        $old=@($records|Where-Object {$_.Path -eq $link.Path -and $_.Target -eq $link.Target -and $_.Arguments -eq $link.Arguments})
        $change=$true
        if(Test-Path -LiteralPath $link.Path) {
            if($old.Count -ne 1){throw "Preserve unowned shortcut: $($link.Path)"}
            $hash=File-Hash $link.Path
            if($old[0].Sha256 -and $old[0].Sha256 -ne $hash){throw "Preserve modified shortcut: $($link.Path)"}
            if($old[0].Sha256){$change=$false}
        }
        [pscustomobject]@{Path=$link.Path;Target=$link.Target;Arguments=$link.Arguments;ChangesRequired=$change}
    }
}
function Build-Plan {
    Assert-NoReparse $install
    $state=Read-State
    if($Action -eq 'Uninstall' -and -not $state) { throw 'The support package is not installed.' }
    if($Action -eq 'Uninstall') { return [pscustomobject]@{State=$state;ChangesRequired=$true} }
    $payloadPath=Join-Path $PSScriptRoot 'payload.json'
    $payload=Read-Json $payloadPath
    if($payload.Schema -ne 1 -or $payload.Owner -ne $owner) { throw 'Invalid support-package payload manifest.' }
    $files=New-Object 'Collections.Generic.Dictionary[string,object]' ([StringComparer]::OrdinalIgnoreCase)
    foreach($file in @($payload.Files)) {
        if(-not (Test-InstallPayload $file.Path)){continue}
        if($files.ContainsKey($file.Path)) { throw 'Duplicate installation payload entry.' }
        $source=Child-Path $PSScriptRoot $file.Path
        if(-not (Test-Path -LiteralPath $source -PathType Leaf)){throw "Missing installation file: $($file.Path)"}
        $sourceHash=File-Hash $source
        if((Test-DllPayload $file.Path) -and
           (-not $file.PSObject.Properties['Sha256'] -or $file.Sha256 -notmatch '^[0-9a-fA-F]{64}$' -or $sourceHash -ne $file.Sha256)) {
            throw "Package DLL does not match its hash: $($file.Path)"
        }
        # Non-DLL hashes record ownership/update state, not package integrity admission.
        $files.Add($file.Path,[pscustomobject]@{Path=$file.Path;Sha256=$sourceHash;Source=$source})
    }
    $requiredFiles=@('Manage-SupportPackage.ps1','Install.cmd','Uninstall.cmd','scripts/Register-BMJ2534.ps1')
    foreach($folder in @('x64','x86')) {
        $core=if($folder -eq 'x64'){'BMJ2534.dll'}else{'BMJ25332.dll'}
        foreach($name in @($core,'BMJ2534Endpoint.dll','BMJ2534Config.exe','BMAPI.dll')){$requiredFiles+='runtime/'+$folder+'/'+$name}
    }
    foreach($required in $requiredFiles){if(-not $files.ContainsKey($required)){throw "Missing required payload hash: $required"}}
    $files.Add('payload.json',[pscustomobject]@{Path='payload.json';Sha256=(File-Hash $payloadPath);Source=$payloadPath})
    $id=if($state){[guid]$state.InstallationId}else{[guid]::NewGuid()}
    $oldFiles=New-Object 'Collections.Generic.Dictionary[string,object]' ([StringComparer]::OrdinalIgnoreCase)
    if($state) { foreach($file in @($state.Files)) { [void](Child-Path $install $file.Path);$oldFiles.Add($file.Path,$file) } }
    $copy=New-Object 'Collections.Generic.List[object]'
    foreach($file in $files.Values) {
        $destination=Child-Path $install $file.Path
        if(Test-Path -LiteralPath $destination) {
            $hash=File-Hash $destination
            if($hash -eq $file.Sha256) { continue }
            if(-not (Test-OwnedFileHash $state $file.Path $hash)) { throw "Preserve modified/unowned file: $destination" }
        }
        $copy.Add($file)
    }
    $registrations=New-Object 'Collections.Generic.List[object]'
    foreach($arch in @('x64','x86')) {
        $check=Native-Registration 'Check' $arch $id
        if($check.ChangesRequired) { $registrations.Add([pscustomobject]@{Architecture=$arch}) }
    }
    if(-not $state -and (Test-Path -LiteralPath $install) -and @(Get-ChildItem -LiteralPath $install -Recurse -File -Force | Select-Object -First 1).Count) { throw 'A new installation requires a dedicated directory without existing files.' }
    $shortcuts=@(Shortcut-Plan $state)
    [pscustomobject]@{State=$state;InstallationId=$id;Files=@($files.Values | Sort-Object Path);CopyFiles=@($copy.ToArray());Registrations=@($registrations.ToArray());Shortcuts=$shortcuts;ChangesRequired=($copy.Count -gt 0 -or $registrations.Count -gt 0 -or @($shortcuts|Where-Object ChangesRequired).Count -gt 0 -or -not $state -or (Test-Path -LiteralPath $pendingPath))}
}
function Elevation-ResultPath([string]$RequestId,[string]$ParentResultPath) {
    if($RequestId -notmatch '^[0-9a-f]{32}$'){throw 'Invalid elevated-operation request ID.'}
    if($ParentResultPath) {
        # Alternate administrator credentials have another user profile. Keep
        # the parent's correlated file, whose ACL grants Administrators access.
        $path=[IO.Path]::GetFullPath($ParentResultPath)
        $folder=Split-Path -Parent $path
        if([IO.Path]::GetFileName($path) -cne ($RequestId+'.json') -or
           (Split-Path -Leaf $folder) -ne 'SetupLogs' -or
           (Split-Path -Leaf (Split-Path -Parent $folder)) -ne 'J2534' -or
           (Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $folder))) -ne 'BUSMUST') {
            throw 'Elevated result path does not match this operation.'
        }
        Assert-NoReparse $path
        return $path
    }
    $folder=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'BUSMUST/J2534/SetupLogs'
    Assert-NoReparse $folder
    if(-not (Test-Path -LiteralPath $folder)){New-Item -ItemType Directory -Path $folder -Force | Out-Null}
    $path=Join-Path $folder ($RequestId+'.json')
    Assert-NoReparse $path
    return $path
}
function Require-Administrator {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $principal=New-Object Security.Principal.WindowsPrincipal($identity)
    if($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){return $true}
    if($NoElevation){throw 'This change requires an administrator; no files or registry entries were changed.'}
    $hostExe=Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
    $requestId=[guid]::NewGuid().ToString('N')
    $resultPath=Elevation-ResultPath $requestId
    # Create the correlation file exclusively before launching the child.
    $stream=[IO.File]::Open($resultPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    $stream.Dispose()
    $acl=New-Object Security.AccessControl.FileSecurity
    $acl.SetAccessRuleProtection($true,$false)
    foreach($sid in @([Security.Principal.WindowsIdentity]::GetCurrent().User,
        (New-Object Security.Principal.SecurityIdentifier('S-1-5-18')),
        (New-Object Security.Principal.SecurityIdentifier('S-1-5-32-544')))) {
        $acl.AddAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($sid,'FullControl','Allow')))
    }
    Set-Acl -LiteralPath $resultPath -AclObject $acl
    try {
        $child=Start-Process -FilePath $hostExe -WindowStyle Hidden -Verb RunAs -Wait -PassThru -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSCommandPath+'"'),'-Action',$Action,'-InstallRoot',('"'+$install+'"'),'-NoElevation','-ElevationRequestId',$requestId,'-ElevationResultPath',('"'+$resultPath+'"'))
    } catch { throw "Could not start the administrator operation: $($_.Exception.Message) Result log: $resultPath" }
    $result=$null
    try {
        Assert-NoReparse $resultPath
        if((Get-Item -LiteralPath $resultPath).Length -gt 65536){throw 'Elevated result is too large.'}
        $result=Read-Json $resultPath
        if($result.Schema -ne 1 -or $result.RequestId -ne $requestId -or $result.Action -ne $Action -or
           $result.InstallRoot -ne $install -or $result.Success -isnot [bool] -or $result.Message -isnot [string]) {
            throw 'Elevated result does not match this operation.'
        }
    } catch { throw "Administrator operation returned exit code $($child.ExitCode), but its result could not be read: $($_.Exception.Message) Result log: $resultPath" }
    if($child.ExitCode -ne 0 -or -not $result.Success) {
        throw "Administrator operation failed ($($child.ExitCode)): $($result.Message) Result log: $resultPath"
    }
    Write-Information "Administrator operation completed. Result log: $resultPath" -InformationAction Continue
    return $false
}
$elevatedError=$null
$elevationResultFile=$null
if($ElevationRequestId) {
    if(-not $NoElevation){throw 'An elevated result ID is only valid for the administrator child.'}
    if(-not $ElevationResultPath){throw 'Missing parent elevated result path.'}
    $elevationResultFile=Elevation-ResultPath $ElevationRequestId $ElevationResultPath
    if(-not (Test-Path -LiteralPath $elevationResultFile -PathType Leaf)){throw 'Missing parent-created elevated result file.'}
}
try {
$plan=Build-Plan
if($Action -eq 'Plan') { [pscustomobject]@{Action=$Action;InstallRoot=$install;ChangesRequired=$plan.ChangesRequired;ProviderCount=2;FilesToCopy=$plan.CopyFiles.Count;RegistryEntriesToUpdate=$plan.Registrations.Count} | ConvertTo-Json;return }
if(-not $plan.ChangesRequired) { [pscustomobject]@{Result='NO_CHANGES';InstallRoot=$install;ProviderCount=2} | ConvertTo-Json;return }
if(-not (Require-Administrator)){return}
$mutex=New-Object Threading.Mutex($false,'Global\BUSMUST.INCA.J2534.Installation')
$held=$false
try {
    try { $held=$mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $held=$true }
    if(-not $held){throw 'Another BUSMUST J2534 installation operation is running.'}
    $plan=Build-Plan
    if($Action -eq 'Uninstall') {
        if(@(Get-Process -Name Inca,TgtSvr -ErrorAction SilentlyContinue).Count) { throw 'Close INCA before uninstalling the support package.' }
        $removeFiles=New-Object 'Collections.Generic.List[string]'
        $preserved=New-Object 'Collections.Generic.List[string]'
        foreach($file in @($plan.State.Files)) {
            $path=Child-Path $install $file.Path
            if(-not (Test-Path -LiteralPath $path)){continue}
            if(-not (Test-InstallPayload $file.Path)){$preserved.Add($path);continue}
            if(-not (Test-OwnedFileHash $plan.State $file.Path (File-Hash $path))) { $preserved.Add($path);continue }
            Assert-Unlocked $path;$removeFiles.Add($path)
        }
        $stagedFile=$null
        if($plan.State.PSObject.Properties['StagingPath'] -and $plan.State.StagingPath) {
            $stagedFile=(Child-Path $install $plan.State.StagingPath)+'.installing'
            Assert-NoReparse $stagedFile
            if((Test-Path -LiteralPath $stagedFile) -and -not (Test-InstallPayload $plan.State.StagingPath)) {
                $preserved.Add($stagedFile);$stagedFile=$null
            }
            if($stagedFile -and (Test-Path -LiteralPath $stagedFile)) {
                if(-not (Test-OwnedFileHash $plan.State $plan.State.StagingPath (File-Hash $stagedFile))){throw "Preserve modified staging file: $stagedFile"}
                Assert-Unlocked $stagedFile
            }
        }
        Remove-LegacyRegistrations $plan.State
        foreach($arch in @('x64','x86')) { Native-Registration 'Remove' $arch ([guid]$plan.State.InstallationId) | Out-Null }
        $expectedLinks=@(Expected-Shortcuts)
        if($plan.State.PSObject.Properties['Shortcuts']) {
            foreach($link in @($plan.State.Shortcuts)) {
                if(@($expectedLinks|Where-Object {$_.Path -eq $link.Path -and $_.Target -eq $link.Target -and $_.Arguments -eq $link.Arguments}).Count -ne 1){throw 'Invalid shortcut ownership path.'}
                Assert-NoReparse $link.Path
                if(-not (Test-Path -LiteralPath $link.Path)){continue}
                if($link.Sha256 -and (File-Hash $link.Path) -eq $link.Sha256){Remove-Item -LiteralPath $link.Path -Force}
                else {$preserved.Add($link.Path)}
            }
        }
        foreach($path in $removeFiles){Remove-Item -LiteralPath $path -Force}
        if($stagedFile -and (Test-Path -LiteralPath $stagedFile)){Remove-Item -LiteralPath $stagedFile -Force}
        if(Test-Path -LiteralPath $pendingPath){Remove-Item -LiteralPath $pendingPath -Force}
        if(-not $preserved.Count){Remove-Item -LiteralPath $statePath -Force -ErrorAction SilentlyContinue}
        [pscustomobject]@{Result='UNINSTALLED';InstallRoot=$install;Preserved=@($preserved.ToArray());Note='Extra/modified user files and empty directories are preserved.'} | ConvertTo-Json
        return
    }
    $changingRuntime=@($plan.CopyFiles | Where-Object {$_.Path -like 'runtime/*' -and (Test-Path -LiteralPath (Child-Path $install $_.Path))})
    if($changingRuntime.Count -and @(Get-Process -Name Inca,TgtSvr -ErrorAction SilentlyContinue).Count) { throw 'Close INCA before updating installed runtime files.' }
    foreach($file in $changingRuntime){Assert-Unlocked (Child-Path $install $file.Path)}
    if(-not (Test-Path -LiteralPath $install)){New-Item -ItemType Directory -Path $install | Out-Null}
    $fileRecords=@($plan.Files | Select-Object Path,Sha256)
    if($plan.State) {
        $desired=@($fileRecords.Path)
        $fileRecords+=@($plan.State.Files | Where-Object {$_.Path -notin $desired})
    }
    $priorStage=if($plan.State -and $plan.State.PSObject.Properties['StagingPath']){$plan.State.StagingPath}else{$null}
    $previousFiles=@()
    if($plan.State){$previousFiles+=@($plan.State.Files);if($plan.State.PSObject.Properties['PreviousFiles']){$previousFiles+=@($plan.State.PreviousFiles)}}
    $legacy=if($plan.State -and $plan.State.PSObject.Properties['Bindings']){@($plan.State.Bindings)}else{@()}
    $oldShortcuts=if($plan.State -and $plan.State.PSObject.Properties['Shortcuts']){@($plan.State.Shortcuts)}else{@()}
    $state=[pscustomobject]@{Schema=2;Owner=$owner;InstallRoot=$install;InstallationId=$plan.InstallationId.ToString();Bindings=$legacy;Files=$fileRecords;PreviousFiles=$previousFiles;StagingPath=$null;Shortcuts=$oldShortcuts}
    Write-JsonAtomic $pendingPath $state
    foreach($file in $plan.CopyFiles) {
        $destination=Child-Path $install $file.Path;$parent=Split-Path -Parent $destination
        if(-not (Test-Path -LiteralPath $parent)){New-Item -ItemType Directory -Path $parent -Force | Out-Null}
        $temporary=$destination+'.installing'
        if((Test-Path -LiteralPath $temporary) -and (File-Hash $temporary) -ne $file.Sha256) {
            if($priorStage -ne $file.Path){throw "Preserve unexpected staging file: $temporary"}
            Remove-Item -LiteralPath $temporary -Force
        }
        $state.StagingPath=$file.Path;Write-JsonAtomic $pendingPath $state
        if(-not (Test-Path -LiteralPath $temporary)){Copy-Item -LiteralPath $file.Source -Destination $temporary}
        if((Test-DllPayload $file.Path) -and (File-Hash $temporary) -ne $file.Sha256){throw 'Staged DLL hash differs.'}
        if(Test-Path -LiteralPath $destination){[IO.File]::Replace($temporary,$destination,[NullString]::Value)}else{[IO.File]::Move($temporary,$destination)}
    }
    foreach($entry in $plan.Registrations) { Native-Registration 'Install' $entry.Architecture $plan.InstallationId | Out-Null }
    foreach($arch in @('x64','x86')) { if((Native-Registration 'Check' $arch $plan.InstallationId).ChangesRequired){throw 'Installed registration readback failed.'} }
    foreach($file in $plan.Files) { if((Test-DllPayload $file.Path) -and (File-Hash (Child-Path $install $file.Path)) -ne $file.Sha256){throw 'Installed DLL readback failed.'} }
    Remove-LegacyRegistrations $state
    $obsoletePreserved=@()
    foreach($file in @($state.Files|Where-Object {$_.Path -notin @($plan.Files.Path)})) {
        $path=Child-Path $install $file.Path
        if(-not (Test-Path -LiteralPath $path)){continue}
        if(-not (Test-InstallPayload $file.Path)){$obsoletePreserved+=$file;continue}
        if((File-Hash $path) -eq $file.Sha256){Assert-Unlocked $path;Remove-Item -LiteralPath $path -Force}
        else {$obsoletePreserved+=$file}
    }
    $shell=New-Object -ComObject WScript.Shell
    foreach($link in $plan.Shortcuts) {
        if(-not $link.ChangesRequired){continue}
        $record=[pscustomobject]@{Path=$link.Path;Target=$link.Target;Arguments=$link.Arguments;Sha256=$null}
        $state.Shortcuts=@($state.Shortcuts|Where-Object {$_.Path -ne $link.Path})+@($record)
        Write-JsonAtomic $pendingPath $state
        $parent=Split-Path -Parent $link.Path
        if(-not (Test-Path -LiteralPath $parent)){New-Item -ItemType Directory -Path $parent -Force|Out-Null}
        $shortcut=$shell.CreateShortcut($link.Path)
        $shortcut.TargetPath=$link.Target;$shortcut.Arguments=$link.Arguments;$shortcut.WorkingDirectory=$install
        $shortcut.IconLocation=(Join-Path $install 'runtime/x64/BMJ2534Config.exe')+',0';$shortcut.Save()
        $readback=$shell.CreateShortcut($link.Path)
        if($readback.TargetPath -ne $link.Target -or $readback.Arguments -ne $link.Arguments){throw 'Shortcut target readback failed.'}
        $record.Sha256=File-Hash $link.Path
        Write-JsonAtomic $pendingPath $state
    }
    $state.Files=@($plan.Files|Select-Object Path,Sha256)+$obsoletePreserved
    $state.Bindings=@()
    $state.StagingPath=$null
    $state.PreviousFiles=@()
    Write-JsonAtomic $statePath $state
    Remove-Item -LiteralPath $pendingPath -Force
    [pscustomobject]@{Result='INSTALLED';InstallRoot=$install;ProviderCount=2;FilesCopied=$plan.CopyFiles.Count;RegistryEntriesUpdated=$plan.Registrations.Count;PreservedObsoleteFiles=@($obsoletePreserved|ForEach-Object {$_.Path})} | ConvertTo-Json
} finally { if($held){$mutex.ReleaseMutex()};$mutex.Dispose() }
} catch { $elevatedError=$_;throw }
finally {
    if($elevationResultFile) {
        try {
            Assert-NoReparse $elevationResultFile
            $result=[pscustomobject]@{Schema=1;RequestId=$ElevationRequestId;Action=$Action;InstallRoot=$install;
                Success=($null -eq $elevatedError);Message=$(if($elevatedError){$elevatedError.Exception.Message.Substring(0,[Math]::Min(8192,$elevatedError.Exception.Message.Length))}else{'Operation completed.'});
                ScriptStack=$(if($elevatedError){([string]$elevatedError.ScriptStackTrace).Substring(0,[Math]::Min(8192,([string]$elevatedError.ScriptStackTrace).Length))}else{''})}
            [IO.File]::WriteAllText($elevationResultFile,($result | ConvertTo-Json -Depth 3),(New-Object Text.UTF8Encoding($false)))
        } catch { Write-Warning "Could not record administrator result: $($_.Exception.Message)" }
    }
}

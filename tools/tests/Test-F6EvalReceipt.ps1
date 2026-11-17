#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Artifact,
    [Parameter(Mandatory)][string]$DlssRuntime,
    [ValidateSet('interop','release','skip','native','before-sr','before-sr-typeless','before-sr-auto','before-sr-default','before-sr-preloaded','before-sr-managed','before-sr-managed-default','before-sr-managed-off','before-sr-managed-missing','before-sr-managed-bad','before-sr-off','before-sr-other','legacy-shutdown','before-sr-caller-srgb','before-sr-caller-unknown','before-sr-caller-linear','before-sr-caller-pq','before-sr-caller-wrong-view','before-sr-caller-wrong-color','before-sr-caller-wrong-extent','before-sr-caller-closed-scope','before-sr-neo-unknown','before-sr-neo-guide-reject','before-sr-neo-srgb','before-sr-neo-gamma-reject')][string[]]$Cases = @('interop','skip','native','before-sr','before-sr-typeless','before-sr-auto','before-sr-default','before-sr-off','before-sr-other'),
    [switch]$IncludeShutdown,
    [string]$OutputDirectory,
    [string]$CorePath = 'D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/HoYoShade/ReShade64.dll',
    [string]$NeoRoot,
    [string]$NeoArtifact,
    [string]$DevCmd = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$Artifact) { $Artifact = Join-Path $root 'build/out/OptiScaler.dll' }
$artifactPath = (Get-Item -LiteralPath $Artifact).FullName
$runtimePath = (Get-Item -LiteralPath $DlssRuntime).FullName
$runRoot = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory, $root) }
    else { Join-Path $root ('build/f6-eval-v1-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff')) }
$null = [IO.Directory]::CreateDirectory($runRoot)
$binaryRoot = Join-Path $runRoot 'inputs'
$null = [IO.Directory]::CreateDirectory($binaryRoot)
# Freeze each immutable binary once; every case links within this same volume.
# Separate fixture INIs and outputs remain independent.
$fixtureArtifact = Join-Path $binaryRoot 'OptiScaler.dll'
$fixtureRuntime = Join-Path $binaryRoot 'nvngx_dlss.dll'
Copy-Item -LiteralPath $artifactPath -Destination $fixtureArtifact
Copy-Item -LiteralPath $runtimePath -Destination $fixtureRuntime
$fixtureCore = Join-Path $binaryRoot 'ReShade64.dll'
$exe = Join-Path $runRoot 'F6EvalReceiptHost.exe'
$hostSnapshot = Join-Path $runRoot 'host-source'
$hostInputs = @()
foreach ($relative in @('F6EvalReceiptV1.h','NeoBeforeSrV1.h','tests/F6EvalReceiptHost.cpp',
    'tests/NeoBeforeSrCallerFixture.h','tests/NeoBeforeSrCallerFixture.cpp','tests/F6NeoBeforeSrHost.inl')) {
    $original = Join-Path $root "OptiScaler/inputs/$relative"
    $target = Join-Path $hostSnapshot "OptiScaler/inputs/$relative"
    $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
    Copy-Item -LiteralPath $original -Destination $target
    $hash = (Get-FileHash -LiteralPath $target).Hash
    if ($hash -ne (Get-FileHash -LiteralPath $original).Hash) { throw "F6 host source changed during freeze: $relative" }
    $hostInputs += [pscustomobject]@{Path="OptiScaler/inputs/$relative"; SHA256=$hash}
}
$hostInputs | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot 'host-source.json') -Encoding utf8
$source = Join-Path $hostSnapshot 'OptiScaler/inputs/tests/F6EvalReceiptHost.cpp'
$sdk = Join-Path $root 'external/nvngx_dlss_sdk'
$compileLog = Join-Path $runRoot 'compile.log'
$realNeo = @($Cases | Where-Object { $_.StartsWith('before-sr-neo-') }).Count -ne 0
$needsCaller = @($Cases | Where-Object { $_.StartsWith('before-sr-caller-') }).Count -ne 0
$callerDll = Join-Path $binaryRoot 'NeoBeforeSrCallerFixture.dll'
if ($realNeo -or $needsCaller) {
    if (!$NeoRoot) { throw 'Production-query caller cases require -NeoRoot (Neo checkout directory)' }
    if ($IncludeShutdown) { throw 'Caller cases do not exercise vendor Shutdown1' }
    $neoSource = (Get-Item -LiteralPath (Join-Path $NeoRoot 'Dx11FsrBridge')).FullName
    $neoSnapshot = Join-Path $runRoot 'neo-source'
    $manifest = @()
    # Freeze the unmodified producer query and its local header closure before
    # compiling; parallel bridge development cannot change this test's inputs.
    foreach ($file in (Get-ChildItem -LiteralPath $neoSource -Recurse -File | Where-Object {
        $_.Extension -in @('.h','.hpp','.inl') -or $_.FullName -eq (Join-Path $neoSource 'ngx/BeforeSr.cpp')
    })) {
        $relative = [IO.Path]::GetRelativePath($neoSource, $file.FullName)
        $target = Join-Path $neoSnapshot $relative
        $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
        Copy-Item -LiteralPath $file.FullName -Destination $target
        $hash = (Get-FileHash -LiteralPath $target).Hash
        if ($hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) { throw "Neo source changed during freeze: $relative" }
        $manifest += [pscustomobject]@{Path=$relative; SHA256=$hash}
    }
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runRoot 'neo-source.json') -Encoding utf8
    if ($needsCaller) {
        $callerSource = Join-Path $hostSnapshot 'OptiScaler/inputs/tests/NeoBeforeSrCallerFixture.cpp'
        $querySource = Join-Path $neoSnapshot 'ngx/BeforeSr.cpp'
        $command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od /LD /I"{1}" /I"{2}" /Fe:"{3}" /Fo:"{4}/" "{5}" "{6}" /link d3d11.lib' -f $DevCmd,$sdk,$neoSnapshot,$callerDll,$runRoot,$callerSource,$querySource
        & cmd.exe /d /s /c $command *> (Join-Path $runRoot 'compile-caller.log')
        if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath (Join-Path $runRoot 'compile-caller.log'); throw 'Neo production-query caller compilation failed' }
    }
}
$neoCompile = ''
if ($realNeo) {
    if (!$NeoArtifact) { throw 'Actual Neo case requires -NeoArtifact' }
    Copy-Item -LiteralPath $NeoArtifact -Destination (Join-Path $binaryRoot 'Dx11FsrBridge.dll')
    $neoCompile = '/DF6_NEO_BEFORE_SR_HOST /I"' + $neoSnapshot + '"'
}
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od {5} /I"{1}" /Fe:"{2}" /Fo:"{3}/" "{4}" /link d3d11.lib d3dcompiler.lib user32.lib dxgi.lib' -f $DevCmd,$sdk,$exe,$runRoot,$source,$neoCompile
& cmd.exe /d /s /c $command *> $compileLog
if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $compileLog; throw 'F6 V1 host compilation failed' }
$rows = @()
function Get-FixtureSelection([string]$Directory)
{
    # Compare authored values, allowing the core to add defaults and normalize
    # path spelling. Byte hashes would mistake these changes for lost settings.
    $values = @{}
    foreach ($name in @('ReShade.ini','preset.ini')) {
        $section = ''
        foreach ($line in [IO.File]::ReadAllLines((Join-Path $Directory $name))) {
            if ($line -match '^\s*\[([^]]+)\]\s*$') { $section = $Matches[1]; continue }
            if ($line -match '^\s*([^;=]+?)\s*=(.*)$') {
                $values["${name}/${section}/$($Matches[1].Trim())"] = $Matches[2].Trim()
            }
        }
    }
    [ordered]@{
        EffectSearchPaths = [IO.Path]::GetFullPath($values['ReShade.ini/GENERAL/EffectSearchPaths'], $Directory)
        PresetPath = [IO.Path]::GetFullPath($values['ReShade.ini/GENERAL/PresetPath'], $Directory)
        TutorialProgress = $values['ReShade.ini/OVERLAY/TutorialProgress']
        Techniques = $values['preset.ini//Techniques']
        TechniqueSorting = $values['preset.ini//TechniqueSorting']
    } | ConvertTo-Json -Compress
}
foreach ($case in $Cases)
{
    $path = Join-Path $runRoot $case
    $null = [IO.Directory]::CreateDirectory($path)
    $null = New-Item -ItemType HardLink -Path (Join-Path $path 'OptiScaler.dll') -Target $fixtureArtifact
    $null = New-Item -ItemType HardLink -Path (Join-Path $path 'nvngx_dlss.dll') -Target $fixtureRuntime
    $callerFixture = $case.StartsWith('before-sr-caller-')
    $neoFixture = $case.StartsWith('before-sr-neo-')
    $managedFixture = $neoFixture -or $callerFixture -or $case.StartsWith('before-sr-managed')
    $expectsAdmission = $case -in @('before-sr-typeless','before-sr-auto','before-sr-default','before-sr-preloaded','before-sr-managed','before-sr-managed-default','before-sr-caller-srgb','before-sr-neo-srgb')
    if ($expectsAdmission -or $managedFixture) {
        $coreHash = (Get-FileHash -LiteralPath $CorePath -Algorithm SHA256).Hash
        if ($coreHash -ne '0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7') {
            throw 'Typeless receipt host requires the pinned ReShade core'
        }
        $null = [IO.Directory]::CreateDirectory((Join-Path $path 'HoYoShade'))
        if ($case -eq 'before-sr-managed-bad') {
            [IO.File]::WriteAllText((Join-Path $path 'HoYoShade/ReShade64.dll'), 'wrong core identity fixture')
        } elseif ($case -ne 'before-sr-managed-missing') {
            if (!(Test-Path -LiteralPath $fixtureCore)) { Copy-Item -LiteralPath $CorePath -Destination $fixtureCore }
            $null = New-Item -ItemType HardLink -Path (Join-Path $path 'HoYoShade/ReShade64.dll') -Target $fixtureCore
        }
        if ($managedFixture) {
            Copy-Item -LiteralPath (Join-Path $root 'OptiScaler/integration/tests/before-sr/Probe.fx') -Destination $path
            [IO.File]::WriteAllText((Join-Path $path 'preset.ini'), "Techniques=Probe@Probe.fx`nTechniqueSorting=Probe@Probe.fx`n")
            [IO.File]::WriteAllText((Join-Path $path 'ReShade.ini'), "[GENERAL]`nEffectSearchPaths=$path`nPresetPath=$path/preset.ini`n[OVERLAY]`nTutorialProgress=4`n")
        }
    }
    $backend = if ($case -eq 'native') { 'dlss' } else { 'dlss_12' }
    $skip = if ($case -eq 'skip') { '2' } else { 'auto' }
    $enabledLine = switch ($case) {
        { $_.StartsWith('before-sr-caller-') -or $_.StartsWith('before-sr-neo-') } { 'Enabled=true' }
        { $_ -in @('before-sr-default','before-sr-managed-default') } { '' } # Absent key, not an explicit false.
        'before-sr-auto' { 'Enabled=auto' }
        { $_ -in @('before-sr','before-sr-typeless','before-sr-preloaded','before-sr-other','before-sr-managed','before-sr-managed-missing','before-sr-managed-bad') } { 'Enabled=true' }
        default { 'Enabled=false' }
    }
    $stage = if ($case -eq 'before-sr-other') { 2 } else { 1 }
    # Only this disposable host's settings. No installed profile is read or
    # changed; all feature DLL lookup paths remain within this fixture.
    $ini = @"
[Upscalers]
Dx11Upscaler=$backend
[FrameGen]
Enabled=false
[DlssNr]
Enabled=false
[HoYoShade]
$enabledLine
Stage=$stage
[CAS]
Enabled=false
[OutputScaling]
Enabled=false
[Plugins]
Path=$path/plugins
LoadAsiPlugins=false
[Libraries]
OptiDllPath=$path
NvngxDlssPath=$path
[Hotfix]
SkipFirstFrames=$skip
[Log]
LogToFile=true
LogToConsole=false
LogToNGX=true
LogLevel=1
LogAsync=false
LogFileName=./OptiScaler.log
"@
    [IO.File]::WriteAllText((Join-Path $path 'OptiScaler.ini'), $ini, [Text.UTF8Encoding]::new($false))
    $hostArguments = @('OptiScaler.dll', $case)
    if ($callerFixture) {
        $null = New-Item -ItemType HardLink -Path (Join-Path $path 'NeoBeforeSrCallerFixture.dll') -Target $callerDll
        $hostArguments += 'NeoBeforeSrCallerFixture.dll'
    }
    if ($neoFixture) {
        $null = New-Item -ItemType HardLink -Path (Join-Path $path 'Dx11FsrBridge.dll') -Target (Join-Path $binaryRoot 'Dx11FsrBridge.dll')
    }
    if ($IncludeShutdown) { $hostArguments += 'shutdown' }
    $selectionBefore = if ($managedFixture) { Get-FixtureSelection $path } else { $null }
    $caseExe = $exe
    if ($managedFixture) {
        $caseExe = Join-Path $path 'F6EvalReceiptHost.exe'
        Copy-Item -LiteralPath $exe -Destination $caseExe
    }
    $process = Start-Process -FilePath $caseExe -ArgumentList $hostArguments -WorkingDirectory $path `
        -RedirectStandardOutput (Join-Path $path 'stdout.log') -RedirectStandardError (Join-Path $path 'stderr.log') `
        -WindowStyle Hidden -PassThru
    $null = $process.Handle
    $complete = $process.WaitForExit(60000)
    if (!$complete) { $process.Kill(); $process.WaitForExit() }
    $stdout = [IO.File]::ReadAllText((Join-Path $path 'stdout.log'))
    $passed = $complete -and $process.ExitCode -eq 0 -and $stdout.Contains('PASS actual F6 V1 ABI')
    $admission = @()
    if ($expectsAdmission) {
        # SR pixels alone do not prove that BeforeSR admitted this input. The
        # isolated host has no natural ReShade donor; verify the production
        # admission separately without claiming that effects executed.
        $logPath = Join-Path $path 'OptiScaler.log'
        if (Test-Path -LiteralPath $logPath) {
            $admission = @(Get-Content -LiteralPath $logPath | Where-Object {
                $_ -match 'HoYoShade BeforeSR input .*source_format=27 .*input_query=current-input input_view=28 .*producer_storage=27 .*producer_color=producer-sdr .*sdr=true '
            })
        }
        $passed = $passed -and $(if ($managedFixture) { $admission.Count -ge 6 } else { $admission.Count -eq 6 })
    }
    $effects = @()
    if ($case -eq 'before-sr-neo-srgb') {
        $effects = @(Select-String -LiteralPath (Join-Path $path 'OptiScaler.log') `
            -Pattern 'HoYoShade stage owned render submitted .*effects_executed=true' | ForEach-Object Line)
        # Production execution logs are sampled; all six filtered outputs are
        # independently checked by the host, so one real submission proves the path.
        $passed = $passed -and $effects.Count -ge 1
    }
    $rejection = @()
    if (($neoFixture -or $callerFixture) -and !$expectsAdmission) {
        $logPath = Join-Path $path 'OptiScaler.log'
        if (Test-Path -LiteralPath $logPath) {
            $reason = switch ($case) {
                { $_ -in @('before-sr-caller-linear','before-sr-caller-pq') } { 'current-input .*producer_color=producer-transfer-unsupported' }
                { $_ -in @('before-sr-caller-wrong-color','before-sr-caller-closed-scope') } { 'no-current-match .*producer_color=legacy-input' }
                'before-sr-caller-wrong-extent' { 'invalid-description .*producer_color=invalid-producer-description' }
                default { 'current-input .*producer_color=producer-transfer-unknown' }
            }
            $rejection = @(Select-String -LiteralPath $logPath -Pattern ('HoYoShade BeforeSR input .*input_query=' + $reason + ' .*sdr=false ') | ForEach-Object Line)
            $executed = Select-String -LiteralPath $logPath -Pattern 'HoYoShade stage owned render submitted .*effects_executed=true' -Quiet
            $passed = $passed -and $rejection.Count -eq 6 -and !$executed
        } else { $passed = $false }
    }
    $configCount = $null
    $selectionPreserved = $null
    if ($managedFixture) {
        $configs = @(Get-ChildItem -LiteralPath $path -Filter 'ReShade*.ini' -File -Recurse)
        $configCount = $configs.Count
        $selectionPreserved = $selectionBefore -eq (Get-FixtureSelection $path)
        $passed = $passed -and $configCount -eq 1 -and $configs[0].FullName -eq (Join-Path $path 'ReShade.ini') -and $selectionPreserved
        if ($case -in @('before-sr-managed-missing','before-sr-managed-bad')) {
            $passed = $passed -and (Select-String -LiteralPath (Join-Path $path 'OptiScaler.log') `
                -SimpleMatch 'result=core-disk-identity-rejected accepted=false' -Quiet)
        }
    }
    $rows += [pscustomobject]@{
        Case=$case; Path=$path; PID=$process.Id; Exit=$process.ExitCode; Timeout=(!$complete); Pass=$passed
        TypelessSdrAdmissions=$admission.Count
        MainIniCount=$configCount; AuthoredSelectionPreserved=$selectionPreserved
        AdmissionEvidence=$admission
        EffectsExecuted=$effects.Count; EffectsEvidence=$effects
        VendorShutdownExercised=([bool]$IncludeShutdown -or $case -eq 'legacy-shutdown')
        ArtifactSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'OptiScaler.dll')).Hash
        CallerSHA256=$(if ($callerFixture) { (Get-FileHash -LiteralPath $callerDll).Hash } else { $null })
        NeoSHA256=$(if ($neoFixture) { (Get-FileHash -LiteralPath (Join-Path $path 'Dx11FsrBridge.dll')).Hash } else { $null })
        RejectionEvidence=$rejection
        RuntimeSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'nvngx_dlss.dll')).Hash
        IniSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'OptiScaler.ini')).Hash
        Evidence=@($stdout -split '\r?\n' | Where-Object { $_ -match '^(receipt |target |before_sr |neo |PASS |FAIL:)' })
    }
    $process.Dispose()
    $rows | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'result.json') -Encoding utf8
    $rows[-1] | Select-Object Case,PID,Exit,Timeout,Pass
}
Write-Output "Receipt: $runRoot/result.json"
if (@($rows | Where-Object { !$_.Pass }).Count) { exit 1 }

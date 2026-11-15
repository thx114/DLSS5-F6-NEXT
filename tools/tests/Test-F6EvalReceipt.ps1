#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Artifact,
    [Parameter(Mandatory)][string]$DlssRuntime,
    [ValidateSet('interop','skip','native','before-sr','before-sr-typeless','before-sr-auto','before-sr-default','before-sr-preloaded','before-sr-managed','before-sr-managed-default','before-sr-managed-off','before-sr-managed-missing','before-sr-managed-bad','before-sr-off','before-sr-other','legacy-shutdown')][string[]]$Cases = @('interop','skip','native','before-sr','before-sr-typeless','before-sr-auto','before-sr-default','before-sr-off','before-sr-other'),
    [switch]$IncludeShutdown,
    [string]$OutputDirectory,
    [string]$CorePath = 'D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/HoYoShade/ReShade64.dll',
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
$exe = Join-Path $runRoot 'F6EvalReceiptHost.exe'
$source = Join-Path $root 'OptiScaler/inputs/tests/F6EvalReceiptHost.cpp'
$sdk = Join-Path $root 'external/nvngx_dlss_sdk'
$compileLog = Join-Path $runRoot 'compile.log'
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od /I"{1}" /Fe:"{2}" /Fo:"{3}/" "{4}" /link d3d11.lib' -f $DevCmd,$sdk,$exe,$runRoot,$source
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
    Copy-Item -LiteralPath $artifactPath -Destination (Join-Path $path 'OptiScaler.dll')
    Copy-Item -LiteralPath $runtimePath -Destination (Join-Path $path 'nvngx_dlss.dll')
    $managedFixture = $case.StartsWith('before-sr-managed')
    $expectsAdmission = $case -in @('before-sr-typeless','before-sr-auto','before-sr-default','before-sr-preloaded','before-sr-managed','before-sr-managed-default')
    if ($expectsAdmission -or $managedFixture) {
        $coreHash = (Get-FileHash -LiteralPath $CorePath -Algorithm SHA256).Hash
        if ($coreHash -ne '0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7') {
            throw 'Typeless receipt host requires the pinned ReShade core'
        }
        $null = [IO.Directory]::CreateDirectory((Join-Path $path 'HoYoShade'))
        if ($case -eq 'before-sr-managed-bad') {
            [IO.File]::WriteAllText((Join-Path $path 'HoYoShade/ReShade64.dll'), 'wrong core identity fixture')
        } elseif ($case -ne 'before-sr-managed-missing') {
            Copy-Item -LiteralPath $CorePath -Destination (Join-Path $path 'HoYoShade/ReShade64.dll')
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
        VendorShutdownExercised=([bool]$IncludeShutdown -or $case -eq 'legacy-shutdown')
        ArtifactSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'OptiScaler.dll')).Hash
        RuntimeSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'nvngx_dlss.dll')).Hash
        IniSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'OptiScaler.ini')).Hash
        Evidence=@($stdout -split '\r?\n' | Where-Object { $_ -match '^(receipt |target |before_sr |PASS |FAIL:)' })
    }
    $process.Dispose()
    $rows | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'result.json') -Encoding utf8
    $rows[-1] | Select-Object Case,PID,Exit,Timeout,Pass
}
Write-Output "Receipt: $runRoot/result.json"
if (@($rows | Where-Object { !$_.Pass }).Count) { exit 1 }

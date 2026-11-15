#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Artifact,
    [Parameter(Mandatory)][string]$DlssRuntime,
    [ValidateSet('interop','skip','native','legacy-shutdown')][string[]]$Cases = @('interop','skip','native'),
    [switch]$IncludeShutdown,
    [string]$DevCmd = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$Artifact) { $Artifact = Join-Path $root 'build/out/OptiScaler.dll' }
$artifactPath = (Get-Item -LiteralPath $Artifact).FullName
$runtimePath = (Get-Item -LiteralPath $DlssRuntime).FullName
$runRoot = Join-Path $root ('build/f6-eval-v1-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$null = [IO.Directory]::CreateDirectory($runRoot)
$exe = Join-Path $runRoot 'F6EvalReceiptHost.exe'
$source = Join-Path $root 'OptiScaler/inputs/tests/F6EvalReceiptHost.cpp'
$sdk = Join-Path $root 'external/nvngx_dlss_sdk'
$compileLog = Join-Path $runRoot 'compile.log'
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od /I"{1}" /Fe:"{2}" /Fo:"{3}/" "{4}" /link d3d11.lib' -f $DevCmd,$sdk,$exe,$runRoot,$source
& cmd.exe /d /s /c $command *> $compileLog
if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $compileLog; throw 'F6 V1 host compilation failed' }
$rows = @()
foreach ($case in $Cases)
{
    $path = Join-Path $runRoot $case
    $null = [IO.Directory]::CreateDirectory($path)
    Copy-Item -LiteralPath $artifactPath -Destination (Join-Path $path 'OptiScaler.dll')
    Copy-Item -LiteralPath $runtimePath -Destination (Join-Path $path 'nvngx_dlss.dll')
    $backend = if ($case -eq 'native') { 'dlss' } else { 'dlss_12' }
    $skip = if ($case -eq 'skip') { '2' } else { 'auto' }
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
Enabled=false
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
LogLevel=1
LogAsync=false
LogFileName=./OptiScaler.log
"@
    [IO.File]::WriteAllText((Join-Path $path 'OptiScaler.ini'), $ini, [Text.UTF8Encoding]::new($false))
    $hostArguments = @('OptiScaler.dll', $case)
    if ($IncludeShutdown) { $hostArguments += 'shutdown' }
    $process = Start-Process -FilePath $exe -ArgumentList $hostArguments -WorkingDirectory $path `
        -RedirectStandardOutput (Join-Path $path 'stdout.log') -RedirectStandardError (Join-Path $path 'stderr.log') `
        -WindowStyle Hidden -PassThru
    $null = $process.Handle
    $complete = $process.WaitForExit(60000)
    if (!$complete) { $process.Kill(); $process.WaitForExit() }
    $stdout = [IO.File]::ReadAllText((Join-Path $path 'stdout.log'))
    $passed = $complete -and $process.ExitCode -eq 0 -and $stdout.Contains('PASS actual F6 V1 ABI')
    $rows += [pscustomobject]@{
        Case=$case; Path=$path; PID=$process.Id; Exit=$process.ExitCode; Timeout=(!$complete); Pass=$passed
        VendorShutdownExercised=([bool]$IncludeShutdown -or $case -eq 'legacy-shutdown')
        ArtifactSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'OptiScaler.dll')).Hash
        RuntimeSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'nvngx_dlss.dll')).Hash
        IniSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'OptiScaler.ini')).Hash
        Evidence=@($stdout -split '\r?\n' | Where-Object { $_ -match '^(receipt |target |PASS |FAIL:)' })
    }
    $process.Dispose()
    $rows | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $runRoot 'result.json') -Encoding utf8
    $rows[-1] | Select-Object Case,PID,Exit,Timeout,Pass
}
Write-Output "Receipt: $runRoot/result.json"
if (@($rows | Where-Object { !$_.Pass }).Count) { exit 1 }

#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Artifact,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$OutputDirectory = ('build/f6-presentation-' + (Get-Date -Format 'yyyyMMdd-HHmmss')),
    [switch]$Sentinel,
    [switch]$RequireStableNr,
    [switch]$ReferenceRuntime,
    [switch]$FrameGeneration
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$out = [IO.Path]::GetFullPath($OutputDirectory, $root)
if (Test-Path -LiteralPath $out) { throw 'Use a new fixture directory' }
$null = [IO.Directory]::CreateDirectory($out)
Copy-Item -LiteralPath $Artifact -Destination (Join-Path $out 'OptiScaler.dll')
$runtimeRoot = [IO.Path]::GetFullPath($RuntimeDirectory, $root)
$libraries = if ($ReferenceRuntime) { $runtimeRoot } else { $out }
foreach ($name in @('nvngx_dlss.dll','nvngx_dlssnr.dll','nvngx.dll_dlssnr.dll')) {
    # Immutable vendor binaries; avoid duplicating multi-GB NR models for
    # each observation group. Failure is explicit, with no silent copy fallback.
    if (!$ReferenceRuntime) { $null = New-Item -ItemType HardLink -Path (Join-Path $out $name) -Target (Join-Path $runtimeRoot $name) }
}
$streamline = Join-Path $RuntimeDirectory 'streamline'
if (!$ReferenceRuntime -and $FrameGeneration -and (Test-Path -LiteralPath $streamline)) {
    foreach ($file in Get-ChildItem -LiteralPath $streamline -File -Recurse) {
        $relative = [IO.Path]::GetRelativePath($RuntimeDirectory, $file.FullName)
        $destination = Join-Path $out $relative
        $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination))
        $null = New-Item -ItemType HardLink -Path $destination -Target $file.FullName
    }
}
$exe = Join-Path $out 'YuanShen.exe' # synthetic fixture, required by the existing process route
$fg = if ($FrameGeneration) { 'true' } else { 'false' }
$nr = if ($Sentinel) { 'false' } else { 'true' }
$ini = @"
[Upscalers]
Dx11Upscaler=dlss_12
[FrameGen]
Enabled=$fg
FGInput=Upscaler
FGOutput=DLSSG
External=false
[DlssNr]
Enabled=$nr
GenshinPresentPipeline=true
PresentedTemporalDetail=false
[HoYoShade]
Enabled=false
[CAS]
Enabled=false
[OutputScaling]
Enabled=false
[Plugins]
Path=$out/plugins
LoadAsiPlugins=false
[Libraries]
OptiDllPath=$libraries
NvngxDlssPath=$libraries
[Hotfix]
DisableOverlays=true
[Log]
LogLevel=0
LogToFile=true
OpenConsole=false
LogFileName=./OptiScaler.log
"@
[IO.File]::WriteAllText((Join-Path $out 'OptiScaler.ini'), $ini)
$source = Join-Path $root 'OptiScaler/inputs/tests/F6EvalReceiptHost.cpp'
$batch = @"
@echo off
call "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cl /nologo /utf-8 /std:c++20 /EHsc /Od /DF6_PRESENTATION_HOST /DNOMINMAX /I"$root/external/nvngx_dlss_sdk" "$source" /Fe:"$exe" /Fo:"$out/" /link d3d11.lib d3d12.lib dxgi.lib user32.lib
exit /b %errorlevel%
"@
$compile = Join-Path $out 'compile.cmd'
[IO.File]::WriteAllText($compile, $batch.Replace("`r`n", "`n").Replace("`n", "`r`n"))
& cmd.exe /d /c $compile *> (Join-Path $out 'compile.log')
if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $out 'compile.log') -Tail 20; throw 'Host compile failed' }
$mode = if ($Sentinel) { 'present-sentinel' } else { 'present' }
$priorStable = [Environment]::GetEnvironmentVariable('F6_PRESENTATION_REQUIRE_STABLE', 'Process')
try {
    [Environment]::SetEnvironmentVariable('F6_PRESENTATION_REQUIRE_STABLE', $(if ($RequireStableNr) { '1' } else { $null }), 'Process')
    $p = Start-Process -FilePath $exe -ArgumentList @('OptiScaler.dll', $mode) -WorkingDirectory $out -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $out 'stdout.log') -RedirectStandardError (Join-Path $out 'stderr.log')
} finally { [Environment]::SetEnvironmentVariable('F6_PRESENTATION_REQUIRE_STABLE', $priorStable, 'Process') }
$null = $p.Handle
$complete = $p.WaitForExit(180000)
if (!$complete) { $p.Kill(); $p.WaitForExit() } # only this disposable fixture
$stdout = Get-Content -LiteralPath (Join-Path $out 'stdout.log') -Raw
$result = [pscustomobject]@{
    Pass=($complete -and $p.ExitCode -eq 0 -and $stdout.Contains('PASS actual presentation observation'))
    PID=$p.Id; Exit=$p.ExitCode; Timeout=(!$complete); Sentinel=[bool]$Sentinel; RequireStableNr=[bool]$RequireStableNr; RequestedFG=[bool]$FrameGeneration
    ArtifactSHA256=(Get-FileHash -LiteralPath (Join-Path $out 'OptiScaler.dll')).Hash
    Runtime=(Get-FileHash -LiteralPath (Join-Path $libraries 'nvngx_dlssnr.dll')).Hash
    Path=$out; Scope='isolated wrapper FG-input readback; generated/display pixels unknown'
}
$result | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'result.json')
$result
if (!$result.Pass) { Get-Content -LiteralPath (Join-Path $out 'stderr.log') -Tail 10; throw 'Presentation fixture failed' }

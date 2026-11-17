#requires -Version 7.0
# Actual DLL + SDK pixel test in fixture-only folders. No installed config is edited.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Artifact,
    [Parameter(Mandatory)][string]$DlssRuntime,
    [string]$XessRuntime,
    [Parameter(Mandatory)][string]$OutputRoot,
    [ValidateSet('fsr22','fsr31','fsr22_12','fsr21_12','xess_12','dlss','dlss_12','auto-xess')]
    [string[]]$Backends = @('fsr22','fsr31','fsr22_12','fsr21_12','xess_12'),
    [ValidateSet('pixel','pixel-upscale','compute')][string[]]$Cases = @('pixel','pixel-upscale','compute')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$out = [IO.Directory]::CreateDirectory($OutputRoot).FullName
$exe = Join-Path $out 'TaaBackendHost.exe'
$hostDirectory = Join-Path $root 'OptiScaler/inputs/taa/tests'
$originalSource = Join-Path $hostDirectory 'TaaHookHost.cpp'
# Native FSR 3.1 requires FL11_1. The shared host's default device request only
# asks for FL11_0, so make the real feature-level request explicit in this fixture.
$hostText = [IO.File]::ReadAllText($originalSource)
$hostText = $hostText.Replace('int wmain(', 'constexpr D3D_FEATURE_LEVEL fixtureLevels[]{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};' + [Environment]::NewLine + 'int wmain(')
$hostText = $hostText.Replace('nullptr,0,D3D11_SDK_VERSION', 'fixtureLevels,2,D3D11_SDK_VERSION')
$source = Join-Path $out 'TaaBackendHost.cpp'
[IO.File]::WriteAllText($source, $hostText, [Text.UTF8Encoding]::new($false))
@{OriginalSource=$originalSource; OriginalSHA256=(Get-FileHash -LiteralPath $originalSource).Hash; GeneratedSHA256=(Get-FileHash -LiteralPath $source).Hash; DeviceFeatureLevels='11_1,11_0'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'host-source.json') -Encoding utf8
$devCmd = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat'
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od /I"{1}/OptiScaler" /I"{1}/OptiScaler/inputs/taa/tests" /Fe:"{2}" /Fo:"{3}/" "{4}" /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib' -f $devCmd,$root,$exe,$out,$source
& cmd.exe /d /s /c $command *> (Join-Path $out 'compile.log')
if ($LASTEXITCODE) { Get-Content (Join-Path $out 'compile.log'); throw 'TAA backend host compilation failed' }
$rows = @()
foreach ($backend in $Backends) {
    foreach ($case in $Cases) {
        $path = [IO.Directory]::CreateDirectory((Join-Path $out "$backend/$case")).FullName
        Copy-Item -LiteralPath $Artifact -Destination (Join-Path $path 'OptiScaler.dll')
        Copy-Item -LiteralPath $DlssRuntime -Destination (Join-Path $path 'nvngx_dlss.dll')
        if ($backend -in @('xess_12','auto-xess')) {
            if (!$XessRuntime) { throw 'XeSS fixture requires an explicit runtime file' }
            Copy-Item -LiteralPath $XessRuntime -Destination (Join-Path $path 'libxess.dll')
        }
        $requested = if ($backend -eq 'auto-xess') { 'auto' } else { $backend }
        $dx12 = if ($backend -eq 'auto-xess') { 'xess' } else { 'dlss' }
        $ini = @"
[Upscalers]
Dx11Upscaler=$requested
Dx12Upscaler=$dx12
[TAA]
Mode=Replace
Profile=taa-profile.ini
Candidate=auto
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
XeSSLibrary=$path/libxess.dll
[Log]
LogToFile=true
LogToConsole=false
LogLevel=1
LogAsync=false
LogFileName=./OptiScaler.log
"@
        [IO.File]::WriteAllText((Join-Path $path 'OptiScaler.ini'), $ini, [Text.UTF8Encoding]::new($false))
        $process = Start-Process -FilePath $exe -ArgumentList $case -WorkingDirectory $path -WindowStyle Hidden -PassThru `
            -RedirectStandardOutput (Join-Path $path 'stdout.log') -RedirectStandardError (Join-Path $path 'stderr.log')
        $finished = $process.WaitForExit(45000)
        if (!$finished) { $process.Kill(); $process.WaitForExit() }
        $stdout = [IO.File]::ReadAllText((Join-Path $path 'stdout.log'))
        $log = if (Test-Path (Join-Path $path 'OptiScaler.log')) { [IO.File]::ReadAllText((Join-Path $path 'OptiScaler.log')) } else { '' }
        $expected = switch ($backend) {
            'fsr22' { 'FSR 2.2.1' } 'fsr31' { 'FSR 3.1' }
            'fsr22_12' { 'FSR 2.2.1 w/Dx12' } 'fsr21_12' { 'FSR 2.1.2 w/Dx12' }
            'xess_12' { 'XeSS w/Dx12' } 'auto-xess' { 'XeSS w/Dx12' }
            'dlss' { 'DLSS' } 'dlss_12' { 'DLSS w/Dx12' }
        }
        $bindings = [regex]::Matches($log,'DX11 provider binding: handle=\d+ backend=([^\r\n]+?) source_device=')
        $actual = if ($bindings.Count) { $bindings[-1].Groups[1].Value } else { '' }
        $passed = $finished -and $process.ExitCode -eq 0 -and `
            $stdout.Contains('PASS actual F6 TAA Hook and provider pixels') -and `
            $log.Contains('copied_back=true') -and $actual -eq $expected
        $row = [pscustomobject]@{ Backend=$backend; Case=$case; Pass=$passed; ActualBackend=$actual;
            ExpectedBackend=$expected; Exit=$process.ExitCode; Timeout=(!$finished); PID=$process.Id;
            ArtifactSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'OptiScaler.dll')).Hash;
            RuntimeSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'nvngx_dlss.dll')).Hash;
            XessSHA256=$(if (Test-Path (Join-Path $path 'libxess.dll')) { (Get-FileHash -LiteralPath (Join-Path $path 'libxess.dll')).Hash } else { '' });
            Pixels=$stdout.Trim(); Path=$path }
        $row | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $path 'receipt.json') -Encoding utf8
        $rows += $row
        Write-Host "$backend/$case pass=$passed actual=$actual exit=$($process.ExitCode)"
    }
}
$rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $out 'receipt.json') -Encoding utf8
if (@($rows | Where-Object { !$_.Pass }).Count) { throw "Backend pixels failed; see $out" }

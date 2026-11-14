#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$CorePath = 'D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/HoYoShade/ReShade64.dll',
    [string]$OutputDirectory = 'build/genshin-reshade-20261114/real-core-test',
    [ValidateSet('color','depth','launchpad','nr-color','nr-depth','nr-launchpad','nr-uncertain')][string]$Mode = 'color'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$output = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$null = [IO.Directory]::CreateDirectory((Join-Path $output 'HoYoShade'))
$expected = '0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7'
if ((Get-FileHash -LiteralPath $CorePath -Algorithm SHA256).Hash -ne $expected) { throw 'Pinned core identity differs' }
Copy-Item -LiteralPath $CorePath -Destination (Join-Path $output 'HoYoShade/ReShade64.dll') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Probe.fx') -Destination (Join-Path $output 'Probe.fx') -Force
$preset = "Techniques=Probe@Probe.fx`nTechniqueSorting=Probe@Probe.fx`n"
if ($Mode -in @('depth','nr-depth')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'DepthProbe.fx') -Destination (Join-Path $output 'DepthProbe.fx') -Force
    $preset = "Techniques=DepthProbe@DepthProbe.fx`nTechniqueSorting=DepthProbe@DepthProbe.fx`n"
}
$effects = $output
$textures = $output
if ($Mode -in @('launchpad','nr-launchpad')) {
    $shaderRoot = Join-Path (Split-Path $CorePath -Parent) 'reshade-shaders/Shaders/iMMERSE'
    $effects = Join-Path $output 'shaders'
    $null = [IO.Directory]::CreateDirectory($effects)
    foreach ($file in @('MartysMods_LAUNCHPAD.fx','MartysMods_MXAO.fx')) {
        Copy-Item -LiteralPath (Join-Path $shaderRoot $file) -Destination (Join-Path $effects $file) -Force
    }
    Copy-Item -LiteralPath (Join-Path $shaderRoot 'MartysMods') -Destination $effects -Recurse -Force
    $textures = Join-Path (Split-Path $CorePath -Parent) 'reshade-shaders/Textures/**'
    $preset = "Techniques=MartysMods_Launchpad@MartysMods_LAUNCHPAD.fx,MartysMods_MXAO@MartysMods_MXAO.fx`nTechniqueSorting=MartysMods_Launchpad@MartysMods_LAUNCHPAD.fx,MartysMods_MXAO@MartysMods_MXAO.fx`nPreprocessorDefinitions=MXAO_USE_LAUNCHPAD_NORMALS=1`n[MartysMods_LAUNCHPAD.fx]`nNORMALS_MODE=1`nHOYO_NORMAL_RADIUS=1.19`n[MartysMods_MXAO.fx]`nHOYO_NORMAL_BLEND=1`n"
}
[IO.File]::WriteAllText((Join-Path $output 'preset.ini'), $preset)
$config = @"
[GENERAL]
EffectSearchPaths=$effects
TextureSearchPaths=$textures
PresetPath=$output/preset.ini
PerformanceMode=0
SkipLoadingDisabledEffects=0
[OVERLAY]
TutorialProgress=4
"@
[IO.File]::WriteAllText((Join-Path $output 'natural.ini'), $config)
$hostSource = if ($Mode.StartsWith('nr-')) { 'host-before-nr.cpp' } else { 'host.cpp' }
$sources = @($hostSource, '../../HoYoShade.cpp', '../../HoYoShadeOwnedRuntime.cpp', '../../HoYoShadeDepth.cpp', '../../HoYoShadeDepthSemantics.cpp') | ForEach-Object { '"' + [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $_)) + '"' }
$include = @($PSScriptRoot, "$root/OptiScaler", "$root/external/nvngx_dlss_sdk", "$root/OptiScaler/include", "$root/external/streamline/include") | ForEach-Object { '/I"' + $_ + '"' }
$batch = @"
@echo off
call "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /MD /Od /MP4 /DNOMINMAX /DHOYOSHADE_STAGE_TEST /DHOYOSHADE_TEST_OWNED_STAGE /DHOYOSHADE_OWNED_RUNTIME_STANDALONE /DHOYOSHADE_OWNED_RUNTIME_TEST $($include -join ' ') $($sources -join ' ') /Fe:"$output/host.exe" /Fo:"$output/" /link d3d11.lib d3d12.lib dxgi.lib bcrypt.lib user32.lib advapi32.lib
exit /b %errorlevel%
"@
[IO.File]::WriteAllText((Join-Path $output 'compile.cmd'), $batch.Replace("`r`n", "`n").Replace("`n", "`r`n"))
Push-Location $output
try {
    & cmd.exe /d /c (Join-Path $output 'compile.cmd') *> (Join-Path $output 'compile.log')
    if ($LASTEXITCODE -ne 0) { Get-Content (Join-Path $output 'compile.log') -Tail 24; throw 'Host compilation failed' }
    & (Join-Path $output 'host.exe') $output $Mode *> (Join-Path $output 'result.log')
    $hostResult = $LASTEXITCODE
    Get-Content (Join-Path $output 'result.log') -Tail 15
    if ($hostResult -ne 0) { throw "Real-core test failed: $hostResult" }
} finally { Pop-Location }

#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$CorePath = 'D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/HoYoShade/ReShade64.dll',
    [string]$OutputDirectory = 'build/genshin-reshade-20261114/real-core-test',
    [ValidateSet('color','depth','launchpad','single-color','single-depth','single-mxao','single-selection','selection-cache','nr-color','nr-depth','nr-launchpad','nr-uncertain')][string]$Mode = 'color',
    [switch]$TypelessInput,
    [switch]$ReversedDepth
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
if ($TypelessInput -and $Mode.StartsWith('nr-')) { throw 'Typeless input contract is BeforeSR only' }
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$output = [IO.Path]::GetFullPath($OutputDirectory, $root)
$null = [IO.Directory]::CreateDirectory((Join-Path $output 'HoYoShade'))
$expected = '0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7'
if ((Get-FileHash -LiteralPath $CorePath -Algorithm SHA256).Hash -ne $expected) { throw 'Pinned core identity differs' }
Copy-Item -LiteralPath $CorePath -Destination (Join-Path $output 'HoYoShade/ReShade64.dll') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Probe.fx') -Destination (Join-Path $output 'Probe.fx') -Force
$preset = "Techniques=Probe@Probe.fx`nTechniqueSorting=Probe@Probe.fx`n"
if ($Mode -in @('selection-cache','single-selection')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'CacheProbe.fx') -Destination (Join-Path $output 'CacheProbe.fx') -Force
    $preset = "Techniques=Add@CacheProbe.fx,Scale@CacheProbe.fx`nTechniqueSorting=Add@CacheProbe.fx,Scale@CacheProbe.fx`n"
}
if ($Mode -in @('depth','nr-depth','single-depth')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'DepthProbe.fx') -Destination (Join-Path $output 'DepthProbe.fx') -Force
    $preset = "Techniques=DepthProbe@DepthProbe.fx`nTechniqueSorting=DepthProbe@DepthProbe.fx`n"
}
$effects = $output
$textures = $output
if ($Mode -in @('launchpad','nr-launchpad','single-mxao')) {
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
if ($Mode.StartsWith('single-')) {
    $reverse = if ($ReversedDepth) { 1 } else { 0 }
    $config = $config.Replace('[GENERAL]', "[GENERAL]`nPreprocessorDefinitions=RESHADE_DEPTH_INPUT_IS_REVERSED=$reverse,RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN=0")
    [IO.File]::WriteAllText((Join-Path $output 'ReShade.ini'), $config)
} else {
[IO.File]::WriteAllText((Join-Path $output 'natural.ini'), $config)
# The core gives process-global tutorial state precedence over runtime config.
# Keep this isolated fixture's splash UI out of the exact pixel oracle, even
# when a previous fixture run saved TutorialProgress=0 beside its executable.
[IO.File]::WriteAllText((Join-Path $output 'ReShade.ini'), "[OVERLAY]`nTutorialProgress=4`n")
}
$hostSource = if ($Mode.StartsWith('nr-')) { 'host-before-nr.cpp' } else { 'host.cpp' }
if ($Mode.StartsWith('nr-')) {
    # Compile the actual forwarding method, not a fixture copy of its policy.
    $wrapper = [IO.File]::ReadAllText((Join-Path $root 'OptiScaler/with_dx12/dx11_with_dx12_sc.cpp'))
    $start = $wrapper.IndexOf('HRESULT STDMETHODCALLTYPE Dx11wDx12SC::HookSetColorSpace1(')
    $end = $wrapper.IndexOf('HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(', $start)
    if ($start -lt 0 -or $end -le $start) { throw 'Cannot extract actual color-space forwarding method' }
    [IO.File]::WriteAllText((Join-Path $output 'Dx11wDx12ColorSpace.inl'), $wrapper.Substring($start, $end - $start))
}
$sources = @($hostSource, '../../HoYoShade.cpp', '../../HoYoShadeOwnedRuntime.cpp', '../../HoYoShadeDepth.cpp', '../../HoYoShadeDepthSemantics.cpp') | ForEach-Object { '"' + [IO.Path]::GetFullPath((Join-Path $PSScriptRoot $_)) + '"' }
$include = @($PSScriptRoot, $output, "$root/OptiScaler", "$root/external/nvngx_dlss_sdk", "$root/OptiScaler/include", "$root/external/streamline/include") | ForEach-Object { '/I"' + $_ + '"' }
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
    $hostArguments = @($output, $Mode)
    if ($ReversedDepth) { $hostArguments += 'typeless-reversed' }
    elseif ($TypelessInput) { $hostArguments += 'typeless' }
    & (Join-Path $output 'host.exe') @hostArguments *> (Join-Path $output 'result.log')
    $hostResult = $LASTEXITCODE
    Get-Content (Join-Path $output 'result.log') -Tail 15
    if ($hostResult -ne 0) { throw "Real-core test failed: $hostResult" }
    if ($Mode.StartsWith('single-')) {
        $iniFiles = @(Get-ChildItem -LiteralPath $output -Filter 'ReShade*.ini' -File -Recurse)
        if ($iniFiles.Count -ne 1 -or $iniFiles[0].FullName -ne (Join-Path $output 'ReShade.ini')) { throw 'Unexpected second ReShade config' }
    }
    if ($TypelessInput -and $Mode -in @('color','single-color')) {
        & (Join-Path $output 'host.exe') $output $Mode 'typeless-partial' *> (Join-Path $output 'result-partial.log')
        $partialResult = $LASTEXITCODE
        Get-Content (Join-Path $output 'result-partial.log') -Tail 8
        if ($partialResult -ne 0) { throw "Real-core partial-input test failed: $partialResult" }
    }
} finally { Pop-Location }

#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$DevCmd = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat',
    [string]$Python = 'D:/APPS/conda/python.exe'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$output = Join-Path $root 'build/menu-preview-tests'
$null = [IO.Directory]::CreateDirectory($output)
& $Python -B (Join-Path $PSScriptRoot 'test_preview_output_wiring.py')
if ($LASTEXITCODE -ne 0) { throw 'Menu preview output wiring failed' }
& $Python -B (Join-Path $root 'OptiScaler/runtime/tests/prepare_guide_capture_hooks.py') (Join-Path $output 'GuideCaptureHooksProduction.inl')
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract production guide capture hooks' }
$source = Join-Path $PSScriptRoot 'PreviewOutputCacheWarpTests.cpp'
$exe = Join-Path $output 'PreviewOutputCacheWarpTests.exe'
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od /I"{1}" /I"{2}" /I"{3}" /Fe:"{4}" /Fo:"{5}/" "{6}" /link d3d12.lib dxgi.lib d3dcompiler.lib' -f $DevCmd,$output,(Join-Path $root 'OptiScaler'),(Join-Path $root 'OptiScaler/include'),$exe,$output,$source
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Menu preview WARP compilation failed: $LASTEXITCODE" }
& $exe
if ($LASTEXITCODE -ne 0) { throw "Menu preview WARP failed: $LASTEXITCODE" }

#requires -Version 7.0
[CmdletBinding()]
param([string]$OutputRoot)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$OutputRoot) { $OutputRoot = Join-Path $root 'build/upscaler-delivery-tests' }
$output = [IO.Directory]::CreateDirectory($OutputRoot).FullName
& python -B (Join-Path $root 'OptiScaler/upscalers/tests/backend-delivery/prepare.py') $output
if ($LASTEXITCODE) { throw 'Provider production extraction failed' }
$devCmd = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat'
$sdk = Join-Path $root 'external/NVIDIA_NGX/include'
if (!(Test-Path -LiteralPath (Join-Path $sdk 'nvsdk_ngx_params.h'))) {
    $sdk = Split-Path -Parent (Get-ChildItem -LiteralPath (Join-Path $root 'external') -Filter 'nvsdk_ngx_params.h' -Recurse -File | Select-Object -First 1).FullName
}
$source = Join-Path $root 'OptiScaler/upscalers/tests/backend-delivery/fixture.cpp'
$exe = Join-Path $output 'UpscalerDeliveryTests.exe'
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od /I"{1}/OptiScaler" /I"{2}" /I"{3}" /Fe:"{4}" /Fo:"{3}/" "{5}" /link d3d11.lib user32.lib' -f $devCmd,$root,$sdk,$output,$exe,$source
& cmd.exe /d /s /c $command *> (Join-Path $output 'compile.log')
if ($LASTEXITCODE) { Get-Content (Join-Path $output 'compile.log'); throw 'Provider delivery fixture compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Provider delivery fixture failed' }

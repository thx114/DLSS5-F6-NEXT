#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$DevCmd = $env:VSDEVCMD,
    [string]$Python = 'python',
    [string]$BaselineShader = ''
)

# Standalone test executable only. Compile production HLSL in WARP memory;
# never regenerate DXIL/SPIR-V/embedded headers, build a DLL, or deploy.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$out = Join-Path $root 'build/look-warp'
$null = [IO.Directory]::CreateDirectory($out)
$exe = Join-Path $out 'LookWarpTests.exe'
$source = Join-Path $PSScriptRoot 'LookWarpTests.cpp'
$object = Join-Path $out 'LookWarpTests.obj'
$include = Join-Path $root 'OptiScaler'
$sdk = Join-Path $root 'external/nvngx_dlss_sdk'
& $Python -B (Join-Path $PSScriptRoot 'colour-transaction/prepare.py')
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract production colour destination/bindings' }
foreach ($path in @($DevCmd, $exe, $source, $object, $include, $sdk)) {
    if ($path -match '["%\r\n]') { throw "Unsupported compiler path: $path" }
}
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /I"{1}" /Fe:"{2}" /Fo:"{3}" "{4}" /I"{5}" /I"{6}" /link d3d11.lib d3dcompiler.lib' -f $DevCmd,$include,$exe,$object,$source,$sdk,(Join-Path $root 'build/common-core-tests')
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Look WARP fixture compile failed: $LASTEXITCODE" }
$arguments = @()
if ($BaselineShader) { $arguments += [IO.Path]::GetFullPath($BaselineShader) }
& $exe @arguments | Tee-Object -FilePath (Join-Path $out 'warp.log')
if ($LASTEXITCODE -ne 0) { throw "Look WARP pixel tests failed: $LASTEXITCODE" }
& $exe --negative-control | Tee-Object -FilePath (Join-Path $out 'negative-control.log')
if ($LASTEXITCODE -ne 1) { throw "Encoded-domain negative control must fail pixel assertions with exit 1; got $LASTEXITCODE" }
Write-Output 'Look WARP positive passed; legacy encoded-domain negative control rejected.'
exit 0

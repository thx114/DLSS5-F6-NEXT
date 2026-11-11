#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Dxc = (Join-Path $PSScriptRoot '../../shader_tools/dxc.exe'),
    [string]$DevCmd = $env:VSDEVCMD,
    [switch]$NoGenerate
)

# Explicitly invoked by Lead only: DXC -> consistency -> cl -> WARP, sequentially.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$rfRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (!$NoGenerate) { & (Join-Path $rfRoot 'Generate-RFShader.ps1') -Dxc $Dxc }
& (Join-Path $rfRoot 'Generate-RFShader.ps1') -CheckOnly
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$out = Join-Path $root 'build/resource-flip-warp'
$null = [IO.Directory]::CreateDirectory($out)
$exe = Join-Path $out 'RFWarp.exe'
$source = Join-Path $PSScriptRoot 'RFWarp.cpp'
$object = Join-Path $out 'RFWarp.obj'
# Local compiler paths only; cmd is required to retain VsDevCmd's environment.
foreach ($path in @($DevCmd, $source, $exe, $object)) {
    if ($path -match '["%\r\n]') { throw "Unsupported compiler path: $path" }
}
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /Fe:"{1}" /Fo:"{2}" "{3}" /link d3d12.lib dxgi.lib d3dcompiler.lib' -f $DevCmd,$exe,$object,$source
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "RF WARP fixture compile failed: $LASTEXITCODE" }
& $exe
if ($LASTEXITCODE -ne 0) { throw "RF WARP pixel regression failed: $LASTEXITCODE" }

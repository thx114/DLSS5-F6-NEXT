#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ShaderEvidence,
    [string]$ProceduralEvidence,
    [string]$OutputRoot,
    [string]$DevCmd = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$evidence = (Get-Item -LiteralPath $ShaderEvidence -ErrorAction Stop).FullName
$out = if ($OutputRoot) { [IO.Path]::GetFullPath($OutputRoot) } else { Join-Path $root 'build/taa-consumer-programs' }
$null = [IO.Directory]::CreateDirectory($out)
$exe = Join-Path $out 'TaaConsumerProgramsWarpTests.exe'
$source = Join-Path $root 'OptiScaler/inputs/taa/tests/TaaConsumerProgramsWarpTests.cpp'
foreach ($value in @($DevCmd, $root, $out, $source)) {
    if ($value -match '["%\r\n]') { throw "Unsupported compiler path: $value" }
}
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /O2 /I"{1}/OptiScaler" /I"{1}/OptiScaler/include" /I"{1}/external/simpleini" /I"{1}/external/nvngx_dlss_sdk" /Fe:"{2}" /Fo:"{3}/" "{4}" /link d3d11.lib d3dcompiler.lib' -f $DevCmd,$root,$exe,$out,$source
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Captured program fixture compile failed: $LASTEXITCODE" }
$fixtureArguments=@($evidence)
if($ProceduralEvidence) {$fixtureArguments+=(Get-Item -LiteralPath $ProceduralEvidence -ErrorAction Stop).FullName}
& $exe @fixtureArguments
if ($LASTEXITCODE -ne 0) { throw "Captured program GPU replay failed: $LASTEXITCODE" }
exit 0

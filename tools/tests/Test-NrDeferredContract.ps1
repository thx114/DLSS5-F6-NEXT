#requires -Version 7.0
[CmdletBinding()]
param([string]$Source,[string]$OutputRoot,[string]$Python='D:/APPS/conda/python.exe',
      [string]$DevCmd='D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat')
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$out=if($OutputRoot){[IO.Path]::GetFullPath($OutputRoot)}else{Join-Path $root 'build/nr-deferred-contract'}
$null=[IO.Directory]::CreateDirectory($out)
if(!$Source){$Source=Join-Path $root 'OptiScaler/shaders/dlssnr/DlssNr_DeferredSr.inl'}
& $Python -B (Join-Path $root 'OptiScaler/shaders/dlssnr/tests/deferred-contract/prepare.py') $Source $out
if($LASTEXITCODE -ne 0){throw 'Deferred seam extraction failed'}
$sourceFile=Join-Path $root 'OptiScaler/shaders/dlssnr/tests/deferred-contract/fixture.cpp'
$exe=Join-Path $out 'NrDeferredContract.exe'
$command='call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /O2 /I"{1}/OptiScaler" /I"{2}" /Fe:"{3}" /Fo:"{2}/NrDeferredContract.obj" "{4}"' -f $DevCmd,$root,$out,$exe,$sourceFile
& cmd.exe /d /s /c $command
if($LASTEXITCODE -ne 0){throw 'Deferred seam compilation failed'}
& $exe
if($LASTEXITCODE -ne 0){throw 'Deferred seam execution failed'}
exit 0

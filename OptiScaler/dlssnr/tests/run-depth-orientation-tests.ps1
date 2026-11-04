param([string]$DevCmd = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$output = Join-Path $root 'build/nr-depth-orientation-tests'
$null = New-Item -ItemType Directory -Force $output
$source = Join-Path $PSScriptRoot 'nr_depth_orientation.cpp'
$exe = Join-Path $output 'nr_depth_orientation.exe'
$obj = Join-Path $output 'nr_depth_orientation.obj'
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /std:c++20 /EHsc /W4 /WX /Od /Fe:"{1}" /Fo:"{2}" "{3}"' -f $DevCmd,$exe,$obj,$source
& cmd.exe /d /s /c $command
$compileExit = $LASTEXITCODE
Write-Host "CPU compile exit=$compileExit"
if ($compileExit -ne 0) { exit $compileExit }
& $exe
$testExit = $LASTEXITCODE
Write-Host "CPU test exit=$testExit"
exit $testExit

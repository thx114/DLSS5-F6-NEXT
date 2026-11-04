param([string]$DevCmd = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../../../')).Path
$output = Join-Path $root 'build/ngx-route-tests'
New-Item -ItemType Directory -Force $output | Out-Null
$source = Join-Path $PSScriptRoot 'RouterTests.cpp'
$exe = Join-Path $output 'RouterTests.exe'
$obj = Join-Path $output 'RouterTests.obj'
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /std:c++20 /EHsc /W4 /WX /Od /Zi /Fe:"{1}" /Fo:"{2}" /Fd:"{3}" "{4}" && "{1}"' -f $DevCmd,$exe,$obj,(Join-Path $output 'RouterTests.pdb'),$source
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "Standalone NGX routing tests failed: $LASTEXITCODE" }

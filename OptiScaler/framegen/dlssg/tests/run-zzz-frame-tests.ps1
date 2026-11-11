param([string]$DevCmd = $env:VSDEVCMD)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../../../')).Path
$output = Join-Path $root 'build/zzz-frame-tests'
New-Item -ItemType Directory -Force $output | Out-Null
$source = Join-Path $PSScriptRoot 'ZzzFrameScopeTests.cpp'
$exe = Join-Path $output 'ZzzFrameScopeTests.exe'
$obj = Join-Path $output 'ZzzFrameScopeTests.obj'
$sdk = Join-Path $root 'external/streamline'
$command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{4}" /Fe:"{1}" /Fo:"{2}" "{3}" && "{1}"' -f $DevCmd,$exe,$obj,$source,$sdk
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) { throw "ZZZ standalone frame-scope tests failed: $LASTEXITCODE" }
& python (Join-Path $PSScriptRoot 'check-zzz-frame-wiring.py')
if ($LASTEXITCODE -ne 0) { throw "ZZZ production wiring checks failed: $LASTEXITCODE" }

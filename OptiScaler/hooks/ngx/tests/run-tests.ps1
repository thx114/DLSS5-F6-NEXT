param([string]$DevCmd = $env:VSDEVCMD)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../../../')).Path
$output = Join-Path $root 'build/ngx-route-tests'
New-Item -ItemType Directory -Force $output | Out-Null
foreach ($suite in @('RouterTests', 'ReleaseContractTests')) {
    $source = Join-Path $PSScriptRoot "$suite.cpp"
    $exe = Join-Path $output "$suite.exe"
    $obj = Join-Path $output "$suite.obj"
    $sdk = Join-Path $root 'external/nvngx_dlss_sdk'
    $dense = Join-Path $root 'external/unordered_dense/include'
    $command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /std:c++20 /EHsc /W4 /WX /Od /Zi /I"{5}" /I"{6}" /Fe:"{1}" /Fo:"{2}" /Fd:"{3}" "{4}" && "{1}"' -f $DevCmd,$exe,$obj,(Join-Path $output "$suite.pdb"),$source,$sdk,$dense
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "Standalone NGX $suite failed: $LASTEXITCODE" }
}

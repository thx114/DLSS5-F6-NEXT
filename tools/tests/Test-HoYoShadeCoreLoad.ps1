#requires -Version 7.0
[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path $env:TEMP ('f6-core-load-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))))
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$output = [IO.Path]::GetFullPath($OutputDirectory, $root)
$null = [IO.Directory]::CreateDirectory($output)
# A disposable DLL tests Windows load identity without loading ReShade hooks.
[IO.File]::WriteAllText((Join-Path $output 'empty.cpp'), '__declspec(dllexport) void F6Fixture() {}')
$batch = @"
@echo off
call "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cl /nologo /LD /MD /W4 /WX /Fo:"$output/empty.obj" /Fe:"$output/ReShade64.dll" "$output/empty.cpp" /link /IMPLIB:"$output/ReShade64.lib"
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /MD /W4 /WX /Fo:"$output/host.obj" /Fe:"$output/host.exe" "$root/OptiScaler/integration/tests/HoYoShadeCoreLoadTests.cpp"
exit /b %errorlevel%
"@
[IO.File]::WriteAllText((Join-Path $output 'compile.cmd'), $batch)
Push-Location $output
try {
    & cmd.exe /d /c (Join-Path $output 'compile.cmd') *> (Join-Path $output 'compile.log')
    if ($LASTEXITCODE) { Get-Content (Join-Path $output 'compile.log'); throw 'Core-load fixture compilation failed' }
    & (Join-Path $output 'host.exe') (Join-Path $output 'ReShade64.dll') *> (Join-Path $output 'result.log')
    if ($LASTEXITCODE) { Get-Content (Join-Path $output 'result.log'); throw 'Core-load ownership test failed' }
    Get-Content (Join-Path $output 'result.log')
    Write-Output "Receipt: $output"
} finally { Pop-Location }

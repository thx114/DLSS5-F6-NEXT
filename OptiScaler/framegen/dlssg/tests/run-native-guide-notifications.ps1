param([string]$DevCmd = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../../'))
$output = Join-Path $root 'build/native-guide-notifications'
$null = [IO.Directory]::CreateDirectory($output)
$fixture = Join-Path $PSScriptRoot 'native-guide-notifications/fixture.cpp'
$prepare = Join-Path $PSScriptRoot 'native-guide-notifications/prepare.py'
$production = Join-Path $output 'NativeGuideNotificationsProduction.inl'
foreach ($negative in @($false, $true)) {
    $arguments = @($prepare, $production)
    if ($negative) { $arguments += '--negative-control' }
    & python @arguments
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract native producer notifications' }
    $name = if ($negative) { 'negative' } else { 'production' }
    $exe = Join-Path $output "$name.exe"
    $obj = Join-Path $output "$name.obj"
    $command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{1}" /I"{2}" /Fe:"{3}" /Fo:"{4}" "{5}"' -f $DevCmd,(Join-Path $root 'OptiScaler'),$output,$exe,$obj,$fixture
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "Native guide $name compilation failed: $LASTEXITCODE" }
    & $exe
    $expected = if ($negative) { 42 } else { 0 }
    if ($LASTEXITCODE -ne $expected) { throw "Native guide $name returned $LASTEXITCODE, expected $expected" }
}
# Leave the generated include identifying the actual production implementation.
& python $prepare $production
if ($LASTEXITCODE -ne 0) { throw 'Cannot restore production test include' }
Write-Host 'PASS production notifications and detected missing-filter negative control'

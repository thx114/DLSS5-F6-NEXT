param([string]$DevCmd = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat')
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../'))
$output = Join-Path $root 'build/module-lifetime-tests'
$null = [IO.Directory]::CreateDirectory($output)
$source = Join-Path $PSScriptRoot 'module-lifetime'
foreach ($name in @('pinned','control','host')) {
    $inputFile = if ($name -eq 'host') { Join-Path $source 'host.cpp' } else { Join-Path $source 'library.cpp' }
    $extra = if ($name -eq 'host') { '' } elseif ($name -eq 'control') { '/LD /DUNPINNED_CONTROL' } else { '/LD' }
    $extension = if ($name -eq 'host') { 'exe' } else { 'dll' }
    $target = Join-Path $output "$name.$extension"
    $obj = Join-Path $output "$name.obj"
    $command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{1}" {2} /Fe:"{3}" /Fo:"{4}" "{5}" /link /IMPLIB:"{6}"' -f $DevCmd,(Join-Path $root 'OptiScaler'),$extra,$target,$obj,$inputFile,(Join-Path $output "$name.lib")
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "Module lifetime $name compile failed: $LASTEXITCODE" }
}
& (Join-Path $output 'host.exe') (Join-Path $output 'pinned.dll') (Join-Path $output 'control.dll')
if ($LASTEXITCODE -ne 0) { throw "Module lifetime fixture failed: $LASTEXITCODE" }

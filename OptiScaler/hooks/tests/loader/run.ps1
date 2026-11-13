#requires -Version 7.0
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$output=Join-Path $root 'build/genshin-reshade-20261114/loader-tests'
$null=[IO.Directory]::CreateDirectory((Join-Path $output 'a'))
$null=[IO.Directory]::CreateDirectory((Join-Path $output 'b'))
$commands=@('@echo off','call "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64','if errorlevel 1 exit /b %errorlevel%')
foreach($variant in @(@('a',11,100),@('b',22,200))) {
    $directory=Join-Path $output $variant[0]
    $commands+='cl /nologo /EHsc /MD /W4 /WX /LD /DIDENTITY={0} "{1}/dependency.cpp" /Fo:"{2}/dependency.obj" /link /OUT:"{2}/loaderdep.dll" /IMPLIB:"{2}/loaderdep.lib"' -f $variant[1],$PSScriptRoot,$directory
    $commands+='if errorlevel 1 exit /b %errorlevel%'
    $commands+='cl /nologo /EHsc /MD /W4 /WX /LD /DIDENTITY={0} "{1}/probe.cpp" /Fo:"{2}/probe.obj" /link "{2}/loaderdep.lib" /OUT:"{2}/d3d11.dll" /IMPLIB:"{2}/d3d11.lib"' -f $variant[2],$PSScriptRoot,$directory
    $commands+='if errorlevel 1 exit /b %errorlevel%'
}
$commands+='cl /nologo /std:c++20 /EHsc /MD /W4 /WX /I"{0}" /I"{1}/OptiScaler" "{0}/host.cpp" /Fo:"{2}/host.obj" /Fe:"{2}/host.exe"' -f $PSScriptRoot,$root,$output
$commands+='if errorlevel 1 exit /b %errorlevel%'
$commands+='cl /nologo /std:c++20 /EHsc /MD /W4 /WX /I"{0}/OptiScaler" "{1}/observation.cpp" /Fo:"{2}/observation.obj" /Fe:"{2}/observation.exe"' -f $root,$PSScriptRoot,$output
$commands+='if errorlevel 1 exit /b %errorlevel%'
$commands+='cl /nologo /std:c++latest /EHsc /MD /W4 /WX /I"{0}/OptiScaler" /I"{2}" "{1}/scheduler.cpp" /Fo:"{2}/scheduler.obj" /Fe:"{2}/scheduler.exe"' -f $root,$PSScriptRoot,$output
$commands+='exit /b %errorlevel%'  
& D:/APPS/conda/python.exe -B (Join-Path $PSScriptRoot 'prepare.py') $output
if($LASTEXITCODE){throw 'Loader production extraction failed'}
$compile=Join-Path $output 'compile.cmd'
[IO.File]::WriteAllLines($compile,$commands)
& cmd.exe /d /c $compile *> (Join-Path $output 'compile.log')
if($LASTEXITCODE){Get-Content (Join-Path $output 'compile.log') -Tail 25;throw 'Loader fixture compile failed'}
foreach($scenario in @('normal','altered','system','user','redirect','fallback','reject','alias','data','cached-data','image','unresolved','missing','invalid-file','invalid-combination','relative-load-dir','signature','native','nested','callout-native')) {
    & (Join-Path $output 'host.exe') $scenario $output
    if($LASTEXITCODE){throw "Loader fixture $scenario failed: $LASTEXITCODE"}
}

foreach($scenario in @('normal','unresolved')) {
    & (Join-Path $output 'observation.exe') $scenario $output
    if($LASTEXITCODE){throw "Production observer fixture $scenario failed: $LASTEXITCODE"}
}

& (Join-Path $output 'scheduler.exe')
if($LASTEXITCODE){throw 'Production loader scheduler failed'}

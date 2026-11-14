#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$CorePath = 'D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/HoYoShade/ReShade64.dll',
    [string]$OutputDirectory = 'build/genshin-reshade-20261114/on12-factory'
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
$output=[IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$null=[IO.Directory]::CreateDirectory((Join-Path $output 'HoYoShade'))
$expected='0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7'
if((Get-FileHash -LiteralPath $CorePath -Algorithm SHA256).Hash -ne $expected){throw 'Pinned ReShade core identity differs'}
$copiedCore=Join-Path $output 'HoYoShade/ReShade64.dll'
Copy-Item -LiteralPath $CorePath -Destination $copiedCore -Force
$sources=@((Join-Path $PSScriptRoot 'host.cpp'))+@('HoYoShade.cpp','HoYoShadeOwnedRuntime.cpp','HoYoShadeDepth.cpp','HoYoShadeDepthSemantics.cpp' | ForEach-Object {Join-Path $root "OptiScaler/integration/$_"})
$sourceArgs=$sources | ForEach-Object {'"'+$_+'"'}
$includes=@('OptiScaler/integration/tests/before-sr','OptiScaler','external/nvngx_dlss_sdk','OptiScaler/include','external/streamline/include') | ForEach-Object {'/I"'+(Join-Path $root $_)+'"'}
$batch=@"
@echo off
call "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /MD /Od /MP4 /DNOMINMAX /DHOYOSHADE_STAGE_TEST /DHOYOSHADE_TEST_OWNED_STAGE /DHOYOSHADE_OWNED_RUNTIME_STANDALONE /DHOYOSHADE_OWNED_RUNTIME_TEST $($includes -join ' ') $($sourceArgs -join ' ') /Fe:"$output/host.exe" /Fo:"$output/" /link "$root/OptiScaler/library/detours/detours.lib" d3d11.lib d3d12.lib dxgi.lib bcrypt.lib user32.lib advapi32.lib
exit /b %errorlevel%
"@
[IO.File]::WriteAllText((Join-Path $output 'compile.cmd'),$batch.Replace("`r`n","`n").Replace("`n","`r`n"))
Push-Location -LiteralPath $output
try {
    & cmd.exe /d /c (Join-Path $output 'compile.cmd') *> (Join-Path $output 'compile.log')
    if($LASTEXITCODE -ne 0){Get-Content -LiteralPath (Join-Path $output 'compile.log') -Tail 30;throw 'Factory host compilation failed'}
    $results=@()
    foreach($mode in @('negative','system-only-negative','before','after')) {
        & (Join-Path $output 'host.exe') $copiedCore $mode *> (Join-Path $output "$mode.log")
        $code=$LASTEXITCODE
        $isNegative=$mode.EndsWith('negative')
        $expectedExit=if($isNegative){-2147483645}else{0}
        Get-Content -LiteralPath (Join-Path $output "$mode.log") -Tail 4
        $results += [pscustomobject]@{Mode=$mode;ExitCode=$code;ExpectedExit=$expectedExit;Passed=$code -eq $expectedExit}
        if($code -ne $expectedExit){throw "Factory host $mode failed with exit $code"}
    }
    $results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding utf8
} finally {Pop-Location}

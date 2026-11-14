#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [string]$OutputDirectory='build/genshin-reshade-20261114/detail-feedback-20261114-140355/real-sdk-input',
    [switch]$Visible
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../../..'))
$runtime=(Get-Item -LiteralPath $RuntimeDirectory).FullName
$output=[IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$null=[IO.Directory]::CreateDirectory($output)
$dlls=@('sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.reflex.dll','sl.pcl.dll','nvngx_dlssg.dll')
$identity=@($dlls | ForEach-Object {
    $file=Get-Item -LiteralPath (Join-Path $runtime $_)
    [pscustomobject]@{Path=$file.FullName;Bytes=$file.Length;Sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash;Version=$file.VersionInfo.FileVersion}
})
$identity | ConvertTo-Json | Set-Content (Join-Path $output 'runtime-before.json') -Encoding utf8
$batch=@"
@echo off
call "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%
cl /nologo /std:c++20 /EHsc /MD /O2 /DNOMINMAX /I"$root/external/streamline" "$PSScriptRoot/host.cpp" /Fe:"$output/host.exe" /Fo:"$output/host.obj" /link d3d12.lib dxgi.lib user32.lib
exit /b %errorlevel%
"@
[IO.File]::WriteAllText((Join-Path $output 'compile.cmd'),$batch.Replace("`r`n","`n").Replace("`n","`r`n"))
Push-Location -LiteralPath $output
try {
    & cmd.exe /d /c (Join-Path $output 'compile.cmd') *> (Join-Path $output 'compile.log')
    if($LASTEXITCODE -ne 0) {Get-Content (Join-Path $output 'compile.log') -Tail 15;throw 'SDK probe compilation failed'}
    $arguments=@($runtime,$output)
    if($Visible) {$arguments+='--visible'}
    & (Join-Path $output 'host.exe') @arguments *> (Join-Path $output 'result.log')
    $hostExit=$LASTEXITCODE
    Get-Content (Join-Path $output 'result.log') -Tail 20
    $after=@($identity | ForEach-Object {[pscustomobject]@{Path=$_.Path;Sha256=(Get-FileHash -LiteralPath $_.Path -Algorithm SHA256).Hash}})
    $after | ConvertTo-Json | Set-Content (Join-Path $output 'runtime-after.json') -Encoding utf8
    $changed=@($after | Where-Object {$item=$_;($identity | Where-Object Path -eq $item.Path).Sha256 -ne $item.Sha256})
    if($changed.Count) {throw 'Installed SDK contents changed during standalone probe'}
    $text=[IO.File]::ReadAllText((Join-Path $output 'result.log'))
    $activeMatch=[regex]::Match($text,'(?m)^ACTIVE_STATE_MODULE path=(.+)\r?$')
    $active=if($activeMatch.Success) {
        $path=$activeMatch.Groups[1].Value.Trim()
        $file=Get-Item -LiteralPath $path
        [pscustomobject]@{Path=$file.FullName;Sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash;Version=$file.VersionInfo.FileVersion}
    } else {$null}
    $samples=[regex]::Matches($text,'(?m)^PRESENT_SAMPLE .+$')
    $outcome=if($hostExit -eq 0){'OWNING_INPUT_OBSERVED'}elseif($hostExit -eq 3){'CAPABILITY_UNAVAILABLE'}elseif($hostExit -eq 4){'NO_CONSUMED_INPUT_PROOF'}elseif($hostExit -eq 5){'CLEANUP_FAILED'}else{'PROBE_FAILED'}
    [pscustomobject]@{TimestampUtc=[DateTime]::UtcNow.ToString('O');Exit=$hostExit;Outcome=$outcome;PresentSamples=$samples.Count;FocusDisabled=$text.Contains('DLSS-G disabled: window not focused');Visible=$Visible.IsPresent;ActiveStateModule=$active;InstalledRuntimeUnchanged=$true;GameStarted=$false;GamePerformanceValidated=$false} |
        ConvertTo-Json -Depth 4 | Set-Content (Join-Path $output 'result.json') -Encoding utf8
    if($hostExit -notin @(0,3,4)) {throw "Standalone real SDK probe failed: $hostExit"}
    # Preserve the explicit unsupported/inconclusive result for CLI callers.
    # Recording a valid observation is different from proving a consumed input.
    exit $hostExit
} finally {Pop-Location}

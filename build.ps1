#requires -Version 7.0
<#
    DLSS5_NEXT clean build entry.

    Builds OptiScaler.dll (Release x64) from the tree in this directory.
    Layout this script expects:
        .\OptiScaler\        product source (OptiScaler.vcxproj)
        .\external\          third-party dependencies
        .\build\             obj / out / logs (generated)

    Usage:
        pwsh -NoProfile -File .\build.ps1              # incremental build
        pwsh -NoProfile -File .\build.ps1 -Clean       # wipe obj/out first
        pwsh -NoProfile -File .\build.ps1 -Jobs 8      # parallel TUs
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [int]$Jobs = 1
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root     = $PSScriptRoot
$source   = Join-Path $root 'OptiScaler'
$external = Join-Path $root 'external'
$build    = Join-Path $root 'build'
$objDir   = Join-Path $build 'obj'
$outDir   = Join-Path $build 'out'
$tempDir  = Join-Path $build 'temp'
$log      = Join-Path $build 'msbuild.log'
$msbuild  = 'D:\VS2022BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe'
$project  = Join-Path $source 'OptiScaler.vcxproj'
$artifact = Join-Path $outDir 'OptiScaler.dll'

foreach ($required in @($source, $external, $project, $msbuild)) {
    if (!(Test-Path -LiteralPath $required)) {
        throw "Missing required path: $required"
    }
}

if ($Clean) {
    Write-Host "Clean: removing $objDir, $outDir, $tempDir"
    foreach ($d in @($objDir, $outDir, $tempDir)) {
        if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force }
    }
}

$null = [IO.Directory]::CreateDirectory($objDir)
$null = [IO.Directory]::CreateDirectory($outDir)
$null = [IO.Directory]::CreateDirectory($tempDir)

$savedTemp = $env:TEMP
$savedTmp  = $env:TMP
$savedCl   = $env:_CL_
try {
    $env:TEMP = $tempDir
    $env:TMP  = $tempDir
    $env:_CL_ = "/MP$Jobs /FS"

    # SolutionDir must keep its trailing separator: the vcxproj uses
    # $(SolutionDir)external\... for includes and libraries.
    $args = @(
        $project,
        '/t:Build',
        '/p:Configuration=Release',
        '/p:Platform=x64',
        "/m:$Jobs",
        '/verbosity:minimal',
        "/p:SolutionDir=$root\",
        "/p:IntDir=$objDir\",
        "/p:OutDir=$outDir\",
        '/p:PostBuildEventUseInBuild=false'
    )

    Write-Host "Building $project"
    Write-Host "  SolutionDir = $root\"
    Write-Host "  OutDir      = $outDir\"
    $started = Get-Date
    & $msbuild @args 2>&1 | Tee-Object -FilePath $log
    $exit = $LASTEXITCODE
    $elapsed = (Get-Date) - $started
}
finally {
    $env:TEMP = $savedTemp
    $env:TMP  = $savedTmp
    $env:_CL_ = $savedCl
}

Write-Host ("MSBuild exit {0} after {1:hh\:mm\:ss}" -f $exit, $elapsed)
if ($exit -ne 0) { exit $exit }

if (!(Test-Path -LiteralPath $artifact)) {
    Write-Error "Build reported success but $artifact is missing." -ErrorAction Continue
    exit 1
}
$dll = Get-Item -LiteralPath $artifact
Write-Host ("Artifact: {0}" -f $dll.FullName)
Write-Host ("  size   : {0} bytes" -f $dll.Length)
Write-Host ("  mtime  : {0:O}Z (UTC)" -f $dll.LastWriteTimeUtc)
exit 0

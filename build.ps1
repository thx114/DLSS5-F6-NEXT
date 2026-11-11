#requires -Version 7.0
<#
Release x64 build. Outputs and the UTF-8 MSBuild log stay under build/.
  pwsh -NoProfile -File ./build.ps1
  pwsh -NoProfile -File ./build.ps1 -Clean -Jobs 8
This entry builds the DLL; deployment and release packaging are separate actions.
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [ValidateRange(1, 64)]
    [int]$Jobs = [Math]::Min(8, [Environment]::ProcessorCount),
    [string]$MSBuildPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$root = $PSScriptRoot
$build = Join-Path $root 'build'
$objDir = Join-Path $build 'obj'
$outDir = Join-Path $build 'out'
$tempDir = Join-Path $build 'temp'
$log = Join-Path $build 'msbuild.log'
. (Join-Path $root 'tools/build/Toolchain.ps1')
$msbuild = Find-F6VisualStudioTool -ExplicitPath $MSBuildPath -Kind MSBuild
$project = Join-Path $root 'OptiScaler/OptiScaler.vcxproj'
$artifact = Join-Path $outDir 'OptiScaler.dll'

foreach ($required in @($project, (Join-Path $root 'external'), $msbuild)) {
    if (!(Test-Path -LiteralPath $required)) { throw "Missing required path: $required" }
}

if ($Clean) {
    # These are fixed, absolute children of this script's build directory.
    foreach ($directory in @($objDir, $outDir, $tempDir)) {
        if (Test-Path -LiteralPath $directory) {
            $item = Get-Item -LiteralPath $directory -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Refusing to clean a linked build directory: $directory"
            }
            Remove-Item -LiteralPath $item.FullName -Recurse -Force
        }
    }
}
foreach ($directory in @($objDir, $outDir, $tempDir)) {
    $null = [IO.Directory]::CreateDirectory($directory)
}

$savedEnvironment = @{}
foreach ($name in @('TEMP', 'TMP', '_CL_', 'VSLANG')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$savedEncoding = [Console]::OutputEncoding
$buildExit = -1
$timer = [Diagnostics.Stopwatch]::StartNew()
try {
    $env:TEMP = $tempDir
    $env:TMP = $tempDir
    $env:_CL_ = "/MP$Jobs /FS"
    # English native diagnostics avoid GBK/UTF-8 decoding mismatches. The file
    # logger writes UTF-8 directly, independently of shell console decoding.
    $env:VSLANG = '1033'
    [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
    $buildArguments = @(
        $project,
        '/nologo', '/t:Build', '/p:Configuration=Release', '/p:Platform=x64',
        '/m:1', '/verbosity:minimal',
        "/p:SolutionDir=$root/", "/p:IntDir=$objDir/", "/p:OutDir=$outDir/",
        '/p:PostBuildEventUseInBuild=false',
        '/fileLogger', "/fileLoggerParameters:LogFile=$log;Verbosity=normal;Encoding=UTF-8"
    )
    Write-Host "Building Release x64 with $Jobs compiler workers"
    Push-Location -LiteralPath $root
    try {
        & $msbuild @buildArguments
        $buildExit = $LASTEXITCODE
    } finally {
        Pop-Location
    }
} finally {
    $timer.Stop()
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
    [Console]::OutputEncoding = $savedEncoding
}

Write-Host ("MSBuild exit {0} after {1:hh\:mm\:ss}" -f $buildExit, $timer.Elapsed)
if ($buildExit -ne 0) { exit $buildExit }
if (!(Test-Path -LiteralPath $artifact -PathType Leaf)) {
    throw "Build reported success but the DLL is missing: $artifact"
}
$dll = Get-Item -LiteralPath $artifact
Write-Host "Artifact: $($dll.FullName)"
Write-Host "  size: $($dll.Length) bytes"
Write-Host "  mtime UTC: $($dll.LastWriteTimeUtc.ToString('O'))"
Write-Host "Log: $log"
exit 0

#requires -Version 7.0
<#
Deploy an already built F6 DLL by writing the existing destination object.
This preserves its runtime hardlinks. No game or profile operations occur here.
#>
[CmdletBinding()]
param(
    [string]$Artifact,
    [string]$InstallationRoot = 'D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$Artifact) { $Artifact = Join-Path $projectRoot 'build/out/OptiScaler.dll' }
$artifactPath = (Get-Item -LiteralPath $Artifact -ErrorAction Stop).FullName
$destination = (Get-Item -LiteralPath (Join-Path $InstallationRoot 'OptiScaler/official/F6/OptiScaler.dll') -ErrorAction Stop).FullName
if ([StringComparer]::OrdinalIgnoreCase.Equals($artifactPath, $destination)) {
    throw 'Artifact and destination must be different files.'
}
$backupRoot = Join-Path $projectRoot 'build/rollback'
$backupPath = Join-Path $backupRoot ('OptiScaler-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfff') + '-' + [Guid]::NewGuid().ToString('N') + '.dll')
$artifactTime = [IO.File]::GetLastWriteTimeUtc($artifactPath)
$originalTime = [IO.File]::GetLastWriteTimeUtc($destination)
$source = [IO.File]::Open($artifactPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    if ($source.Length -lt 64 -or $source.ReadByte() -ne 0x4d -or $source.ReadByte() -ne 0x5a) {
        throw 'Artifact does not contain a PE DOS header.'
    }
    $source.Position = 0
    # Open, never Create/rename/replace: the official file object must survive.
    # If the DLL is shared or unavailable, fail before writing, without retries.
    $target = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $restored = $false
    try {
        $null = [IO.Directory]::CreateDirectory($backupRoot)
        $backup = [IO.File]::Open($backupPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        try {
            $target.CopyTo($backup)
            $backup.Flush($true)
            if ($backup.Length -ne $target.Length) { throw 'Incomplete rollback copy; destination was not changed.' }
        } finally { $backup.Dispose() }
        [IO.File]::SetLastWriteTimeUtc($backupPath, $originalTime)
        $target.Position = 0
        try {
            $source.CopyTo($target)
            $target.SetLength($source.Length)
            $target.Flush($true)
        } catch {
            $deploymentFailure = $_
            try {
                $rollback = [IO.File]::OpenRead($backupPath)
                try {
                    $target.Position = 0
                    $rollback.CopyTo($target)
                    $target.SetLength($rollback.Length)
                    $target.Flush($true)
                } finally { $rollback.Dispose() }
                $restored = $true
            } catch {
                throw "Deployment failed ($($deploymentFailure.Exception.Message)); rollback also failed ($($_.Exception.Message)). Retained backup: $backupPath"
            }
            throw "Deployment failed; original bytes restored in place. $($deploymentFailure.Exception.Message) Backup: $backupPath"
        }
    } finally {
        $target.Dispose()
        if ($restored) { [IO.File]::SetLastWriteTimeUtc($destination, $originalTime) }
    }
    [IO.File]::SetLastWriteTimeUtc($destination, $artifactTime)
    [pscustomobject]@{
        Status = 'DEPLOYED_IN_PLACE'
        Artifact = $artifactPath
        Destination = $destination
        Bytes = $source.Length
        LastWriteTimeUtc = $artifactTime.ToString('O')
        Rollback = $backupPath
        ProfilesChanged = $false
    } | ConvertTo-Json
} finally { $source.Dispose() }

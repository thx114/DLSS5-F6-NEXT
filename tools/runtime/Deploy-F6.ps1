#requires -Version 7.0
<#
Deploy an already built F6 DLL by writing the existing destination object.
This preserves its runtime hardlinks. No game or profile operations occur here.
If the destination is in use, stage the candidate beside it as OptiScaler.dll.next.
#>
[CmdletBinding()]
param(
    [string]$Artifact,
    [ValidatePattern('^(?:official/F6|local/[A-Za-z0-9_-]+)$')]
    [string]$BuildId = 'official/F6',
    [string]$InstallationRoot = 'D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$Artifact) { $Artifact = Join-Path $projectRoot 'build/out/OptiScaler.dll' }
$artifactPath = (Get-Item -LiteralPath $Artifact -ErrorAction Stop).FullName
$destination = (Get-Item -LiteralPath (Join-Path $InstallationRoot "OptiScaler/$BuildId/OptiScaler.dll") -ErrorAction Stop).FullName
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
    $artifactHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($source))
    $source.Position = 0
    # Open, never Create/rename/replace: the selected file object must survive.
    # Actual file contention decides staging; process names are irrelevant.
    try {
        $target = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    } catch {
        $nativeError = $_.Exception.GetBaseException().HResult -band 0xffff
        # ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION / ERROR_USER_MAPPED_FILE.
        # Permission, missing-file and storage failures remain errors.
        if ($nativeError -notin @(32, 33, 1224)) { throw }
        $nextPath = $destination + '.next'
        if (![StringComparer]::OrdinalIgnoreCase.Equals($artifactPath, $nextPath)) {
            # Publish only a complete, verified sidecar; retain an older .next on failure.
            $temporaryPath = $nextPath + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
            try {
                $pending = [IO.File]::Open($temporaryPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
                try {
                    $source.CopyTo($pending)
                    $pending.Flush($true)
                    $pending.Position = 0
                    $pendingHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($pending))
                    if ($pending.Length -ne $source.Length -or $pendingHash -ne $artifactHash) {
                        throw 'Pending DLL verification failed; installed file was not changed.'
                    }
                } finally { $pending.Dispose() }
                [IO.File]::SetLastWriteTimeUtc($temporaryPath, $artifactTime)
                [IO.File]::Move($temporaryPath, $nextPath, $true)
            } finally {
                if ([IO.File]::Exists($temporaryPath)) { [IO.File]::Delete($temporaryPath) }
            }
        }
        [pscustomobject]@{
            Status = 'STAGED_NEXT_FILE_IN_USE'
            Artifact = $artifactPath
            Destination = $destination
            PendingArtifact = $nextPath
            SHA256 = $artifactHash
            Bytes = $source.Length
            LastWriteTimeUtc = $artifactTime.ToString('O')
            NativeError = $nativeError
            InstalledChanged = $false
            ProfilesChanged = $false
        } | ConvertTo-Json
        return
    }
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
        SHA256 = $artifactHash
        Bytes = $source.Length
        LastWriteTimeUtc = $artifactTime.ToString('O')
        Rollback = $backupPath
        InstalledChanged = $true
        ProfilesChanged = $false
    } | ConvertTo-Json
} finally { $source.Dispose() }

#requires -Version 7.0
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$fixtureRoot = Join-Path $projectRoot ('build/deployment-tests/' + [Guid]::NewGuid().ToString('N'))
$fixtureRuntime = Join-Path $fixtureRoot 'tools/runtime'
$installationRoot = Join-Path $fixtureRoot 'installation'
$officialRoot = Join-Path $installationRoot 'OptiScaler/official/F6'
$null = [IO.Directory]::CreateDirectory($fixtureRuntime)
$null = [IO.Directory]::CreateDirectory($officialRoot)
$deploymentScript = Join-Path $fixtureRuntime 'Deploy-F6.ps1'
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools/runtime/Deploy-F6.ps1') -Destination $deploymentScript
$destination = Join-Path $officialRoot 'OptiScaler.dll'
$nextPath = $destination + '.next'
$artifact = Join-Path $fixtureRoot 'candidate.dll'
$config = Join-Path $officialRoot 'OptiScaler.ini'
$script:checks = 0
function Assert-Deployment([bool]$Condition, [string]$Message) {
    $script:checks++
    if (!$Condition) { throw $Message }
}
function Get-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Set-Candidate([byte]$Fill, [int]$Length) {
    $bytes = [byte[]]::new($Length)
    [Array]::Fill[byte]($bytes, $Fill)
    $bytes[0] = 0x4d
    $bytes[1] = 0x5a
    [IO.File]::WriteAllBytes($artifact, $bytes)
}
# Fail if the deployment script ever resumes process discovery.
function Get-CimInstance { throw 'Deployment must not inspect game processes.' }
function Get-Process { throw 'Deployment must not inspect game processes.' }
[IO.File]::WriteAllBytes($destination, [byte[]](1..100))
[IO.File]::WriteAllText($config, 'user configuration remains unchanged')
$originalHash = Get-Hash $destination
$configHash = Get-Hash $config
$runtimeLinks = @(1..3 | ForEach-Object {
    $link = Join-Path $fixtureRoot "runtime-$_.dll"
    $null = New-Item -ItemType HardLink -Path $link -Target $destination
    $link
})
$originalId = (& fsutil file queryfileid $destination) -join "`n"
if ($LASTEXITCODE -ne 0) { throw 'Fixture file ID query failed.' }

# Writable target: all aliases change together and rollback contains original bytes.
Set-Candidate 21 512
$result = & $deploymentScript -Artifact $artifact -InstallationRoot $installationRoot | ConvertFrom-Json
Assert-Deployment ($result.Status -eq 'DEPLOYED_IN_PLACE') 'Writable target was not deployed.'
Assert-Deployment $result.InstalledChanged 'Deployment did not report an installed change.'
Assert-Deployment ((Get-Hash $destination) -eq (Get-Hash $artifact)) 'Installed candidate mismatch.'
Assert-Deployment ((Get-Hash $result.Rollback) -eq $originalHash) 'Rollback bytes mismatch.'
Assert-Deployment ($result.SHA256 -eq (Get-Hash $artifact)) 'Deployment hash mismatch.'
foreach ($link in $runtimeLinks) {
    Assert-Deployment ((Get-Hash $link) -eq $result.SHA256) 'Hardlink content mismatch.'
}
Assert-Deployment (((& fsutil file queryfileid $destination) -join "`n") -eq $originalId) 'File identity changed.'
Assert-Deployment (!(Test-Path -LiteralPath $nextPath)) 'Writable target unexpectedly created a .next file.'

# Lock through a runtime alias: stage without opening the installed object for writing.
$installedHash = Get-Hash $destination
$backupsBefore = @(Get-ChildItem -LiteralPath (Join-Path $fixtureRoot 'build/rollback')).Count
$held = [IO.File]::Open($runtimeLinks[0], [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    Set-Candidate 42 1024
    $result = & $deploymentScript -Artifact $artifact -InstallationRoot $installationRoot | ConvertFrom-Json
    Assert-Deployment ($result.Status -eq 'STAGED_NEXT_FILE_IN_USE') 'Locked target was not staged.'
    Assert-Deployment (!$result.InstalledChanged) 'Staged candidate was reported as installed.'
    Assert-Deployment ($result.NativeError -eq 32) 'Expected a real sharing violation.'
    Assert-Deployment ($result.PendingArtifact -eq $nextPath) 'Wrong pending path.'
    Assert-Deployment ((Get-Hash $nextPath) -eq (Get-Hash $artifact)) 'Pending candidate mismatch.'
    Assert-Deployment ((Get-Hash $destination) -eq $installedHash) 'Staging changed installed bytes.'
    Assert-Deployment (@(Get-ChildItem -LiteralPath (Join-Path $fixtureRoot 'build/rollback')).Count -eq $backupsBefore) 'Staging created a false rollback.'

    # Replace an older sidecar only after a complete newer copy has been verified.
    Set-Candidate 63 2048
    $result = & $deploymentScript -Artifact $artifact -InstallationRoot $installationRoot | ConvertFrom-Json
    Assert-Deployment ((Get-Hash $nextPath) -eq $result.SHA256) 'Existing sidecar was not refreshed.'
    $result = & $deploymentScript -Artifact $nextPath -InstallationRoot $installationRoot | ConvertFrom-Json
    Assert-Deployment ($result.Status -eq 'STAGED_NEXT_FILE_IN_USE') 'Reusing the pending candidate failed.'

    # Failed publication retains both the previous sidecar and installed bytes.
    $pendingHash = Get-Hash $nextPath
    $heldPending = [IO.File]::Open($nextPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        Set-Candidate 84 4096
        $failed = $false
        try { $null = & $deploymentScript -Artifact $artifact -InstallationRoot $installationRoot } catch { $failed = $true }
        Assert-Deployment $failed 'Publication to an occupied .next should fail.'
        Assert-Deployment ((Get-Hash $nextPath) -eq $pendingHash) 'Failed publication damaged the previous sidecar.'
    } finally { $heldPending.Dispose() }
    Assert-Deployment ((Get-Hash $destination) -eq $installedHash) 'Failed publication changed installed bytes.'
    Assert-Deployment (@(Get-ChildItem -LiteralPath $officialRoot -Filter '*.tmp').Count -eq 0) 'Temporary publication file leaked.'
} finally { $held.Dispose() }

# Once released, promote .next using the same in-place flow and preserve all aliases.
$pendingHash = Get-Hash $nextPath
$result = & $deploymentScript -Artifact $nextPath -InstallationRoot $installationRoot | ConvertFrom-Json
Assert-Deployment ($result.Status -eq 'DEPLOYED_IN_PLACE') 'Released target did not deploy the pending candidate.'
Assert-Deployment ((Get-Hash $destination) -eq $pendingHash) 'Promoted candidate mismatch.'
Assert-Deployment ((Get-Hash $result.Rollback) -eq $installedHash) 'Promotion rollback mismatch.'
foreach ($link in $runtimeLinks) {
    Assert-Deployment ((Get-Hash $link) -eq $pendingHash) 'Promotion broke a runtime alias.'
}
Assert-Deployment (((& fsutil file queryfileid $destination) -join "`n") -eq $originalId) 'Promotion changed file identity.'

# Candidate validation errors remain errors, even when the target is occupied.
$held = [IO.File]::Open($destination, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    [IO.File]::WriteAllBytes($artifact, [byte[]](1..100))
    $failed = $false
    try { $null = & $deploymentScript -Artifact $artifact -InstallationRoot $installationRoot } catch { $failed = $true }
    Assert-Deployment $failed 'Invalid candidate should fail.'
    Assert-Deployment ((Get-Hash $nextPath) -eq $pendingHash) 'Invalid candidate replaced the sidecar.'
} finally { $held.Dispose() }
Assert-Deployment ((Get-Hash $config) -eq $configHash) 'User configuration changed.'

# An isolated build updates its own runtime aliases without touching official F6.
$localRoot = Join-Path $installationRoot 'OptiScaler/local/F6-ASI'
$null = [IO.Directory]::CreateDirectory($localRoot)
$localDestination = Join-Path $localRoot 'OptiScaler.dll'
[IO.File]::WriteAllBytes($localDestination, [byte[]](1..100))
$localLink = Join-Path $fixtureRoot 'local-runtime.dll'
$null = New-Item -ItemType HardLink -Path $localLink -Target $localDestination
$localId = (& fsutil file queryfileid $localDestination) -join "`n"
Set-Candidate 105 768
$result = & $deploymentScript -Artifact $artifact -BuildId 'local/F6-ASI' -InstallationRoot $installationRoot | ConvertFrom-Json
Assert-Deployment ($result.Status -eq 'DEPLOYED_IN_PLACE') 'Local build was not deployed.'
Assert-Deployment ((Get-Hash $localLink) -eq (Get-Hash $artifact)) 'Local runtime alias did not update.'
Assert-Deployment (((& fsutil file queryfileid $localDestination) -join "`n") -eq $localId) 'Local file identity changed.'
Assert-Deployment ((Get-Hash $destination) -eq $pendingHash) 'Local deployment changed official F6.'
Assert-Deployment ((Get-Hash $config) -eq $configHash) 'Local deployment changed user configuration.'
foreach ($invalidBuild in @('../F6', 'local/../F6', 'official/F6/../Other', 'local/F6/extra')) {
    $failed = $false
    try { $null = & $deploymentScript -Artifact $artifact -BuildId $invalidBuild -InstallationRoot $installationRoot } catch { $failed = $true }
    Assert-Deployment $failed 'Invalid build identity should be rejected.'
}
[pscustomobject]@{ Status='PASS'; Checks=$script:checks; Fixture=$fixtureRoot; OfficialInstallationTouched=$false } | ConvertTo-Json

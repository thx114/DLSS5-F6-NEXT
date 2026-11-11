#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [string[]]$Case = @(),
    [switch]$Hardware
)
# Run only an already compiled executable. Lead remains the sole compiler.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$fixtureExe = (Resolve-Path -LiteralPath $Executable).Path
$available = @(& $fixtureExe --list)
if ($LASTEXITCODE -ne 0 -or $available.Count -eq 0) { throw 'Fixture case enumeration failed.' }
if ($Hardware) {
    $available += @(& $fixtureExe --list-hardware)
    if ($LASTEXITCODE -ne 0) { throw 'Hardware case enumeration failed.' }
}
$selected = @(if ($Case.Count) { $Case } else { $available })
foreach ($name in $selected) {
    if ($name -notin $available) { throw "Unknown fixture case: $name" }
    # One process per case isolates quarantined product globals. Never reset
    # production fault flags or force-retire tokens to prepare the next case.
    & $fixtureExe --case $name
    if ($LASTEXITCODE -ne 0) { throw "DetailHistory case '$name' failed: exit $LASTEXITCODE" }
}
Write-Output "PASS $($selected.Count) isolated DetailHistory runtime cases."

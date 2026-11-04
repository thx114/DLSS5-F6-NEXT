#requires -Version 7.0
<#
Extract recorded inputs and completion evidence from a saved F6 log. Use a
closed or copied log: a changing file cannot supply a reproducible hash.
Counts describe log lines, never generated pixels or exclusive GPU cost.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Log,
    [Parameter(Mandatory)][ValidateRange(1, 2147483647)][int]$ExpectedPid,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{7,40}$')][string]$ExpectedCommit,
    [string]$Output,
    [ValidateRange(1, 256)][int]$SampleLimit = 64
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$path = (Get-Item -LiteralPath $Log -ErrorAction Stop).FullName
if ($Output -and [StringComparer]::OrdinalIgnoreCase.Equals($path, [IO.Path]::GetFullPath($Output))) {
    throw 'Output must not replace the input log.'
}
$before = Get-Item -LiteralPath $path
$counts = [ordered]@{
    ProjectNgxDx12CreateLogLines = 0; ProjectNgxDx12EvaluateLogLines = 0
    StreamlineEvaluateLogLines = 0; NrCadenceLogLines = 0
    FgDispatchOkLines = 0; FgTaggedDepthLogLines = 0; FgDepthRefusalLogLines = 0
    ZzzSourceSdkLogLines = 0; GpuCompletedLogLines = 0
}
$nr = [Collections.Generic.List[object]]::new()
$fg = [Collections.Generic.List[object]]::new()
$zzz = [Collections.Generic.List[object]]::new()
$gpu = [Collections.Generic.List[object]]::new()
$versions = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$observedPids = [Collections.Generic.HashSet[int]]::new()

function Read-Fields([string]$Line) {
    $fields = [ordered]@{}
    foreach ($match in [regex]::Matches($Line, '(\w+)=([^\s]+)')) {
        $fields[$match.Groups[1].Value] = $match.Groups[2].Value
    }
    return $fields
}
function Save-Sample($Rows, [int]$Number, [string]$Line) {
    $Rows.Add([pscustomobject]@{ Line = $Number; Fields = (Read-Fields $Line); Text = $Line })
    if ($Rows.Count -gt $SampleLimit) { $Rows.RemoveAt(0) }
}

$lineNumber = 0
$stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
$reader = [IO.StreamReader]::new($stream, [Text.UTF8Encoding]::new($false), $true)
try {
    while ($null -ne ($line = $reader.ReadLine())) {
        ++$lineNumber
        if ($line -match 'OptiScaler v[^\r\n]*\(([0-9a-fA-F]{7,40})\) loaded') {
            $null = $versions.Add($Matches[1])
        }
        if ($line.Contains('NVSDK_NGX_D3D12_CreateFeature')) { ++$counts.ProjectNgxDx12CreateLogLines }
        if ($line.Contains('NVSDK_NGX_D3D12_EvaluateFeature')) { ++$counts.ProjectNgxDx12EvaluateLogLines }
        if ($line.Contains('StreamlineHooks::hkslEvaluateFeature frameIndex:')) { ++$counts.StreamlineEvaluateLogLines }
        if ($line.Contains('DLSSG_Dx12::Dispatch Result: Ok')) { ++$counts.FgDispatchOkLines }
        if ($line.Contains('R1054 FG depth refusal:')) { ++$counts.FgDepthRefusalLogLines }
        $evidenceRow = $false
        if ($line.Contains('DLSS-NR cadence recorded')) {
            ++$counts.NrCadenceLogLines; Save-Sample $nr $lineNumber $line; $evidenceRow = $true
        }
        if ($line.Contains('R1054 FG tagged depth input:')) {
            ++$counts.FgTaggedDepthLogLines; Save-Sample $fg $lineNumber $line; $evidenceRow = $true
        }
        if ($line.Contains('F6 ZZZ source SDK Present:')) {
            ++$counts.ZzzSourceSdkLogLines; Save-Sample $zzz $lineNumber $line; $evidenceRow = $true
        }
        if ($line.Contains('DLSS-NR GPU evidence completed scope=')) {
            ++$counts.GpuCompletedLogLines; Save-Sample $gpu $lineNumber $line
        }
        # Only PID-bearing product evidence can bind this log to the requested
        # process. A file name or another application's embedded message cannot.
        if (($evidenceRow -or $line.Contains('pid=')) -and $line -match '\bpid=(\d+)\b') {
            $null = $observedPids.Add([int]$Matches[1])
        }
    }
} finally { $reader.Dispose() }
$hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
$after = Get-Item -LiteralPath $path
if ($before.Length -ne $after.Length -or $before.LastWriteTimeUtc -ne $after.LastWriteTimeUtc) {
    throw 'Log changed while reading; copy it once and analyze the saved copy.'
}
if ($versions.Count -ne 1 -or !@($versions)[0].StartsWith($ExpectedCommit, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Expected one loaded commit $ExpectedCommit; found $(@($versions) -join ', ')."
}
if ($observedPids.Count -ne 1 -or !$observedPids.Contains($ExpectedPid)) {
    throw "Expected only PID $ExpectedPid; observed $(@($observedPids) -join ', ')."
}
$gpuChecks = @($gpu | ForEach-Object {
    $fields = $_.Fields
    $required = @('queue', 'fence', 'fenceTarget', 'fenceCompleted')
    $known = @($required | Where-Object { !$fields.Contains($_) }).Count -eq 0
    $complete = $false
    if ($known) {
        $target = [uint64]$fields.fenceTarget
        $completed = [uint64]$fields.fenceCompleted
        $complete = $fields.queue -ne '0x0' -and $fields.fence -ne '0x0' -and
            $target -gt 0 -and $completed -ne [uint64]::MaxValue -and $completed -ge $target
    }
    [pscustomobject]@{ Line = $_.Line; OwnFenceFieldsKnown = $known; RecordedOwnFenceCompleted = $complete }
})
$result = [ordered]@{
    Log = $path; Sha256 = $hash; Bytes = $after.Length; Lines = $lineNumber
    LoadedCommit = @($versions)[0]; Pid = $ExpectedPid
    IdentityScope = 'loaded-version-and-recorded-pid; loaded-memory-hash-and-process-creation-UNKNOWN'
    Counters = $counts; RetainedRowsPerKind = $SampleLimit; RetainedRows = 'last rows only'
    NrCadence = @($nr); FgTaggedDepth = @($fg); ZzzSourceSdk = @($zzz)
    GpuCompleted = @($gpu); GpuFenceChecks = $gpuChecks
    Boundary = 'Recording/input evidence only. NR history may be queue-ordered and GPU-pending. GPU intervals are inclusive and cannot be added or treated as whole-game performance. Generated pixels, temporal detail, skipped-model reuse benefit and net performance require separate validation.'
}
$json = $result | ConvertTo-Json -Depth 8
if ($Output) { [IO.File]::WriteAllText([IO.Path]::GetFullPath($Output), $json, [Text.UTF8Encoding]::new($false)) }
$json

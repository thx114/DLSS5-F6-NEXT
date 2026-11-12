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
    [ValidateRange(1, 256)][int]$SampleLimit = 64,
    [switch]$Latency,
    [ValidateRange(1, 2147483647)][int]$LatencyFirstLine = 1,
    [ValidateRange(1, 2147483647)][int]$LatencyLastLine = [int]::MaxValue
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
    ProjectNgxDx11CreateLogLines = 0; ProjectNgxDx11EvaluateLogLines = 0
    StreamlineEvaluateLogLines = 0; NrCadenceLogLines = 0
    FgDispatchOkLines = 0; FgTaggedDepthLogLines = 0; FgDepthRefusalLogLines = 0
    ZzzSourceSdkLogLines = 0; GpuCompletedLogLines = 0
    ZzzNgxRouteLogLines = 0; ZzzSelectedCreateLogLines = 0; ZzzSelectedEvaluateLogLines = 0
    GenshinSdkClosureLogLines = 0; GenshinStreamOutputLogLines = 0; GenshinOwnerOutputLogLines = 0
    GenshinOwnerShownLogLines = 0; GenshinPhysicalRefusalLogLines = 0
    FgCapabilityLogLines = 0; FgConfigurationLogLines = 0
    GenshinVisibilityLogLines = 0; GenshinPresentCpuLogLines = 0
    GenshinProducerProgressLogLines = 0; GenshinProducerComparisonLogLines = 0
}
$nr = [Collections.Generic.List[object]]::new()
$fg = [Collections.Generic.List[object]]::new()
$zzz = [Collections.Generic.List[object]]::new()
$gpu = [Collections.Generic.List[object]]::new()
$zzzNgx = [Collections.Generic.List[object]]::new()
$genshinSdk = [Collections.Generic.List[object]]::new()
$genshinOutput = [Collections.Generic.List[object]]::new()
$genshinOwner = [Collections.Generic.List[object]]::new()
$genshinRefusal = [Collections.Generic.List[object]]::new()
$fgCapability = [Collections.Generic.List[object]]::new()
$fgConfiguration = [Collections.Generic.List[object]]::new()
$genshinVisibility = [Collections.Generic.List[object]]::new()
$genshinPresentCpu = [Collections.Generic.List[object]]::new()
$genshinProducerProgress = [Collections.Generic.List[object]]::new()
$genshinProducerComparison = [Collections.Generic.List[object]]::new()
$versions = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$observedPids = [Collections.Generic.HashSet[int]]::new()

function Read-Fields([string]$Line) {
    $fields = [ordered]@{}
    foreach ($match in [regex]::Matches($Line, '(\w+)=([^\s]+)')) {
        $fields[$match.Groups[1].Value] = $match.Groups[2].Value.TrimEnd(';')
    }
    return $fields
}
function Save-Sample($Rows, [int]$Number, [string]$Line) {
    $Rows.Add([pscustomobject]@{ Line = $Number; Fields = (Read-Fields $Line); Text = $Line })
    if ($Rows.Count -gt $SampleLimit) { $Rows.RemoveAt(0) }
}

if ($LatencyFirstLine -gt $LatencyLastLine) { throw 'Invalid latency line range.' }
$reflexRows = [Collections.Generic.List[object]]::new()
$reflexReports = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$partitionRows = [Collections.Generic.List[object]]::new()
$cpuLongest = @{}
$latencyCounts = [ordered]@{ ReflexRows = 0; RejectedReflexRows = 0; DuplicateReflexRows = 0; NrPartitionRows = 0; RejectedNrPartitions = 0 }
$nrFields = @('before_encode_ms', 'encode_ms', 'model_prepare_ms', 'model_ms', 'history_ms', 'resolve_ms', 'finish_ms')

function Read-ReflexIntervals($Fields, [int]$Number) {
    if ($Fields.estimated -ne 'false' -or $Fields.raw_units -ne 'us' -or $Fields.report_frame_valid -ne 'true' -or
        $Fields.queue_valid -ne 'true' -or $Fields.gpu_valid -ne 'true' -or $Fields.frame -notmatch '^[1-9]\d*$') { return }
    $ends = [uint64[]]::new(6)
    $i = 0
    foreach ($key in 'report_begin_us','queue_begin_us','gpu_begin_us','gpu_end_us','queue_end_us','report_end_us') {
        $v = [uint64]0
        if (![uint64]::TryParse($Fields[$key], [ref]$v) -or $v -eq 0 -or $v -eq [uint64]::MaxValue -or
            ($i -gt 0 -and $v -lt $ends[$i - 1])) { return }
        $ends[$i++] = $v
    }
    # Subtract validated endpoints, never two rounded log durations.
    [pscustomobject]@{
        Line = $Number; ReportFrame = $Fields.frame; ReportMs = ($ends[5] - $ends[0]) / 1000.0
        PreQueueReportMs = ($ends[1] - $ends[0]) / 1000.0
        PreGpuQueueMs = ($ends[2] - $ends[1]) / 1000.0; GpuSpanMs = ($ends[3] - $ends[2]) / 1000.0
        PostGpuReportMs = ($ends[5] - $ends[3]) / 1000.0
    }
}
function Read-NrPartition($Fields, [int]$Number) {
    if ($Fields.disjoint -ne 'true' -or $Fields.same_recording -ne 'true' -or
        $Fields.queue -notmatch '^0x[0-9a-f]+$' -or $Fields.queue -match '^0x0+$' -or
        $Fields.completion -ne 'existing_actual_queue_fence') { return }
    $target = [uint64]0; $completed = [uint64]0
    if (![uint64]::TryParse($Fields.fenceTarget, [ref]$target) -or $target -eq 0 -or
        ![uint64]::TryParse($Fields.fenceCompleted, [ref]$completed) -or $completed -eq [uint64]::MaxValue -or
        $completed -lt $target) { return }
    $row = [ordered]@{ Line = $Number; Queue = $Fields.queue; Dispatch = $Fields.sampleDispatch; Epoch = $Fields.sampleEpoch }
    $sum = 0.0
    foreach ($key in $nrFields + @('total_ms')) {
        $value = 0.0
        if ($Fields[$key] -notmatch '^\d+(\.\d+)?$' -or
            ![double]::TryParse($Fields[$key], [Globalization.CultureInfo]::InvariantCulture, [ref]$value) -or
            ![double]::IsFinite($value)) { return }
        $row[$key] = $value
        if ($key -ne 'total_ms') { $sum += $value }
    }
    if ([Math]::Abs($sum - $row.total_ms) -gt 0.001) { return } # log rounds eight fields to 0.0001 ms
    [pscustomobject]$row
}
function Measure-Rows($Rows, [string]$Property) {
    if (!$Rows.Count) { return $null }
    $values = @($Rows | ForEach-Object { $_.$Property } | Sort-Object)
    $n = $values.Count
    $low = [int][Math]::Floor(($n - 1) / 2)
    $high = [int][Math]::Floor($n / 2)
    [ordered]@{ Samples = $n; MedianMs = ($values[$low] + $values[$high]) / 2.0
        P95Ms = $values[[int][Math]::Ceiling(0.95 * $n) - 1]; MaxMs = $values[$n - 1] }
}

$lineNumber = 0
$stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
$reader = [IO.StreamReader]::new($stream, [Text.UTF8Encoding]::new($false), $true, 4096, $true)
try {
    while ($null -ne ($line = $reader.ReadLine())) {
        ++$lineNumber
        if ($line -match 'OptiScaler v[^\r\n]*\(([0-9a-fA-F]{7,40})\) loaded') {
            $null = $versions.Add($Matches[1])
        }
        if ($line.Contains('NVSDK_NGX_D3D12_CreateFeature')) { ++$counts.ProjectNgxDx12CreateLogLines }
        if ($line.Contains('NVSDK_NGX_D3D12_EvaluateFeature')) { ++$counts.ProjectNgxDx12EvaluateLogLines }
        if ($line.Contains('NVSDK_NGX_D3D11_CreateFeature')) { ++$counts.ProjectNgxDx11CreateLogLines }
        if ($line.Contains('NVSDK_NGX_D3D11_EvaluateFeature')) { ++$counts.ProjectNgxDx11EvaluateLogLines }
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
        if ($line.Contains('ZZZ NGX DX12 entry=')) {
            ++$counts.ZzzNgxRouteLogLines; Save-Sample $zzzNgx $lineNumber $line
            if ($line.Contains('entry=Create owner=selected ')) { ++$counts.ZzzSelectedCreateLogLines }
            if ($line.Contains('entry=Evaluate owner=selected ')) { ++$counts.ZzzSelectedEvaluateLogLines }
        }
        if ($line.Contains('R1069 sdk-closure:')) {
            ++$counts.GenshinSdkClosureLogLines; Save-Sample $genshinSdk $lineNumber $line
        }
        if ($line.Contains('F6 stream-output:')) {
            ++$counts.GenshinStreamOutputLogLines; Save-Sample $genshinOutput $lineNumber $line
        }
        if ($line.Contains('F6 owner-output:')) {
            ++$counts.GenshinOwnerOutputLogLines; Save-Sample $genshinOwner $lineNumber $line
            if ($line.Contains('event=shown-after-physical-tail ')) { ++$counts.GenshinOwnerShownLogLines }
        }
        if ($line.Contains('F6 physical-output refusal:')) {
            ++$counts.GenshinPhysicalRefusalLogLines; Save-Sample $genshinRefusal $lineNumber $line
        }
        if ($line.Contains('F6 FG capability:')) {
            ++$counts.FgCapabilityLogLines; Save-Sample $fgCapability $lineNumber $line
        }
        if ($line.Contains('F6 FG configuration:')) {
            ++$counts.FgConfigurationLogLines; Save-Sample $fgConfiguration $lineNumber $line
        }
        if ($line.Contains('R846 FG visibility-boundary:')) {
            ++$counts.GenshinVisibilityLogLines; Save-Sample $genshinVisibility $lineNumber $line
        }
        if ($line.Contains('R082 Present CPU window:')) {
            ++$counts.GenshinPresentCpuLogLines; Save-Sample $genshinPresentCpu $lineNumber $line
        }
        if ($line.Contains('Genshin producer progress:')) {
            ++$counts.GenshinProducerProgressLogLines; Save-Sample $genshinProducerProgress $lineNumber $line
        }
        if ($line.Contains('Genshin producer comparison:')) {
            ++$counts.GenshinProducerComparisonLogLines; Save-Sample $genshinProducerComparison $lineNumber $line
        }
        if ($line.Contains('DLSS-NR GPU evidence completed scope=')) {
            ++$counts.GpuCompletedLogLines; Save-Sample $gpu $lineNumber $line
        }
        if ($Latency -and $lineNumber -ge $LatencyFirstLine -and $lineNumber -le $LatencyLastLine) {
            if ($line.Contains('R858 Reflex raw:')) {
                ++$latencyCounts.ReflexRows
                $fields = Read-Fields $line
                $row = Read-ReflexIntervals $fields $lineNumber
                if ($row) {
                    $identity = [string]::Join(':', @($fields.pid, $fields.frame, $fields.report_begin_us,
                        $fields.queue_begin_us, $fields.gpu_begin_us, $fields.gpu_end_us,
                        $fields.queue_end_us, $fields.report_end_us))
                    if ($reflexReports.Add($identity)) { $reflexRows.Add($row) }
                    else { ++$latencyCounts.DuplicateReflexRows }
                } else { ++$latencyCounts.RejectedReflexRows }
            } elseif ($line.Contains('F6 NR GPU partition:')) {
                ++$latencyCounts.NrPartitionRows
                $row = Read-NrPartition (Read-Fields $line) $lineNumber
                if ($row) { $partitionRows.Add($row) } else { ++$latencyCounts.RejectedNrPartitions }
            } elseif ($line.Contains('CallerCPU span op=')) {
                $f = Read-Fields $line
                $ms = 0.0
                if ($f.op -and [double]::TryParse($f.cpuMs, [Globalization.CultureInfo]::InvariantCulture, [ref]$ms) -and
                    [double]::IsFinite($ms) -and $ms -ge 0) {
                    $entry = $cpuLongest[$f.op]
                    if (!$entry) { $entry = [ordered]@{ LoggedSamples = 0; Longest = $null }; $cpuLongest[$f.op] = $entry }
                    ++$entry.LoggedSamples
                    if (!$entry.Longest -or $ms -gt $entry.Longest.Ms) {
                        $entry.Longest = [ordered]@{ Ms = $ms; Line = $lineNumber; Frame = $f.frame
                            Owner = $f.owner; QpcStart = $f.qpcStart; QpcEnd = $f.qpcEnd }
                    }
                }
            }
        }
        # Only PID-bearing product evidence can bind this log to the requested
        # process. A file name or another application's embedded message cannot.
        if (($evidenceRow -or $line.Contains('pid=')) -and $line -match '\bpid=(\d+)\b') {
            $null = $observedPids.Add([int]$Matches[1])
        }
    }
    # Hash the same handle while writes and replacement are excluded. This
    # deliberately rejects a live writer instead of mixing two log snapshots.
    $stream.Position = 0
    $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream))
} finally { $reader.Dispose(); $stream.Dispose() }
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
    [pscustomobject]@{ Line = $_.Line; FenceFieldsKnown = $known; RecordedFenceTargetReached = $complete }
})
$result = [ordered]@{
    Log = $path; Sha256 = $hash; Bytes = $after.Length; Lines = $lineNumber
    LoadedCommit = @($versions)[0]; Pid = $ExpectedPid
    IdentityScope = 'loaded-version-and-recorded-pid; loaded-memory-hash-and-process-creation-UNKNOWN'
    Counters = $counts; RetainedRowsCapPerKind = $SampleLimit; RetainedRows = 'last rows only'
    RetainedRowCounts = @{ NrCadence = $nr.Count; FgTaggedDepth = $fg.Count; ZzzSourceSdk = $zzz.Count; GpuCompleted = $gpu.Count
        ZzzNgxRoute = $zzzNgx.Count; GenshinSdkClosure = $genshinSdk.Count; GenshinStreamOutput = $genshinOutput.Count
        GenshinOwnerOutput = $genshinOwner.Count; GenshinPhysicalRefusal = $genshinRefusal.Count
        FgCapability = $fgCapability.Count; FgConfiguration = $fgConfiguration.Count
        GenshinVisibility = $genshinVisibility.Count; GenshinPresentCpu = $genshinPresentCpu.Count
        GenshinProducerProgress = $genshinProducerProgress.Count; GenshinProducerComparison = $genshinProducerComparison.Count }
    NrCadence = @($nr); FgTaggedDepth = @($fg); ZzzSourceSdk = @($zzz)
    GpuCompleted = @($gpu); GpuFenceChecks = $gpuChecks
    ZzzNgxRoute = @($zzzNgx); GenshinSdkClosure = @($genshinSdk); GenshinStreamOutput = @($genshinOutput)
    GenshinOwnerOutput = @($genshinOwner); GenshinPhysicalRefusal = @($genshinRefusal)
    FgCapability = @($fgCapability); FgConfiguration = @($fgConfiguration)
    GenshinVisibility = @($genshinVisibility); GenshinPresentCpu = @($genshinPresentCpu)
    GenshinProducerProgress = @($genshinProducerProgress); GenshinProducerComparison = @($genshinProducerComparison)
    IndependentlyVerified = @{ SelectedSr = $false; GpuObjectOwnership = $false; GeneratedPixels = $false; TemporalDetail = $false; NetPerformance = $false }
    Boundary = 'Recorded calls and fence fields only; Matching log-line counts are not total calls or successes; only retained detail rows are capped. Requested/accepted FG configuration and raw SDK capability do not prove generated output or scanout. Present CPU windows and child-call wall time are not display FPS or GPU duration. SDK input acceptance, physical output completion, window reveal and generated pixels are separate. NR history may be queue-ordered and GPU-pending. GPU intervals are inclusive and cannot be added or treated as whole-game performance. Temporal detail, skipped-model reuse benefit and net performance require separate validation.'
}
if ($Latency) {
    $reflexSummary = [ordered]@{}
    foreach ($key in 'ReportMs','PreQueueReportMs','PreGpuQueueMs','GpuSpanMs','PostGpuReportMs') { $reflexSummary[$key] = Measure-Rows $reflexRows $key }
    $nrByQueue = [ordered]@{}
    foreach ($group in ($partitionRows | Group-Object -Property Queue)) {
        $phases = [ordered]@{}
        foreach ($key in $nrFields + @('total_ms')) { $phases[$key] = Measure-Rows $group.Group $key }
        $nrByQueue[$group.Name] = $phases
    }
    $result.Latency = [ordered]@{
        FirstLine = $LatencyFirstLine; LastLine = [Math]::Min($LatencyLastLine, $lineNumber); Counts = $latencyCounts
        Reflex = $reflexSummary; NrGpuPartitionByQueue = $nrByQueue; CpuThresholdedSpans = $cpuLongest
        PeakReport = $reflexRows | Sort-Object -Property ReportMs -Descending | Select-Object -First 1
        PeakPreGpuQueue = $reflexRows | Sort-Object -Property PreGpuQueueMs -Descending | Select-Object -First 1
        Boundary = 'Reflex ranges are non-estimated reported endpoints, not input-to-photon; duplicate endpoint reports are counted separately and excluded from statistics. PreQueueReportMs + PreGpuQueueMs + GpuSpanMs + PostGpuReportMs reconstruct each ReportMs; only PreGpuQueueMs is the GPU-before-start queue span. CPU spans are thresholded and may nest. NR partitions are disjoint within a single recording only. No join across CPU/Reflex/GPU clocks; no A/B or display claim.'
    }
}
$json = $result | ConvertTo-Json -Depth 8
if ($Output) { [IO.File]::WriteAllText([IO.Path]::GetFullPath($Output), $json, [Text.UTF8Encoding]::new($false)) }
$json

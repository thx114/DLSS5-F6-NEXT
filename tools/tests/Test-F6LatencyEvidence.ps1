#requires -Version 7.0
# Synthetic parser fixtures only; no game, SDK, or hardware timings are generated.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$reader = Join-Path $PSScriptRoot '../runtime/Get-F6RunEvidence.ps1'
$path = New-TemporaryFile
$checks = 0
function Check($Condition, $Message) {
    if (!$Condition) { throw $Message }
    ++$script:checks
}
try {
    $reflex = 'R858 Reflex raw: pid=42 frame=7 estimated=false raw_units=us report_frame_valid=true queue_valid=true gpu_valid=true report_begin_us=1000000 queue_begin_us=1001000 gpu_begin_us=1005000 gpu_end_us=1010000 queue_end_us=1011000 report_end_us=1013000'
    $part = 'F6 NR GPU partition: sampleDispatch=64 sampleEpoch=65 queue=0x12 fenceTarget=3 fenceCompleted=4 total_ms=28.0000 before_encode_ms=1.0000 encode_ms=2.0000 model_prepare_ms=3.0000 model_ms=4.0000 history_ms=5.0000 resolve_ms=6.0000 finish_ms=7.0000 disjoint=true same_recording=true completion=existing_actual_queue_fence'
    @(
        'OptiScaler v0.1.29 (abcdef1) loaded', $reflex,
        $reflex.Replace('estimated=false','estimated=true'),
        $reflex.Replace('gpu_end_us=1010000','gpu_end_us=1015000'),
        $reflex.Replace(' gpu_end_us=1010000',''),
        $part, $part.Replace('fenceCompleted=4','fenceCompleted=18446744073709551615'),
        $part.Replace('total_ms=28.0000','total_ms=30.0000'),
        $part.Replace(' finish_ms=7.0000',''),
        'CallerCPU span op=fgPresent pid=42 frame=17 cpuMs=70.001 qpcStart=100 qpcEnd=200 owner=0x44',
        'CallerCPU span op=fgPresent pid=42 frame=18 cpuMs=30.001 qpcStart=500 qpcEnd=700 owner=0x44',
        'CallerCPU span op=hiddenPresent pid=42 frame=18 cpuMs=60.001 qpcStart=300 qpcEnd=500 owner=0x44'
    ) | Set-Content -LiteralPath $path.FullName -Encoding utf8
    $a = & $reader -Log $path.FullName -ExpectedCommit abcdef1 -ExpectedPid 42 -Latency | ConvertFrom-Json -AsHashtable
    $t = $a.Latency
    Check ($t.Counts.ReflexRows -eq 4 -and $t.Counts.RejectedReflexRows -eq 3) 'Reject estimates, reversed/missing endpoints.'
    Check ($t.PeakReport.Line -eq 2 -and $t.PeakReport.ReportMs -eq 13) 'Preserve report line and outer interval.'
    Check ($t.Reflex.PreGpuQueueMs.MedianMs -eq 4 -and $t.Reflex.GpuSpanMs.MedianMs -eq 5 -and
           $t.Reflex.PostGpuReportMs.MedianMs -eq 3) 'Derive spans from one report; do not conflate queue and report start.'
    Check ($t.Counts.NrPartitionRows -eq 4 -and $t.Counts.RejectedNrPartitions -eq 3) 'Reject removal, non-summing and incomplete partitions.'
    Check ($t.NrGpuPartitionByQueue['0x12'].history_ms.MedianMs -eq 5) 'Keep per-queue GPU summaries.'
    Check ($t.CpuThresholdedSpans.fgPresent.LoggedSamples -eq 2 -and $t.CpuThresholdedSpans.fgPresent.Longest.Line -eq 10 -and
           $t.CpuThresholdedSpans.hiddenPresent.Longest.Line -eq 12) 'Preserve per-operation thresholded peaks.'
    $range = & $reader -Log $path.FullName -ExpectedCommit abcdef1 -ExpectedPid 42 -Latency -LatencyFirstLine 6 -LatencyLastLine 8 | ConvertFrom-Json -AsHashtable
    Check ($range.Latency.Counts.ReflexRows -eq 0 -and $range.Latency.Counts.NrPartitionRows -eq 3 -and
           $null -eq $range.Latency.Reflex.ReportMs) 'Limit statistics to selected line range and leave empty data unknown.'
    $noLatency = & $reader -Log $path.FullName -ExpectedCommit abcdef1 -ExpectedPid 42 | ConvertFrom-Json -AsHashtable
    Check (!$noLatency.Contains('Latency')) 'Default reader remains unchanged.'
    $badIdentityRejected = $false
    try { $null = & $reader -Log $path.FullName -ExpectedCommit abcdef1 -ExpectedPid 41 -Latency } catch { $badIdentityRejected = $true }
    Check $badIdentityRejected 'Never attribute a log to a different PID.'
    # Resume can leave a report begin long before queue submission. Its head
    # must remain visible instead of disappearing from the latency split.
    @('OptiScaler v0.1.29 (abcdef1) loaded', $reflex,
      $reflex.Replace('frame=7','frame=8').Replace('report_begin_us=1000000','report_begin_us=1000'),
      $reflex) | Set-Content -LiteralPath $path.FullName -Encoding utf8
    $gaps = (& $reader -Log $path.FullName -ExpectedCommit abcdef1 -ExpectedPid 42 -Latency |
        ConvertFrom-Json -AsHashtable).Latency
    Check ($gaps.PeakReport.PreQueueReportMs -eq 1000 -and $gaps.PeakReport.PreGpuQueueMs -eq 4) 'Keep the report-to-queue gap separate from GPU queue waiting.'
    $peak = $gaps.PeakReport
    Check ([Math]::Abs($peak.ReportMs - ($peak.PreQueueReportMs + $peak.PreGpuQueueMs +
        $peak.GpuSpanMs + $peak.PostGpuReportMs)) -lt 1e-9) 'The four same-report pieces must reconstruct its full span.'
    Check ($gaps.Counts.DuplicateReflexRows -eq 1 -and $gaps.Reflex.ReportMs.Samples -eq 2) 'Repeated endpoint reports are one sample, not extra rendered frames.'
    Check ($gaps.PeakReport.Line -eq 3) 'Retain the original line of each unique endpoint report.'
    "F6 latency evidence: $checks synthetic parser checks passed"
} finally { Remove-Item -LiteralPath $path.FullName }

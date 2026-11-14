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
    $pacing = 'F6 source pacing: pid=42 thread=23 sc=CAFE token_frame=7 original_token_lease=true source_role=direct_dlss observation_only=true sdk_call_known=true reflex_called=true reflex_result=0 limiter_called=true sdk_begin_qpc=1000000 sdk_end_qpc=1010000 marker_end_qpc=1011000 cleanup_end_qpc=1012000 unlock_end_qpc=1013000 reflex_begin_qpc=1014000 reflex_end_qpc=1039000 limiter_begin_qpc=1039000 limiter_end_qpc=1041000 tail_end_qpc=1043000 qpc_hz=1000000 reflex_wall_ms=99.0'
    $noWait = $pacing.Replace('reflex_called=true','reflex_called=false').Replace('limiter_called=true','limiter_called=false').
        Replace('reflex_begin_qpc=1014000','reflex_begin_qpc=0').Replace('reflex_end_qpc=1039000','reflex_end_qpc=0').
        Replace('limiter_begin_qpc=1039000','limiter_begin_qpc=0').Replace('limiter_end_qpc=1041000','limiter_end_qpc=0')
    @('OptiScaler v0.1.29 (abcdef1) loaded',
      ($reflex + ' gpu_active_valid=true gpu_active_ms=4.75 report_device=0x11'),
      ($reflex + ' gpu_active_valid=true gpu_active_ms=4.75 report_device=0x22'),
      ($reflex + ' gpu_active_valid=true gpu_active_ms=4.75 report_device=0x11'),
      $pacing, $noWait, $pacing.Replace('qpc_hz=1000000','qpc_hz=0'),
      $pacing.Replace('reflex_begin_qpc=1014000','reflex_begin_qpc=1012000'),
      $pacing.Replace('limiter_begin_qpc=1039000','limiter_begin_qpc=1038000'),
      $pacing.Replace('original_token_lease=true','original_token_lease=false'),
      $pacing.Replace('thread=23','thread=24').Replace('reflex_end_qpc=1039000','reflex_end_qpc=1031000').
        Replace('limiter_begin_qpc=1039000','limiter_begin_qpc=1031000').Replace('limiter_end_qpc=1041000','limiter_end_qpc=1033000'),
      $pacing.Replace('sc=CAFE','sc=BEEF')) |
        Set-Content -LiteralPath $path.FullName -Encoding utf8
    $paced = (& $reader -Log $path.FullName -ExpectedCommit abcdef1 -ExpectedPid 42 -Latency -TotalReflexTargetMs 10 |
        ConvertFrom-Json -AsHashtable).Latency
    Check ($paced.Counts.SourcePacingRows -eq 8 -and $paced.Counts.RejectedSourcePacingRows -eq 4) 'Reject bad clocks, waits under the lock, overlapping waits and invalid token leases.'
    $source = $paced.SourcePacingByThreadSwapchain | Where-Object { $_.Thread -eq '23' -and $_.Swapchain -eq 'CAFE' }
    Check ($source.Thread -eq '23' -and $source.Swapchain -eq 'CAFE' -and
           $source.Phases.SdkMs.MedianMs -eq 10 -and $source.Phases.PresentTailMs.MedianMs -eq 33) 'Keep source identity and inclusive SDK/tail QPC intervals.'
    Check ($source.Phases.ReflexMs.Samples -eq 1 -and $source.Phases.ReflexMs.MedianMs -eq 25 -and
           $source.Phases.LimiterMs.MedianMs -eq 2) 'Use original paired clocks, not rounded log values or zero durations for skipped calls.'
    Check ($source.PeakPresentTail.UnattributedAfterUnlockMs -eq 3 -and
           $source.Phases.UnattributedAfterUnlockMs.MaxMs -eq 30) 'Exclude the two observed waits from after-unlock remainder without double counting the inclusive tail.'
    Check ($paced.Reflex.GpuActiveMs.MedianMs -eq 4.75 -and $paced.Reflex.ReportMs.Samples -eq 2) 'Retain reported GPU activity and distinguish report devices in deduplication.'
    Check ($paced.Counts.DuplicateReflexRows -eq 1 -and $paced.Counts.ReflexRows -eq 3) 'Collapse the repeated same-device report while retaining the otherwise identical different-device report.'
    $otherThread = $paced.SourcePacingByThreadSwapchain | Where-Object { $_.Thread -eq '24' -and $_.Swapchain -eq 'CAFE' }
    $otherChain = $paced.SourcePacingByThreadSwapchain | Where-Object { $_.Thread -eq '23' -and $_.Swapchain -eq 'BEEF' }
    Check ($paced.SourcePacingByThreadSwapchain.Count -eq 3 -and $otherThread.Phases.ReflexMs.MedianMs -eq 17 -and
           $otherChain.Phases.ReflexMs.MedianMs -eq 25) 'Keep different threads on one chain and different chains on one thread in separate groups.'
    Check ($paced.TotalReflexTarget.Ms -eq 10 -and $paced.TotalReflexTarget.Above -eq 2 -and
           $paced.TotalReflexTarget.AtOrBelow -eq 0 -and !$paced.TotalReflexTarget.MedianAtOrBelow) 'Judge the threshold against full report spans, not the shorter GPU interval.'
    Check ($null -eq $gaps.Reflex.GpuActiveMs -and $gaps.TotalReflexTarget.Ms -eq 60 -and
           $gaps.TotalReflexTarget.AtOrBelow -eq 1 -and $gaps.TotalReflexTarget.Above -eq 1) 'Old logs keep unknown GPU activity; default threshold is 60ms.'
    "F6 latency evidence: $checks synthetic parser checks passed"
} finally { Remove-Item -LiteralPath $path.FullName }

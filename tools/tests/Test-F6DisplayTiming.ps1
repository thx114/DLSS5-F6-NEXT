#requires -Version 7.0
# Offline-only parser regression, no PresentMon, live PID, ETW or build needed.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$collector=Join-Path $PSScriptRoot '../runtime/Collect-F6DisplayTiming.ps1'
$temp=Join-Path ([IO.Path]::GetTempPath()) ('f6-display-tests-'+[guid]::NewGuid().ToString('N'))
$null=[IO.Directory]::CreateDirectory($temp)
$script:checks=0
function Check([bool]$Ok,[string]$Message) {
    ++$script:checks
    if (!$Ok) { throw $Message }
}
function Parse([string]$Name,[string[]]$Lines) {
    $csv=Join-Path $temp ($Name+'.csv'); $json=Join-Path $temp ($Name+'.json')
    [IO.File]::WriteAllLines($csv,$Lines,[Text.UTF8Encoding]::new($false))
    $original=(Get-FileHash -LiteralPath $csv).Hash
    $null=& $collector -TargetPid 42 -AnalyzeCsv $csv -OutputCsv $csv -OutputJson $json -CandidateDllPath (Join-Path $temp 'missing.dll')
    $r=Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
    Check ($original -eq (Get-FileHash -LiteralPath $csv).Hash) ($Name+': CSV unchanged')
    Check (!$r.loadedMemoryKnown -and $null -eq $r.loadedMemorySHA) ($Name+': no loaded memory identity claim')
    Check ($r.source.status -eq 'unknown' -and $r.sdk.status -eq 'unknown') ($Name+': source and SDK remain separate unknowns')
    Check ($r.targetPid -eq 42 -and $r.mode -eq 'offline') ($Name+': explicit PID/offline mode')
    return $r
}
try {
    foreach ($column in @('MsBetweenDisplayChange','msBetweenDisplayChange','DisplayedTime','DisplayedTime (ms)','DisplayedTimeMs')) {
        $r=Parse $column @(('ProcessID,SwapChainAddress,'+$column+',CPUFrameTime'),
            '42,0x123,10,20','42,0x123,10,20','99,0x123,1,1')
        Check ($r.status -eq 'observed' -and [Math]::Abs($r.display.hz-100) -lt 1e-8) ($column+': real display schema in ms')
        Check ($r.analysis.targetRowCount -eq 2) 'foreign PID is excluded'
        Check ([Math]::Abs($r.analysis.presentCadence.hz-50) -lt 1e-8) 'CPU cadence is separate from display'
    }
    foreach ($column in @('MsBetweenPresents','msBetweenPresents','CPUFrameTime','DisplayedTime (s)','DisplayLatency','MsUntilDisplayed')) {
        $r=Parse ('not-display-'+$column) @(('ProcessID,'+$column),'42,8.333','42,8.333')
        Check ($r.status -eq 'unknown' -and $null -eq $r.display.hz) ($column+': cannot claim display FPS')
    }
    foreach ($value in @('0','-1','NaN','Infinity','garbage','60001','N/A','')) {
        $r=Parse ('invalid-'+[guid]::NewGuid().ToString('N')) @('ProcessID,DisplayedTime',('42,'+$value),('42,'+$value))
        Check ($r.status -eq 'unknown' -and $null -eq $r.display.hz) ('invalid display value '+$value)
    }
    $r=Parse 'zero-plus-valid' @('ProcessID,DisplayedTime','42,0','42,16','42,16')
    Check ($r.status -eq 'observed' -and $r.display.zeroSamples -eq 1) 'zero duration is excluded and disclosed'
    $r=Parse 'invalid-plus-valid' @('ProcessID,DisplayedTime','42,-1','42,16','42,16')
    Check ($r.status -eq 'unknown') 'malformed sample cannot silently disappear'
    $r=Parse 'pid-missing' @('Application,DisplayedTime','game,8','game,8')
    Check ($r.status -eq 'unknown') 'missing PID refuses attribution'
    $r=Parse 'pid-wrong' @('ProcessID,DisplayedTime','77,8','77,8')
    Check ($r.status -eq 'unknown') 'different PID refuses attribution'
    $r=Parse 'all-dropped' @('ProcessID,DisplayedTime,Dropped','42,8,1','42,8,True')
    Check ($r.status -eq 'unknown') 'dropped presents do not prove display'
    $r=Parse 'multiple-chains' @('ProcessID,SwapChainAddress,DisplayedTime','42,a,10','42,a,10','42,b,20','42,b,20')
    Check ($r.status -eq 'unknown' -and $r.analysis.swapchains.Count -eq 2) 'swapchain timing cannot be combined'
    $r=Parse 'one-sample' @('ProcessID,DisplayedTime','42,8')
    Check ($r.status -eq 'unknown') 'one interval does not establish cadence'
    Write-Output ($script:checks.ToString()+' offline display timing checks passed')
} finally {
    # Only files created in this unique test directory; no recursive shell delete.
    Get-ChildItem -LiteralPath $temp -File | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
    Remove-Item -LiteralPath $temp
}

#requires -Version 7.0
<# Optional ETW observer; no downloads, injection or GPU retirement authority.
Offline: -TargetPid 42 -AnalyzeCsv saved.csv -OutputCsv copied.csv
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateRange(1,2147483647)][int]$TargetPid,
    [string]$PresentMonPath, [string]$AnalyzeCsv, [string]$CandidateDllPath,
    [string]$OutputJson, [string]$OutputCsv,
    [ValidateRange(1,3600)][int]$DurationSeconds=30,
    [ValidateRange(0,300)][int]$WarmupSeconds=5
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$OutputJson) { $OutputJson=Join-Path $root 'build/current-display-timing.json' }
if (!$OutputCsv) { $OutputCsv=Join-Path $root 'build/current-display-timing.csv' }
if (!$CandidateDllPath) { $CandidateDllPath=Join-Path $root 'build/out/OptiScaler.dll' }
$OutputJson=[IO.Path]::GetFullPath($OutputJson); $OutputCsv=[IO.Path]::GetFullPath($OutputCsv)
if ($OutputJson -eq $OutputCsv) { throw 'JSON and CSV paths must differ.' }
if ($AnalyzeCsv) {
    $AnalyzeCsv=(Get-Item -LiteralPath $AnalyzeCsv).FullName
    if ($OutputJson -eq $AnalyzeCsv) { throw 'JSON must not overwrite input CSV.' }
}
function Unknown([string]$Reason) { return [ordered]@{status='unknown';reason=$Reason;hz=$null} }
function FileIdentity([string]$Path) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $a=Get-Item -LiteralPath $Path; $sha=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    $b=Get-Item -LiteralPath $Path
    if ($a.Length -ne $b.Length -or $a.LastWriteTimeUtc -ne $b.LastWriteTimeUtc) { throw 'File changed during hashing.' }
    return [ordered]@{path=$a.FullName;bytes=$a.Length;lastWriteUtc=$a.LastWriteTimeUtc.ToString('o');sha256=$sha}
}
function Number($Value) {
    $n=0.0
    if (![double]::TryParse([string]$Value,[Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture,[ref]$n) -or ![double]::IsFinite($n)) { return $null }
    return $n
}
function Read-Timing([string]$Path) {
    $before=FileIdentity $Path; $rows=@(Import-Csv -LiteralPath $Path); $after=FileIdentity $Path
    if ($before.sha256 -ne $after.sha256) { throw 'CSV changed while parsing.' }
    $r=[ordered]@{status='unknown';reason='no target PID rows';csv=$after;rowCount=$rows.Count;
        targetRowCount=0;columns=@();display=(Unknown 'no display evidence');
        presentCadence=(Unknown 'no present interval evidence');swapchains=@()}
    if (!$rows.Count) { return $r }
    $columns=@($rows[0].PSObject.Properties.Name); $r.columns=$columns
    $pidColumn=@($columns | Where-Object { $_ -in @('ProcessID','ProcessId','PID') })
    if (!$pidColumn.Count) { $r.reason='CSV has no PID column; target identity unknown';return $r }
    $pidColumn=$pidColumn[0]; $target=@($rows | Where-Object { (Number $_.$pidColumn) -eq $TargetPid })
    $r.targetRowCount=$target.Count
    if (!$target.Count) { return $r }
    # Exact schemas with defined milliseconds, never guessed from numeric magnitude.
    $dc=@($columns | Where-Object { $_ -cin @('MsBetweenDisplayChange','msBetweenDisplayChange',
        'DisplayedTime','DisplayedTime (ms)','DisplayedTimeMs') })
    $pc=@($columns | Where-Object { $_ -in @('MsBetweenPresents','msBetweenPresents','CPUFrameTime') })
    $sc=@($columns | Where-Object { $_ -in @('SwapChainAddress','SwapChain') })
    $groups=@(if ($sc.Count) { $target | Group-Object -Property $sc[0] }
        else { [pscustomobject]@{Name='unattributed';Group=$target} })
    $chains=@(); $displays=@(); $presents=@()
    foreach ($group in $groups) {
        $dm=@(); $pm=@()
        foreach ($column in @($dc+$pc)) {
            $values=[Collections.Generic.List[double]]::new(); $invalid=0; $zeros=0; $dropped=0
            foreach ($row in $group.Group) {
                if ('Dropped' -in $columns -and [string]$row.Dropped -in @('1','true','True')) { ++$dropped;continue }
                $raw=[string]$row.$column
                if ($raw -in @('','NA','N/A')) { continue }
                $v=Number $raw
                if ($null -eq $v -or $v -lt 0 -or $v -gt 60000) { ++$invalid;continue }
                if ($v -eq 0) { ++$zeros;continue }
                $values.Add($v)
            }
            $m=[ordered]@{status='unknown';column=$column;unit='ms';samples=$values.Count;
                zeroSamples=$zeros;invalidSamples=$invalid;droppedRowsExcluded=$dropped;hz=$null;meanMs=$null;
                reason='need two positive finite millisecond samples and no malformed samples'}
            if ($values.Count -ge 2 -and !$invalid) {
                $sorted=@($values | Sort-Object); $sum=($values | Measure-Object -Sum).Sum
                $m.status='observed'; $m.meanMs=$sum/$values.Count; $m.hz=1000.0/$m.meanMs
                $m.reason='inverse mean interval/duration for this PID and swapchain; no generated-frame attribution'
                $m.p50Ms=$sorted[[int][Math]::Floor(($sorted.Count-1)*.50)]
                $m.p95Ms=$sorted[[int][Math]::Floor(($sorted.Count-1)*.95)]
            }
            if ($column -cin $dc) { $dm+=$m; $displays+=$m } else { $pm+=$m; $presents+=$m }
        }
        $chains+=[ordered]@{identity=$group.Name;rows=@($group.Group).Count;display=$dm;presentCadence=$pm}
    }
    $r.swapchains=$chains
    if (!$dc.Count) { $r.display=Unknown 'no actual display column; CPUFrameTime/MsBetweenPresents cannot prove display FPS' }
    elseif ($groups.Count -ne 1) { $r.display=Unknown 'multiple swapchains: inspect each, no combined display Hz' }
    else {
        $good=@($displays | Where-Object { $_.status -eq 'observed' })
        $r.display=if ($good.Count) { $good[0] } else { Unknown 'display columns lack valid nonzero timing evidence' }
    }
    if ($groups.Count -eq 1) {
        $good=@($presents | Where-Object { $_.status -eq 'observed' })
        if ($good.Count) { $r.presentCadence=$good[0] }
    }
    $r.status=$r.display.status; $r.reason=$r.display.reason; return $r
}
function Find-PresentMon {
    if ($PresentMonPath) { return (Get-Item -LiteralPath $PresentMonPath).FullName }
    foreach ($name in @('PresentMon.exe','PresentMon')) {
        $c=Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($c) { return $c.Source }
    }
    foreach ($base in @($env:ProgramFiles,[Environment]::GetEnvironmentVariable('ProgramFiles(x86)'),$env:LOCALAPPDATA)) {
        if (!$base) { continue }
        foreach ($relative in @('PresentMon/PresentMon.exe','Intel/PresentMon/PresentMon.exe','Programs/PresentMon/PresentMon.exe')) {
            $p=Join-Path $base $relative
            if (Test-Path -LiteralPath $p -PathType Leaf) { return [IO.Path]::GetFullPath($p) }
        }
    }
    return $null
}
function Invoke-Observer([string]$Executable,[string[]]$Arguments,[int]$TimeoutSeconds) {
    $s=[Diagnostics.ProcessStartInfo]::new(); $s.FileName=$Executable; $s.UseShellExecute=$false
    $s.CreateNoWindow=$true; $s.RedirectStandardOutput=$true; $s.RedirectStandardError=$true
    foreach ($arg in $Arguments) { $s.ArgumentList.Add($arg) }
    $p=[Diagnostics.Process]::new(); $p.StartInfo=$s
    try {
        $null=$p.Start(); $stdout=$p.StandardOutput.ReadToEndAsync(); $stderr=$p.StandardError.ReadToEndAsync()
        if (!$p.WaitForExit($TimeoutSeconds*1000)) { $p.Kill();$p.WaitForExit();throw 'PresentMon observer timed out; only this observer was stopped.' }
        return @{exitCode=$p.ExitCode;text=($stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult())}
    } finally { $p.Dispose() }
}
$report=[ordered]@{schema='f6.display-timing.v1';requestedUtc=[DateTime]::UtcNow.ToString('o');targetPid=$TargetPid;
    mode=$(if ($AnalyzeCsv) {'offline'} else {'capture'});loadedMemoryKnown=$false;loadedMemorySHA=$null;
    candidateDisk=$null;source=(Unknown 'source Hz not observed');sdk=(Unknown 'SDK counters not observed');
    display=(Unknown 'not collected');status='unknown';reason='not collected';tool=$null;process=$null;
    warmupSeconds=$WarmupSeconds;durationSeconds=$DurationSeconds;captureStartedUtc=$null;captureEndedUtc=$null;analysis=$null;
    authority='observations only; no GPU retirement or configuration authority'}
try {
    $report.candidateDisk=FileIdentity $CandidateDllPath
    if ($AnalyzeCsv) { $csv=$AnalyzeCsv }
    else {
        $exe=Find-PresentMon
        if (!$exe) { throw 'PresentMon CLI unavailable: use -PresentMonPath; nothing downloaded or installed.' }
        $report.tool=FileIdentity $exe
        $p=Get-Process -Id $TargetPid; $start=$p.StartTime.ToUniversalTime().ToString('o')
        $report.process=@{pid=$TargetPid;name=$p.ProcessName;startUtc=$start}
        $help=Invoke-Observer $exe @('--help') 8
        if ($help.text -notmatch '(?i)process_id') { $help=Invoke-Observer $exe @('-help') 8 }
        $prefix=if ($help.text -match '(?m)--process_id') {'--'} else {'-'}
        foreach ($required in @('process_id','output_file','timed','terminate_after_timed')) {
            if ($help.text -notmatch ('(?i)'+[regex]::Escape($prefix+$required)+'\b')) { throw ('Unsupported CLI: missing '+$required) }
        }
        if (Test-Path -LiteralPath $OutputCsv) { throw 'Capture CSV exists: choose fresh -OutputCsv; stale CSV is never reused.' }
        $null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($OutputCsv))
        Start-Sleep -Seconds $WarmupSeconds
        if ((Get-Process -Id $TargetPid).StartTime.ToUniversalTime().ToString('o') -ne $start) { throw 'Target PID reused during warmup.' }
        $arguments=@(($prefix+'process_id'),[string]$TargetPid,($prefix+'output_file'),$OutputCsv,
            ($prefix+'timed'),[string]$DurationSeconds,($prefix+'terminate_after_timed'))
        $report.captureStartedUtc=[DateTime]::UtcNow.ToString('o')
        $run=Invoke-Observer $exe $arguments ($DurationSeconds+20)
        $report.captureEndedUtc=[DateTime]::UtcNow.ToString('o')
        if ($run.exitCode -ne 0) { throw ('PresentMon failed (permission/ETW/CLI unknown), exit '+$run.exitCode+': '+$run.text.Substring(0,[Math]::Min(1200,$run.text.Length))) }
        if ((Get-Process -Id $TargetPid).StartTime.ToUniversalTime().ToString('o') -ne $start) { throw 'Target PID changed during capture.' }
        $csv=$OutputCsv
    }
    $report.analysis=Read-Timing $csv; $report.display=$report.analysis.display
    $report.status=$report.analysis.status; $report.reason=$report.analysis.reason
    if ($AnalyzeCsv -and $OutputCsv -ne $AnalyzeCsv) {
        if (Test-Path -LiteralPath $OutputCsv) { throw 'Output CSV exists: choose a fresh path.' }
        $null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($OutputCsv))
        Copy-Item -LiteralPath $AnalyzeCsv -Destination $OutputCsv
    }
} catch { $report.status='unknown';$report.reason=$_.Exception.Message;$report.display=Unknown $report.reason }
$report.finishedUtc=[DateTime]::UtcNow.ToString('o')
$null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($OutputJson))
$json=$report | ConvertTo-Json -Depth 16
[IO.File]::WriteAllText($OutputJson,$json,[Text.UTF8Encoding]::new($false))
$json

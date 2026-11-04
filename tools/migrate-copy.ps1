#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$src = 'D:\DATA\Documents\ChatGPT\DLSS F5 NEXT'
$dst = 'D:\CODE\DLSS5_NEXT'

$xd = @('obj','bin','out','.vs','Debug','Release','x64','compiler-temp','ipch','.git','__pycache__')
$xf = @('*.pch','*.ipch','*.tlog','*.iobj','*.obj','*.pdb','*.ilk','*.sbr','*.bsc')

function Copy-Tree([string]$from, [string]$to, [string[]]$exDirs, [string[]]$exFiles) {
    $args = @($from, $to, '/E', '/MT:64', '/NFL', '/NDL', '/NJH', '/NJS', '/R:1', '/W:1', '/XJ')
    foreach ($d in $exDirs) { $args += '/XD'; $args += $d }
    foreach ($f in $exFiles) { $args += '/XF'; $args += $f }
    $null = & robocopy @args
    $code = $LASTEXITCODE
    # robocopy: 0-7 are success-ish, >=8 is failure
    if ($code -ge 8) { throw "robocopy failed ($code) copying $from" }
    Write-Host ("copied [{0}] {1} -> {2}" -f $code, $from, $to)
}

Write-Host '=== 1/2 product source ==='
Copy-Tree (Join-Path $src 'OptiScaler') (Join-Path $dst 'OptiScaler') $xd $xf

Write-Host '=== 2/2 live external deps ==='
Copy-Tree (Join-Path $src 'work\full-source-f5-v11-candidate-20261018\external') (Join-Path $dst 'external') @('.git') @()

Write-Host '=== file counts ==='
foreach ($p in @('OptiScaler','external')) {
    $n = (Get-ChildItem -LiteralPath (Join-Path $dst $p) -Recurse -File -Force | Measure-Object).Count
    $mb = [math]::Round(((Get-ChildItem -LiteralPath (Join-Path $dst $p) -Recurse -File -Force | Measure-Object Length -Sum).Sum)/1MB,1)
    Write-Host ("{0}: {1} files, {2} MB" -f $p, $n, $mb)
}

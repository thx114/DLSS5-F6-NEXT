#requires -Version 7.0
<# Synthetic history-stage comparison, never a game FPS or total Reflex test. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$BaselineShader,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$CurrentShader = (Join-Path $PSScriptRoot '../../OptiScaler/shaders/dlssnr/precompile/dlssnr_detail_reuse_Shader.cso')
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$fixturePath=(Get-Item -LiteralPath $Executable).FullName
$currentPath=(Get-Item -LiteralPath $CurrentShader).FullName
$baselinePath=(Get-Item -LiteralPath $BaselineShader).FullName
$outputPath=[IO.Path]::GetFullPath($OutputDirectory)
$null=[IO.Directory]::CreateDirectory($outputPath)
$fixtureHash=(Get-FileHash -LiteralPath $fixturePath).Hash
$rows=@()
foreach($run in @(
    @{Name='current-a';Shader=$currentPath},
    @{Name='old';Shader=$baselinePath},
    @{Name='current-b';Shader=$currentPath}
)) {
    $shaderHash=(Get-FileHash -LiteralPath $run.Shader).Hash
    $started=[DateTime]::UtcNow
    $logPath=Join-Path $outputPath ($run.Name+'.log')
    $identity=[ordered]@{Executable=$fixturePath;ExecutableSha256=$fixtureHash;Shader=$run.Shader;ShaderSha256=$shaderHash;StartedUtc=$started.ToString('O')}
    $identity | ConvertTo-Json -Compress | Set-Content -LiteralPath $logPath -Encoding utf8NoBOM
    & $fixturePath --benchmark-history $run.Shader *>> $logPath
    $code=$LASTEXITCODE
    if($code -ne 0) {Get-Content -LiteralPath $logPath -Tail 20;throw "History-stage benchmark $($run.Name) failed: $code"}
    if((Get-FileHash -LiteralPath $fixturePath).Hash -ne $fixtureHash -or
       (Get-FileHash -LiteralPath $run.Shader).Hash -ne $shaderHash) {throw 'Comparison executable/shader changed during measurement'}
    $summary=Select-String -LiteralPath $logPath -Pattern '^BENCH history ' | Select-Object -ExpandProperty Line
    if(@($summary).Count -ne 1 -or !(Select-String -LiteralPath $logPath -SimpleMatch 'BENCH d3d12_debug_layer=true gpu_based_validation=false errors=0' -Quiet)) {
        throw "Incomplete benchmark/validation summary: $logPath"
    }
    $rows+= [pscustomobject]@{Name=$run.Name;Identity=$identity;EndedUtc=[DateTime]::UtcNow.ToString('O');Log=$logPath;LogSha256=(Get-FileHash -LiteralPath $logPath).Hash;Summary=$summary}
    Write-Output "$($run.Name): $summary"
}
[ordered]@{Schema='f6.synthetic-nr-history-stage.v1';Scope='same host and resources; synthetic flat inputs with normal expiry branches; excludes model/SR/FG and game';GameFpsConclusion=$null;Runs=$rows} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputPath 'identity.json') -Encoding utf8NoBOM

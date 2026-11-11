param([string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$dxc = Join-Path $PSScriptRoot '../../shader_tools/dxc.exe'
$generator = Join-Path $PSScriptRoot '../../shader_tools/create_header.py'
$shader = Join-Path $PSScriptRoot 'dlssnr_guide_preview.hlsl'
$binary = Join-Path $PSScriptRoot 'dlssnr_guide_preview_Shader.cso'
& $dxc -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect $shader -Fo $binary
if ($LASTEXITCODE -ne 0) { throw 'Guide preview DXIL compilation failed' }
& $Python $generator $binary (Join-Path $PSScriptRoot 'dlssnr_guide_preview_Shader.h') dlssnr_guide_preview_cso
if ($LASTEXITCODE -ne 0) { throw 'Guide preview DXIL header generation failed' }

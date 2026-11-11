param()
$ErrorActionPreference = 'Stop'
$dxc = Join-Path $PSScriptRoot '../../shader_tools/dxc.exe'
$generator = Join-Path $PSScriptRoot '../../shader_tools/create_header.py'
$python = 'python'
$shader = Join-Path $PSScriptRoot 'dlssnr_detail_reuse.hlsl'
$binary = Join-Path $PSScriptRoot 'dlssnr_detail_reuse_Shader.cso'
& $dxc -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect $shader -Fo $binary
if ($LASTEXITCODE -ne 0) { throw 'DXIL compilation failed' }
& $python $generator $binary (Join-Path $PSScriptRoot 'dlssnr_detail_reuse_Shader.h') dlssnr_detail_reuse_cso
if ($LASTEXITCODE -ne 0) { throw 'DXIL header generation failed' }
$spirv = Join-Path $PSScriptRoot 'dlssnr_detail_reuse_Shader_Vk.spv'
& $dxc -spirv -T cs_6_0 -E CSMain -O3 -Qstrip_debug -D VK_MODE -Cc -Vi $shader -Fo $spirv
if ($LASTEXITCODE -ne 0) { throw 'SPIR-V compilation failed' }
& $python $generator $spirv (Join-Path $PSScriptRoot 'dlssnr_detail_reuse_Shader_Vk.h') dlssnr_detail_reuse_spv
if ($LASTEXITCODE -ne 0) { throw 'SPIR-V header generation failed' }

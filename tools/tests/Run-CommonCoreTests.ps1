#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$DevCmd = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat',
    [string]$Python = 'D:/APPS/conda/python.exe',
    [string[]]$SuiteName = @(),
    [switch]$SkipSharedRegression
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$output = Join-Path $root 'build/common-core-tests'
$null = [IO.Directory]::CreateDirectory($output)
$sdk = Join-Path $root 'external/nvngx_dlss_sdk'
$streamline = Join-Path $root 'external/streamline'
$include = Join-Path $root 'OptiScaler/include'
$simpleini = Join-Path $root 'external/simpleini'
$env:VSLANG = '1033'
$suites = @(
    @{ Name = 'FrameOutputRate'; Source = 'OptiScaler/framegen/tests/FrameOutputRateTests.cpp' },
    @{ Name = 'FgRuntimeStatus'; Source = 'OptiScaler/framegen/tests/FgRuntimeStatusTests.cpp' },
    @{ Name = 'TaaSourceOutput'; Source = 'OptiScaler/inputs/taa/tests/TaaSourceOutputTests.cpp' },
    @{ Name = 'TaaAutomaticWarp'; Source = 'OptiScaler/inputs/taa/tests/TaaAutomaticWarpTests.cpp'; Libraries = 'd3d11.lib d3dcompiler.lib' },
    @{ Name = 'TaaProjectionJitter'; Source = 'OptiScaler/inputs/taa/tests/TaaProjectionJitterTests.cpp'; Libraries = 'd3d11.lib d3dcompiler.lib' },
    @{ Name = 'TaaShaderAnalysis'; Source = 'OptiScaler/inputs/taa/tests/TaaShaderAnalysisTests.cpp'; Libraries = 'd3dcompiler.lib' },
    @{ Name = 'TaaConsumerWarp'; Source = 'OptiScaler/inputs/taa/tests/TaaConsumerWarpTests.cpp'; Libraries = 'd3d11.lib d3dcompiler.lib' },
    @{ Name = 'TaaPassWarp'; Source = 'OptiScaler/inputs/taa/tests/TaaPassWarpTests.cpp'; Libraries = 'd3d11.lib d3dcompiler.lib' },
    @{ Name = 'FgHandoffWarp'; Source = 'OptiScaler/with_dx12/tests/fg-handoff/fixture.cpp'; StubInclude = 'build/common-core-tests'; Libraries = 'd3d11.lib d3d12.lib dxgi.lib' },
    @{ Name = 'BridgeRetirementWarp'; Source = 'OptiScaler/upscalers/tests/bridge-retirement/fixture.cpp'; StubInclude = 'build/common-core-tests'; Libraries = 'd3d12.lib dxgi.lib' },
    @{ Name = 'ReflexQueueTiming'; Source = 'OptiScaler/runtime/tests/ReflexQueueTimingTests.cpp' },
    @{ Name = 'BeforeSrInput'; Source = 'OptiScaler/inputs/tests/BeforeSrInputTests.cpp'; Libraries = 'd3d11.lib' },
    @{ Name = 'ReflexReportPolicy'; Source = 'OptiScaler/runtime/tests/ReflexReportPolicyTests.cpp'; StubInclude = 'external/nvapi' },
    @{ Name = 'ReflexForwarding'; Source = 'OptiScaler/hooks/tests/ReflexForwardingTests.cpp' },
    @{ Name = 'FrameLimitTimer'; Source = 'OptiScaler/runtime/tests/FrameLimitTimerTests.cpp' },
    @{ Name = 'NrVitSplit'; Source = 'OptiScaler/dlssnr/tests/nr_vit_split.cpp' },
    @{ Name = 'DlssPresetMenu'; Source = 'OptiScaler/menu/tests/DlssPresetMenuTests.cpp' },
    @{ Name = 'LookControls'; Source = 'OptiScaler/menu/tests/look-controls/fixture.cpp'; StubInclude = 'build/common-core-tests' },
    @{ Name = 'NrColourTransaction'; Source = 'OptiScaler/shaders/dlssnr/tests/colour-transaction/fixture.cpp'; StubInclude = 'build/common-core-tests' },
    @{ Name = 'NrColourEncoding'; Source = 'OptiScaler/shaders/dlssnr/tests/NrColourEncodingTests.cpp' },
    @{ Name = 'NrDeferredContract'; Source = 'OptiScaler/shaders/dlssnr/tests/deferred-contract/fixture.cpp'; StubInclude = 'build/common-core-tests' },
    @{ Name = 'NrPresentationSource'; Source = 'OptiScaler/runtime/tests/NrPresentationSourceTests.cpp' },
    @{ Name = 'NrPresentationOutputWarp'; Source = 'OptiScaler/runtime/tests/NrPresentationOutputWarpTests.cpp'; Libraries = 'd3d12.lib dxgi.lib' },
    @{ Name = 'SwapchainHookBoundary'; Source = 'OptiScaler/wrapped/tests/SwapchainHookBoundaryTests.cpp' },
    @{ Name = 'ProviderOutputLifecycle'; Source = 'OptiScaler/wrapped/tests/ProviderOutputLifecycleTests.cpp'; StubInclude = 'build/common-core-tests'; Libraries = 'd3d12.lib dxgi.lib user32.lib' },
    @{ Name = 'NrEvaluateGuides'; Source = 'OptiScaler/runtime/tests/NrEvaluateGuidesTests.cpp'; Libraries = 'd3d12.lib dxgi.lib' },
    @{ Name = 'NrDetailHistoryPolicy'; Source = 'OptiScaler/dlssnr/tests/nr_detail_history_policy.cpp' },
    @{ Name = 'NrHistoryDebug'; Source = 'OptiScaler/runtime/tests/NrHistoryDebugTests.cpp' },
    @{ Name = 'NrDetailHistoryWarp'; Source = 'OptiScaler/runtime/tests/NrDetailHistoryWarpTests.cpp'; Libraries = 'd3d11.lib d3dcompiler.lib' },
    @{ Name = 'NrDetailHistoryRuntime'; Source = 'OptiScaler/shaders/dlssnr/tests/detail-history-runtime/fixture.cpp'; AdditionalSource = 'OptiScaler/shaders/dlssnr/NrGpuExecutionLifetime.cpp'; CompilerFlags = '/DDLSSNR_LIFETIME_TEST_SEAM'; StubInclude = 'OptiScaler/shaders/dlssnr/tests/detail-history-runtime/stubs'; Libraries = 'd3d12.lib dxgi.lib' },
    @{ Name = 'NrGpuExecutionLifetime'; Source = 'OptiScaler/runtime/tests/NrGpuExecutionLifetimeTests.cpp'; AdditionalSource = 'OptiScaler/shaders/dlssnr/NrGpuExecutionLifetime.cpp' },
    @{ Name = 'NrLayerLookWarp'; Source = 'OptiScaler/shaders/dlssnr/tests/LookWarpTests.cpp'; StubInclude = 'build/common-core-tests'; Libraries = 'd3d11.lib d3dcompiler.lib' },
    @{ Name = 'NrGpuTimeWarp'; Source = 'OptiScaler/shaders/dlssnr/tests/gpu-time/fixture.cpp'; StubInclude = 'OptiScaler/shaders/dlssnr/tests/gpu-time'; Libraries = 'd3d12.lib dxgi.lib' },
    @{ Name = 'GuideMetadata'; Source = 'OptiScaler/dlssnr/tests/GuideMetadata/GuideMetadataTests.cpp' },
    @{ Name = 'GuideObservationWarp'; Source = 'OptiScaler/dlssnr/tests/GuideMetadata/GuideObservationWarpTests.cpp'; AdditionalSource = 'OptiScaler/runtime/GuideCapture.cpp'; Libraries = 'd3d12.lib dxgi.lib d3dcompiler.lib' },
    @{ Name = 'GuideDiagnostics'; Source = 'OptiScaler/runtime/tests/GuideDiagnosticsTests.cpp' },
    @{ Name = 'GuidePairExport'; Source = 'OptiScaler/runtime/tests/GuidePairExportTests.cpp' },
    @{ Name = 'DlssgPacingLifetime'; Source = 'OptiScaler/runtime/tests/DlssgPacingLifetimeTests.cpp' },
    @{ Name = 'NrHistorySurfaceWarp'; Source = 'OptiScaler/runtime/tests/NrHistorySurfaceWarpTests.cpp'; Libraries = 'd3d11.lib d3dcompiler.lib' },
    @{ Name = 'NrWatchedLists'; Source = 'OptiScaler/runtime/tests/NrWatchedListsTests.cpp' },
    @{ Name = 'GuideSourceMetadata'; Source = 'OptiScaler/runtime/tests/GuideSourceMetadataTests.cpp' },
    @{ Name = 'GuideCalibration'; Source = 'OptiScaler/runtime/tests/GuideCalibrationTests.cpp' },
    @{ Name = 'FgColourJoin'; Source = 'OptiScaler/framegen/dlssg/tests/FgColourJoinTests.cpp' },
    @{ Name = 'GuideCaptureWarp'; Source = 'OptiScaler/runtime/tests/GuideCaptureWarpTests.cpp'; AdditionalSource = 'OptiScaler/runtime/GuideCapture.cpp'; Libraries = 'd3d12.lib dxgi.lib d3dcompiler.lib'; StubInclude = 'build/common-core-tests' },
    @{ Name = 'GuidePreviewWarp'; Source = 'OptiScaler/runtime/tests/GuidePreviewWarpTests.cpp'; Libraries = 'd3d12.lib dxgi.lib d3dcompiler.lib'; StubInclude = 'build/common-core-tests' },
    @{ Name = 'MenuPreviewOutput'; Source = 'OptiScaler/menu/tests/PreviewOutputCacheWarpTests.cpp'; Libraries = 'd3d12.lib dxgi.lib d3dcompiler.lib'; StubInclude = 'build/common-core-tests' },
    @{ Name = 'GuideOrientation'; Source = 'OptiScaler/framegen/tests/GuideOrientationTests.cpp'; StubInclude = 'build/common-core-tests' },
    @{ Name = 'ResourceFlipWarp'; Source = 'OptiScaler/shaders/resource_flip/tests/RFWarp.cpp'; Libraries = 'd3d12.lib dxgi.lib d3dcompiler.lib' },
    @{ Name = 'F6InputPolicy'; Source = 'OptiScaler/runtime/tests/F6InputPolicyTests.cpp' },
    @{ Name = 'PrivateGraphicsScope'; Source = 'OptiScaler/runtime/tests/PrivateGraphicsScopeTests.cpp'; AdditionalSource = 'OptiScaler/runtime/tests/PrivateGraphicsScopeOther.cpp' },
    @{ Name = 'HookOwnership'; Source = 'OptiScaler/runtime/tests/HookOwnershipTests.cpp'; Libraries = ('"{0}"' -f (Join-Path $root 'OptiScaler/library/detours/detours.lib')) },
    @{ Name = 'HoYoShadeEffectRequirements'; Source = 'OptiScaler/runtime/tests/HoYoShadeEffectRequirementsTests.cpp' },
    @{ Name = 'MethodHooks'; Source = 'OptiScaler/runtime/tests/MethodHooksTests.cpp'; Libraries = ('"{0}"' -f (Join-Path $root 'OptiScaler/library/detours/detours.lib')) },
    @{ Name = 'D3D11On12FactoryBoundary'; Source = 'OptiScaler/runtime/tests/D3D11On12FactoryBoundaryTests.cpp'; Libraries = ('"{0}"' -f (Join-Path $root 'OptiScaler/library/detours/detours.lib')) },
    @{ Name = 'ProviderOutputCreation'; Source = 'OptiScaler/runtime/tests/ProviderOutputCreationTests.cpp'; Libraries = ('"{0}"' -f (Join-Path $root 'OptiScaler/library/detours/detours.lib')) },
    @{ Name = 'SourcePresent'; Source = 'OptiScaler/runtime/tests/SourcePresentTests.cpp' },
    @{ Name = 'FeatureTransition'; Source = 'OptiScaler/upscalers/tests/FeatureTransitionTests.cpp' },
    @{ Name = 'Dx11BackendBinding'; Source = 'OptiScaler/upscalers/tests/backend-binding/fixture.cpp'; StubInclude = 'build/common-core-tests'; Libraries = 'd3d11.lib' },
    @{ Name = 'NgxDx12ResourceView'; Source = 'OptiScaler/upscalers/tests/NgxDx12ResourceViewTests.cpp' },
    @{ Name = 'Dx11OutputCopy'; Source = 'OptiScaler/with_dx12/tests/output-copy/fixture.cpp'; StubInclude = 'build/common-core-tests' },
    @{ Name = 'InteropSwapchainPolicy'; Source = 'OptiScaler/with_dx12/tests/InteropSwapchainPolicyTests.cpp' },
    @{ Name = 'InteropSwapchainWarp'; Source = 'OptiScaler/with_dx12/tests/InteropSwapchainWarpTests.cpp'; Libraries = 'd3d11.lib d3d12.lib dxgi.lib user32.lib' },
    @{ Name = 'ProducerProgress'; Source = 'OptiScaler/runtime/tests/ProducerProgressTests.cpp' },
    @{ Name = 'NativeProducerCpuTiming'; Source = 'OptiScaler/runtime/tests/NativeProducerCpuTimingTests.cpp' },
    @{ Name = 'NativePrepGpuTiming'; Source = 'OptiScaler/runtime/tests/NativePrepGpuTimingTests.cpp'; Libraries = 'd3d12.lib dxgi.lib' },
    @{ Name = 'StarRailResidencyPolicy'; Source = 'OptiScaler/upscalers/tests/StarRailResidencyPolicyTests.cpp' },
    @{ Name = 'NrResident'; Source = 'OptiScaler/runtime/tests/NrResidentTests.cpp' },
    @{ Name = 'NrResidentConsumer'; Source = 'OptiScaler/runtime/tests/NrResidentConsumerTests.cpp'; StubInclude = 'OptiScaler/runtime/tests/nr-resident/stubs' },
    @{ Name = 'FenceCompletion'; Source = 'OptiScaler/runtime/tests/FenceCompletionTests.cpp' },
    @{ Name = 'CpuMappingTransactions'; Source = 'OptiScaler/runtime/tests/CpuMappingTransactionsTests.cpp' },
    @{ Name = 'CpuChangedByteRanges'; Source = 'OptiScaler/runtime/tests/CpuChangedByteRangesTests.cpp' },
    @{ Name = 'CpuIndexBounds'; Source = 'OptiScaler/runtime/tests/CpuIndexBoundsTests.cpp' },
    @{ Name = 'HookCpuTiming'; Source = 'OptiScaler/runtime/tests/HookCpuTimingTests.cpp'; AdditionalSource = 'OptiScaler/runtime/HookCpuTiming.cpp' },
    @{ Name = 'HookTimingPublication'; Source = 'OptiScaler/runtime/tests/HookTimingPublicationTests.cpp'; AdditionalSource = 'OptiScaler/runtime/HookCpuTiming.cpp' },
    @{ Name = 'Dx11SharedRead'; Source = 'OptiScaler/runtime/tests/Dx11SharedReadTests.cpp'; Libraries = 'd3d11.lib d3d12.lib dxgi.lib' },
    @{ Name = 'Dx11RasterBindings'; Source = 'OptiScaler/runtime/tests/Dx11RasterBindingsTests.cpp'; Libraries = 'd3d11.lib d3dcompiler.lib' },
    @{ Name = 'DlssgTelemetry'; Source = 'OptiScaler/runtime/tests/DlssgTelemetryTests.cpp' },
    @{ Name = 'LoaderStartupContext'; Source = 'OptiScaler/runtime/tests/LoaderStartupContextTests.cpp' },
    @{ Name = 'PluginStartupGate'; Source = 'OptiScaler/runtime/tests/PluginStartupGateTests.cpp' },
    @{ Name = 'UpscalerFrameParameters'; Source = 'OptiScaler/inputs/FG/tests/UpscalerFrameParametersTests.cpp' },
    @{ Name = 'SceneGuideContract'; Source = 'OptiScaler/dlssnr/tests/nr_scene_guide_contract.cpp' },
    @{ Name = 'NrDetailContinuity'; Source = 'OptiScaler/dlssnr/tests/nr_detail_history_continuity.cpp' },
    @{ Name = 'NrNativeDetailHost'; Source = 'OptiScaler/dlssnr/tests/nr_native_detail_host.cpp'; StubInclude = 'OptiScaler/runtime/tests/nr-resident/stubs' },
    @{ Name = 'NrModelChainHistory'; Source = 'OptiScaler/dlssnr/tests/nr_model_chain_history.cpp' },
    @{ Name = 'NrDepthOrientation'; Source = 'OptiScaler/dlssnr/tests/nr_depth_orientation.cpp' },
    @{ Name = 'NrReconfiguration'; Source = 'OptiScaler/dlssnr/tests/nr-reconfiguration/fixture.cpp' },
    @{ Name = 'DlssgOptionsPolicy'; Source = 'OptiScaler/framegen/dlssg/tests/DlssgOptionsPolicyTests.cpp' },
    @{ Name = 'HookCpuProcessOverride'; Source = 'OptiScaler/runtime/tests/hook_cpu_process_override.cpp'; ProcessEnvironment = $true }
)

$SuiteName = @($SuiteName | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($SuiteName.Count) {
    $unknown = @($SuiteName | Where-Object { $_ -notin $suites.Name })
    if ($unknown.Count) { throw "Unknown CPU suite: $($unknown -join ', ')" }
    $suites = @($suites | Where-Object { $_.Name -in $SuiteName })
}

if ('Dx11BackendBinding' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/upscalers/tests/backend-binding/prepare.py')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract actual DX11 backend/source binding' }
}
if ('FgHandoffWarp' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/with_dx12/tests/fg-handoff/prepare.py')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract FG queue/guide handoff implementation' }
    & $Python -B (Join-Path $root 'OptiScaler/with_dx12/tests/fg-handoff/check_wiring.py')
    if ($LASTEXITCODE -ne 0) { throw 'FG queue/guide handoff wiring failed' }
}
if ('BridgeRetirementWarp' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/upscalers/tests/bridge-retirement/prepare.py')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract bridge recording retirement implementation' }
}
if ('Dx11OutputCopy' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/with_dx12/tests/output-copy/prepare.py')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract current DX11 output copy implementation' }
}
if ('NrReconfiguration' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/dlssnr/tests/nr-reconfiguration/prepare.py') --emit
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract current NR reconfiguration implementation' }
}
if ('NrDeferredContract' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/shaders/dlssnr/tests/deferred-contract/prepare.py') (Join-Path $root 'OptiScaler/shaders/dlssnr/DlssNr_DeferredSr.inl') $output
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract current deferred colour transaction' }
}
if ('NrColourTransaction' -in $suites.Name -or 'NrLayerLookWarp' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/shaders/dlssnr/tests/colour-transaction/prepare.py')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract current colour transaction implementation' }
}
if ('GuideCaptureWarp' -in $suites.Name -or 'GuidePreviewWarp' -in $suites.Name -or 'MenuPreviewOutput' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/runtime/tests/prepare_guide_capture_hooks.py') (Join-Path $output 'GuideCaptureHooksProduction.inl')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract current guide capture submission wrappers' }
}

if ('MenuPreviewOutput' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/menu/tests/test_preview_output_wiring.py')
    if ($LASTEXITCODE -ne 0) { throw 'Menu preview output wiring failed' }
}

if ('ProviderOutputLifecycle' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/wrapped/tests/prepare_provider_output_lifecycle.py') (Join-Path $output 'ProviderOutputLifecycleProduction.inl')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract physical output lifecycle' }
}

if ('LookControls' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/menu/tests/look-controls/prepare.py')
    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare production Look controls fixture' }
}

if ('ResourceFlipWarp' -in $suites.Name) {
    & (Join-Path $root 'OptiScaler/shaders/resource_flip/Generate-RFShader.ps1') -CheckOnly
}

if (@($suites | Where-Object Name -eq 'GuideOrientation').Count) {
    $configSource = [IO.File]::ReadAllText((Join-Path $root 'OptiScaler/Config.h'))
    $begin = $configSource.IndexOf('enum HasDefaultValue')
    $end = $configSource.IndexOf('constexpr inline int UnboundKey')
    if ($begin -lt 0 -or $end -le $begin) { throw 'Cannot extract current CustomOptional' }
    [IO.File]::WriteAllText((Join-Path $output 'GuideOrientationConfigBase.h'), $configSource.Substring($begin, $end-$begin))
}

foreach ($suite in $suites) {
    $source = Join-Path $root $suite.Source
    if (!(Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing CPU suite: $source" }
    $exe = Join-Path $output ($suite.Name + '.exe')
    $suiteOutput = Join-Path $output $suite.Name
    $null = [IO.Directory]::CreateDirectory($suiteOutput)
    $obj = $suiteOutput + '/'
    $sources = '"' + $source + '"'
    if ($suite.ContainsKey('AdditionalSource')) { $sources += ' "' + (Join-Path $root $suite.AdditionalSource) + '"' }
    # All arguments are local paths constructed above. One compiler owner runs
    # the suites in sequence. Graphics fixtures use real software WARP devices;
    # remaining suites are CPU-only. No SDK entry point or game is executed.
    $command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{1}" /I"{2}" /I"{6}" /I"{7}" /Fe:"{3}" /Fo:"{4}" {5}' -f $DevCmd,$sdk,$streamline,$exe,$obj,$sources,$include,$simpleini
    $command = $command.Replace('/nologo /std:c++20', ('/nologo /I"{0}" /utf-8 /std:c++20' -f (Join-Path $root 'OptiScaler')))
    if ($suite.ContainsKey('StubInclude')) {
        $stub = Join-Path $root $suite.StubInclude
        $command = $command.Replace('cl /nologo', ('cl /I"{0}" /nologo' -f $stub))
    }
    if ($suite.ContainsKey('CompilerFlags')) { $command += ' ' + $suite.CompilerFlags }
    if ($suite.ContainsKey('Libraries')) { $command += ' /link ' + $suite.Libraries }
    & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw "CPU suite $($suite.Name) failed with exit $LASTEXITCODE" }
    if ($suite.ContainsKey('ProcessEnvironment')) {
        $previous = [Environment]::GetEnvironmentVariable('OPTISCALER_HOOK_CPU_TIMING', 'Process')
        try {
            foreach ($value in @($null, '0', '1', '10', 'true', '1 ', 'x', '12345')) {
                [Environment]::SetEnvironmentVariable('OPTISCALER_HOOK_CPU_TIMING', $value, 'Process')
                $expected = if ($value -ceq '1') { '1' } else { '0' }
                & $exe $expected
                if ($LASTEXITCODE -ne 0) { throw "Process override case failed: $value" }
            }
        } finally { [Environment]::SetEnvironmentVariable('OPTISCALER_HOOK_CPU_TIMING', $previous, 'Process') }
        Write-Output 'HookCpuProcessOverride: 8 fresh-process cases passed'
    } elseif ($suite.Name -eq 'GuidePairExport') {
        $fixture=Join-Path $suiteOutput 'pair-fixture.json'
        & $exe --write-fixture $fixture
        if ($LASTEXITCODE -ne 0) { throw 'Guide pair cross-language fixture failed' }
        $previousFixture=[Environment]::GetEnvironmentVariable('F6_GUIDE_PAIR_FIXTURE','Process')
        try {
            $env:F6_GUIDE_PAIR_FIXTURE=$fixture
            & $Python -B (Join-Path $root 'tools/tests/test_f6_guide_pair.py')
            if ($LASTEXITCODE -ne 0) { throw 'Guide pair cross-language parser checks failed' }
        } finally {
            [Environment]::SetEnvironmentVariable('F6_GUIDE_PAIR_FIXTURE',$previousFixture,'Process')
        }
    } elseif ($suite.Name -eq 'NrDetailHistoryRuntime') {
        & (Join-Path $root 'OptiScaler/shaders/dlssnr/tests/detail-history-runtime/Run.ps1') -Executable $exe
        if ($LASTEXITCODE -ne 0) { throw 'NR history runtime lifetime checks failed' }
    } else {
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "CPU suite $($suite.Name) failed with exit $LASTEXITCODE" }
        if ($suite.Name -eq 'NrDetailHistoryWarp') {
            # Reintroduce the rolling-reference defect only in an ignored shader
            # copy. The production-pixel test must detect the original failure.
            $negativeRoot = Join-Path $suiteOutput 'rolling-reference-negative'
            $null = [IO.Directory]::CreateDirectory($negativeRoot)
            $shaderRoot = Join-Path $root 'OptiScaler/shaders/dlssnr/precompile'
            foreach ($file in @('dlssnr_detail_reuse.hlsl','dlssnr_detail_history.hlsli','dlssnr_replace_curve.hlsli')) {
                Copy-Item -LiteralPath (Join-Path $shaderRoot $file) -Destination (Join-Path $negativeRoot $file) -Force
            }
            $historyPath = Join-Path $negativeRoot 'dlssnr_detail_history.hlsli'
            $historySource = [IO.File]::ReadAllText($historyPath)
            $original = 'result.context = float4(transported ? birth : proxy.rgb, z);'
            if (!$historySource.Contains($original)) { throw 'History birth-reference negative no longer matches production' }
            [IO.File]::WriteAllText($historyPath,$historySource.Replace($original,'result.context = float4(proxy.rgb, z);'))
            $negativeLog = Join-Path $negativeRoot 'result.log'
            & $exe --shader (Join-Path $negativeRoot 'dlssnr_detail_reuse.hlsl') *> $negativeLog
            if ($LASTEXITCODE -ne 1 -or
                !(Select-String -LiteralPath $negativeLog -SimpleMatch 'FAIL: gradual appearance drift cannot carry an unrelated old edit' -Quiet)) {
                throw "History birth-reference negative did not detect the original defect; inspect $negativeLog"
            }
            Write-Output 'NR history rolling-reference mutation: original drift defect detected as expected'
            # Independently restore the short filter's birth-colour read. The
            # three-frame test must fail even while long ancestry stays correct.
            $adjacentRoot = Join-Path $suiteOutput 'adjacent-raw-negative'
            $null = [IO.Directory]::CreateDirectory($adjacentRoot)
            foreach ($file in @('dlssnr_detail_reuse.hlsl','dlssnr_detail_history.hlsli','dlssnr_replace_curve.hlsli')) {
                Copy-Item -LiteralPath (Join-Path $shaderRoot $file) -Destination (Join-Path $adjacentRoot $file) -Force
            }
            $adjacentPath = Join-Path $adjacentRoot 'dlssnr_detail_history.hlsli'
            $adjacentSource = [IO.File]::ReadAllText($adjacentPath)
            $adjacentRead = 'const float3 adjacentRaw = t7.Load(int3(q, 0)).rgb;'
            if (!$adjacentSource.Contains($adjacentRead)) { throw 'Adjacent-raw negative no longer matches production' }
            [IO.File]::WriteAllText($adjacentPath, $adjacentSource.Replace($adjacentRead, 'const float3 adjacentRaw = context.rgb;'))
            $adjacentLog = Join-Path $adjacentRoot 'result.log'
            & $exe --shader (Join-Path $adjacentRoot 'dlssnr_detail_reuse.hlsl') *> $adjacentLog
            if ($LASTEXITCODE -ne 1 -or
                !(Select-String -LiteralPath $adjacentLog -SimpleMatch 'FAIL: short stability follows adjacent raw even when the long birth differs' -Quiet) -or
                !(Select-String -LiteralPath $adjacentLog -SimpleMatch 'FAIL: return to birth colour does not hide an adjacent-frame appearance change' -Quiet)) {
                throw "Adjacent-raw negative did not detect the original three-frame defect; inspect $adjacentLog"
            }
            Write-Output 'NR history adjacent-raw mutation: both three-frame regressions detected as expected'
        }
        if ($suite.Name -eq 'GuidePreviewWarp') {
            & $exe --producer-replay
            if ($LASTEXITCODE -ne 0) { throw 'Preview first-use producer replay failed' }
        }
        if ($suite.Name -eq 'FgHandoffWarp') {
            foreach ($negative in @('--negative-queue', '--negative-guides', '--negative-handback', '--negative-input-handback')) {
                $negativeLog = Join-Path $suiteOutput ($negative.Substring(2) + '.log')
                & $exe $negative *> $negativeLog
                $expected = if ($negative -eq '--negative-queue') { 'SDK physical factory cannot overwrite source queue' } else { 'GPU snapshot uses the matching source frame at every pixel' }
                if ($LASTEXITCODE -ne 17 -or !(Select-String -LiteralPath $negativeLog -SimpleMatch $expected -Quiet)) {
                    throw "FG handoff negative escaped or failed for an unrelated reason: $negative; inspect $negativeLog"
                }
            }
            Write-Output 'FG handoff: old queue publication, delayed guide read and missing writer wait detected as expected'
        }
        if ($suite.Name -eq 'BridgeRetirementWarp') {
            $negativeLog = Join-Path $suiteOutput 'baseline.log'
            & $exe --baseline *> $negativeLog
            if ($LASTEXITCODE -ne 1 -or
                !(Select-String -LiteralPath $negativeLog -SimpleMatch 'released SR feature must close NR recording before model rebuild' -Quiet)) {
                throw "Bridge old-release baseline escaped; inspect $negativeLog"
            }
            Write-Output 'Bridge retirement: old release flow reproduced the pinned NR generation'
        }
        if ($suite.Name -eq 'NrReconfiguration') {
            foreach ($negative in @('--negative-gpu', '--negative-pending', '--negative-reset')) {
                & $exe $negative
                if ($LASTEXITCODE -ne 1) { throw "NR retirement negative control escaped: $negative" }
            }
            Write-Output 'NR reconfiguration: three unsafe mutation controls failed as expected'
        }
    }
}

if ($SkipSharedRegression) {
    Write-Output 'Selected suites passed; shared regression explicitly skipped.'
    exit 0
}

& $Python -B (Join-Path $root 'OptiScaler/inputs/taa/tests/test_hook_writeback.py')
if ($LASTEXITCODE -ne 0) { throw 'TAA Map/Unmap staging writeback checks failed' }
& $Python -B (Join-Path $root 'OptiScaler/inputs/taa/tests/test_hook_writeback.py') --negative-old-order
if ($LASTEXITCODE -ne 0) { throw 'TAA old staging writeback order negative control escaped' }

& $Python -B (Join-Path $root 'OptiScaler/inputs/FG/tests/test_upscaler_frame_wiring.py')
if ($LASTEXITCODE -ne 0) { throw "Input wiring checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/framegen/dlssg/tests/GuideFgWiringTests.py')
if ($LASTEXITCODE -ne 0) { throw "FG guide observation checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/framegen/dlssg/tests/test_fg_preview_admission.py')
if ($LASTEXITCODE -ne 0) { throw "FG preview admission checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/framegen/dlssg/tests/check-zzz-frame-wiring.py')
if ($LASTEXITCODE -ne 0) { throw "ZZZ production wiring checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/framegen/tests/test_guide_orientation_wiring.py')
if ($LASTEXITCODE -ne 0) { throw "FG orientation wiring checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/with_dx12/tests/test_interop_swapchain_policy.py')
if ($LASTEXITCODE -ne 0) { throw "Interop swapchain wiring checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/framegen/dlssg/tests/check-dlssg-options-wiring.py')
if ($LASTEXITCODE -ne 0) { throw "FG option wiring checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/dlssnr/tests/test_direct_dlss_scene_wiring.py')
if ($LASTEXITCODE -ne 0) { throw "Direct DLSS scene wiring checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/dlssnr/tests/check_detail_history_continuity.py')
if ($LASTEXITCODE -ne 0) { throw "NR detail history continuity checks failed with exit $LASTEXITCODE" }



& $Python -B (Join-Path $root 'OptiScaler/hooks/tests/test_fg_sdk_present_timing.py') --cpu
if ($LASTEXITCODE -ne 0) { throw "SDK Present observation checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/hooks/tests/test_fg_source_scheduling.py') --cpu
if ($LASTEXITCODE -ne 0) { throw "FG source pacing/query/latency checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/hooks/tests/test_dlssg_state_observation.py')
if ($LASTEXITCODE -ne 0) { throw "DLSSG state observation ABI/LastError checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/framegen/tests/test_runtime_output_observation.py')
if ($LASTEXITCODE -ne 0) { throw "FG runtime output observation checks failed with exit $LASTEXITCODE" }

& (Join-Path $root 'tools/tests/Test-F6DisplayTiming.ps1')
if ($LASTEXITCODE -ne 0) { throw "Display event parser checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/hooks/tests/test_fg_reflex_ownership.py')
if ($LASTEXITCODE -ne 0) { throw "FG Reflex source/output ownership checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/hooks/tests/test_starrail_gap_suppression.py')
if ($LASTEXITCODE -ne 0) { throw 'StarRail production gap suppression checks failed' }
$suppressionNegativeLog = Join-Path $output 'StarRailGapSuppression/negative-no-cleanup.log'
& $Python -B (Join-Path $root 'OptiScaler/hooks/tests/test_starrail_gap_suppression.py') --negative-no-cleanup *> $suppressionNegativeLog
if ($LASTEXITCODE -ne 17 -or !(Select-String -LiteralPath $suppressionNegativeLog -SimpleMatch 'failure/exception revokes both configuration ACKs' -Quiet)) {
    throw "StarRail suppression cleanup negative escaped or failed for an unrelated reason; inspect $suppressionNegativeLog"
}

& $Python -B (Join-Path $root 'OptiScaler/with_dx12/tests/test_interop_copy_retirement.py') --cpu
if ($LASTEXITCODE -ne 0) { throw "Interop copy reader retirement checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/with_dx12/tests/run_interop_reader_warp.py') --output (Join-Path $output 'InteropReaderWarp')
if ($LASTEXITCODE -ne 0) { throw "Interop shared reader WARP checks failed with exit $LASTEXITCODE" }





& $Python -B (Join-Path $root 'OptiScaler/runtime/tests/test_observed_settings.py')
if ($LASTEXITCODE -ne 0) { throw "Observed settings checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/upscalers/tests/test_dx11_release_retirement.py')
if ($LASTEXITCODE -ne 0) { throw 'DX11 release retirement checks failed' }
$releaseNegativeLog = Join-Path $output 'Dx11ReleaseRetirement/negative.log'
& $Python -B (Join-Path $root 'OptiScaler/upscalers/tests/test_dx11_release_retirement.py') --negative-skip-retirement *> $releaseNegativeLog
if ($LASTEXITCODE -ne 1 -or !(Select-String -LiteralPath $releaseNegativeLog -SimpleMatch 'FAIL 5:' -Quiet)) {
    throw "DX11 release retirement negative escaped or failed for an unrelated reason; inspect $releaseNegativeLog"
}
# The negative process has the expected failure code; do not export it as the
# result of the otherwise successful shared regression script.
$global:LASTEXITCODE = 0

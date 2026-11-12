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
    @{ Name = 'FgHandoffWarp'; Source = 'OptiScaler/with_dx12/tests/fg-handoff/fixture.cpp'; StubInclude = 'build/common-core-tests'; Libraries = 'd3d11.lib d3d12.lib dxgi.lib' },
    @{ Name = 'BridgeRetirementWarp'; Source = 'OptiScaler/upscalers/tests/bridge-retirement/fixture.cpp'; StubInclude = 'build/common-core-tests'; Libraries = 'd3d12.lib dxgi.lib' },
    @{ Name = 'ReflexQueueTiming'; Source = 'OptiScaler/runtime/tests/ReflexQueueTimingTests.cpp' },
    @{ Name = 'DlssPresetMenu'; Source = 'OptiScaler/menu/tests/DlssPresetMenuTests.cpp' },
    @{ Name = 'LookControls'; Source = 'OptiScaler/menu/tests/look-controls/fixture.cpp'; StubInclude = 'build/common-core-tests' },
    @{ Name = 'NrColourTransaction'; Source = 'OptiScaler/shaders/dlssnr/tests/colour-transaction/fixture.cpp'; StubInclude = 'build/common-core-tests' },
    @{ Name = 'NrPresentationSource'; Source = 'OptiScaler/runtime/tests/NrPresentationSourceTests.cpp' },
    @{ Name = 'NrPresentationOutputWarp'; Source = 'OptiScaler/runtime/tests/NrPresentationOutputWarpTests.cpp'; Libraries = 'd3d12.lib dxgi.lib' },
    @{ Name = 'SwapchainHookBoundary'; Source = 'OptiScaler/wrapped/tests/SwapchainHookBoundaryTests.cpp' },
    @{ Name = 'NrEvaluateGuides'; Source = 'OptiScaler/runtime/tests/NrEvaluateGuidesTests.cpp'; Libraries = 'd3d12.lib dxgi.lib' },
    @{ Name = 'NrDetailHistoryPolicy'; Source = 'OptiScaler/dlssnr/tests/nr_detail_history_policy.cpp' },
    @{ Name = 'NrHistoryDebug'; Source = 'OptiScaler/runtime/tests/NrHistoryDebugTests.cpp' },
    @{ Name = 'NrDetailHistoryWarp'; Source = 'OptiScaler/runtime/tests/NrDetailHistoryWarpTests.cpp'; Libraries = 'd3d11.lib d3dcompiler.lib' },
    @{ Name = 'NrDetailHistoryRuntime'; Source = 'OptiScaler/shaders/dlssnr/tests/detail-history-runtime/fixture.cpp'; StubInclude = 'OptiScaler/shaders/dlssnr/tests/detail-history-runtime/stubs'; Libraries = 'd3d12.lib dxgi.lib' },
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
    @{ Name = 'SourcePresent'; Source = 'OptiScaler/runtime/tests/SourcePresentTests.cpp' },
    @{ Name = 'FeatureTransition'; Source = 'OptiScaler/upscalers/tests/FeatureTransitionTests.cpp' },
    @{ Name = 'NgxDx12ResourceView'; Source = 'OptiScaler/upscalers/tests/NgxDx12ResourceViewTests.cpp' },
    @{ Name = 'InteropSwapchainPolicy'; Source = 'OptiScaler/with_dx12/tests/InteropSwapchainPolicyTests.cpp' },
    @{ Name = 'ProducerProgress'; Source = 'OptiScaler/runtime/tests/ProducerProgressTests.cpp' },
    @{ Name = 'NativeProducerCpuTiming'; Source = 'OptiScaler/runtime/tests/NativeProducerCpuTimingTests.cpp' },
    @{ Name = 'NativePrepGpuTiming'; Source = 'OptiScaler/runtime/tests/NativePrepGpuTimingTests.cpp'; Libraries = 'd3d12.lib dxgi.lib' },
    @{ Name = 'DrawAdmission'; Source = 'OptiScaler/genshin/tests/DrawAdmissionTests.cpp' },
    @{ Name = 'FsrComposite'; Source = 'OptiScaler/genshin/tests/FsrCompositeTests.cpp'; Libraries = 'd3d11.lib d3dcompiler.lib' },
    @{ Name = 'FsrPassObservation'; Source = 'OptiScaler/genshin/tests/FsrPassObservationTests.cpp'; Libraries = 'd3d11.lib' },
    @{ Name = 'ShaderCurrent'; Source = 'OptiScaler/genshin/tests/shader-current/fixture.cpp'; AdditionalSource = 'OptiScaler/genshin/GenshinHookCpuTiming.cpp'; Libraries = ('d3d11.lib d3dcompiler.lib /LIBPATH:"{0}"' -f (Join-Path $root 'OptiScaler/library/detours')) },
    @{ Name = 'FsrHookHudMetrics'; Source = 'OptiScaler/runtime/tests/FsrHookHudMetricsTests.cpp' },
    @{ Name = 'OutputRevealHandoff'; Source = 'OptiScaler/with_dx12/tests/OutputRevealHandoffTests.cpp'; Libraries = 'user32.lib' },
    @{ Name = 'VisibleLeaseCompletion'; Source = 'OptiScaler/with_dx12/tests/VisibleLeaseCompletionTests.cpp'; Libraries = 'user32.lib d3d12.lib dxgi.lib' },
    @{ Name = 'GenshinFsrHudWiring'; Source = 'OptiScaler/runtime/tests/GenshinFsrHudWiringTests.cpp'; AdditionalSource = 'OptiScaler/genshin/GenshinHookCpuTiming.cpp' },
    @{ Name = 'StarRailResidencyPolicy'; Source = 'OptiScaler/upscalers/tests/StarRailResidencyPolicyTests.cpp' },
    @{ Name = 'NrResident'; Source = 'OptiScaler/runtime/tests/NrResidentTests.cpp' },
    @{ Name = 'NrResidentConsumer'; Source = 'OptiScaler/runtime/tests/NrResidentConsumerTests.cpp'; StubInclude = 'OptiScaler/runtime/tests/nr-resident/stubs' },
    @{ Name = 'FenceCompletion'; Source = 'OptiScaler/runtime/tests/FenceCompletionTests.cpp' },
    @{ Name = 'CpuMappingTransactions'; Source = 'OptiScaler/runtime/tests/CpuMappingTransactionsTests.cpp' },
    @{ Name = 'CpuChangedByteRanges'; Source = 'OptiScaler/runtime/tests/CpuChangedByteRangesTests.cpp' },
    @{ Name = 'CpuIndexBounds'; Source = 'OptiScaler/runtime/tests/CpuIndexBoundsTests.cpp' },
    @{ Name = 'HookTimingPublication'; Source = 'OptiScaler/genshin/tests/HookTimingPublicationTests.cpp'; AdditionalSource = 'OptiScaler/genshin/GenshinHookCpuTiming.cpp' },
    @{ Name = 'CpuMappingDx11'; Source = 'OptiScaler/runtime/tests/CpuMappingDx11Tests.cpp'; Libraries = 'd3d11.lib' },
    @{ Name = 'Dx11SharedRead'; Source = 'OptiScaler/runtime/tests/Dx11SharedReadTests.cpp'; Libraries = 'd3d11.lib d3d12.lib dxgi.lib' },
    @{ Name = 'Dx11RasterBindings'; Source = 'OptiScaler/runtime/tests/Dx11RasterBindingsTests.cpp'; Libraries = 'd3d11.lib d3dcompiler.lib' },
    @{ Name = 'DepthRetention'; Source = 'OptiScaler/genshin/tests/depth-retention/DepthRetentionTests.cpp'; Libraries = 'd3d11.lib' },
    @{ Name = 'CpuWriterProbe'; Source = 'OptiScaler/genshin/tests/CpuWriterProbeTests.cpp'; AdditionalSource = 'OptiScaler/genshin/GenshinCpuWriterProbe.cpp'; StubInclude = 'OptiScaler/genshin/tests/cpu-writer-probe/stubs' },
    @{ Name = 'RecordingBackpressure'; Source = 'OptiScaler/runtime/tests/RecordingBackpressureTests.cpp'; AdditionalSource = 'OptiScaler/inputs/GenshinFsrContract.cpp' },
    @{ Name = 'Sm86Settings'; Source = 'OptiScaler/runtime/tests/Sm86SettingsTests.cpp' },
    @{ Name = 'PrivateFgTestModule'; Source = 'OptiScaler/runtime/tests/PrivateFgTestModuleTests.cpp'; AdditionalSource = 'tests/private-fg-test/fixture.cpp'; StubInclude = 'tests/private-fg-test/stubs'; Libraries = 'bcrypt.lib' },
    @{ Name = 'LoaderStartupContext'; Source = 'OptiScaler/runtime/tests/LoaderStartupContextTests.cpp' },
    @{ Name = 'UpscalerFrameParameters'; Source = 'OptiScaler/inputs/FG/tests/UpscalerFrameParametersTests.cpp' },
    @{ Name = 'SceneGuideContract'; Source = 'OptiScaler/dlssnr/tests/nr_scene_guide_contract.cpp' },
    @{ Name = 'NrDetailContinuity'; Source = 'OptiScaler/dlssnr/tests/nr_detail_history_continuity.cpp' },
    @{ Name = 'NrNativeDetailHost'; Source = 'OptiScaler/dlssnr/tests/nr_native_detail_host.cpp'; StubInclude = 'OptiScaler/runtime/tests/nr-resident/stubs' },
    @{ Name = 'NrModelChainHistory'; Source = 'OptiScaler/dlssnr/tests/nr_model_chain_history.cpp' },
    @{ Name = 'NrDepthOrientation'; Source = 'OptiScaler/dlssnr/tests/nr_depth_orientation.cpp' },
    @{ Name = 'NrReconfiguration'; Source = 'OptiScaler/dlssnr/tests/nr-reconfiguration/fixture.cpp' },
    @{ Name = 'DlssgOptionsPolicy'; Source = 'OptiScaler/framegen/dlssg/tests/DlssgOptionsPolicyTests.cpp' },
    @{ Name = 'HookCpuProcessOverride'; Source = 'OptiScaler/genshin/tests/hook_cpu_process_override.cpp'; ProcessEnvironment = $true }
)

$SuiteName = @($SuiteName | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if ($SuiteName.Count) {
    $unknown = @($SuiteName | Where-Object { $_ -notin $suites.Name })
    if ($unknown.Count) { throw "Unknown CPU suite: $($unknown -join ', ')" }
    $suites = @($suites | Where-Object { $_.Name -in $SuiteName })
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
if ('NrReconfiguration' -in $suites.Name) {
    & $Python -B (Join-Path $root 'OptiScaler/dlssnr/tests/nr-reconfiguration/prepare.py') --emit
    if ($LASTEXITCODE -ne 0) { throw 'Cannot extract current NR reconfiguration implementation' }
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
    } elseif ($suite.Name -eq 'ShaderCurrent' -and
              (Test-Path -LiteralPath (Join-Path $root 'build/fsr-78057A29AF6C2D99.dxbc')) -and
              (Test-Path -LiteralPath (Join-Path $root 'build/fsr-FEAC669E48FD931E.dxbc'))) {
        & $exe (Join-Path $root 'build')
        if ($LASTEXITCODE -ne 0) { throw "Real captured shader authentication failed with exit $LASTEXITCODE" }
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
        }
        if ($suite.Name -eq 'GuidePreviewWarp') {
            & $exe --producer-replay
            if ($LASTEXITCODE -ne 0) { throw 'Preview first-use producer replay failed' }
        }
        if ($suite.Name -eq 'FgHandoffWarp') {
            foreach ($negative in @('--negative-queue', '--negative-guides', '--negative-handback')) {
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

& $Python -B (Join-Path $root 'OptiScaler/with_dx12/tests/test_genshin_output_owner.py')
if ($LASTEXITCODE -ne 0) { throw "Output owner wiring checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/genshin/tests/check_cpu_mapping_wiring.py')
if ($LASTEXITCODE -ne 0) { throw "CPU mapping wiring checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/hooks/tests/test_fg_sdk_present_timing.py') --cpu
if ($LASTEXITCODE -ne 0) { throw "SDK Present observation checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/hooks/tests/test_fg_source_scheduling.py') --cpu
if ($LASTEXITCODE -ne 0) { throw "FG source pacing/query/latency checks failed with exit $LASTEXITCODE" }

& (Join-Path $root 'tools/tests/Test-F6DisplayTiming.ps1')
if ($LASTEXITCODE -ne 0) { throw "Display event parser checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/hooks/tests/test_fg_reflex_ownership.py')
if ($LASTEXITCODE -ne 0) { throw "FG Reflex source/output ownership checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/with_dx12/tests/test_interop_copy_retirement.py') --cpu
if ($LASTEXITCODE -ne 0) { throw "Interop copy reader retirement checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/with_dx12/tests/test_private_copy_wait.py') --cpu
if ($LASTEXITCODE -ne 0) { throw "Private copy queue ordering checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/genshin/tests/test_legacy_owner_boundary.py')
if ($LASTEXITCODE -ne 0) { throw "Owner callback boundary checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/genshin/tests/test_resource_observation.py') --cpu
if ($LASTEXITCODE -ne 0) { throw "Resource observation lifecycle checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/genshin/tests/test_input_observation.py')
if ($LASTEXITCODE -ne 0) { throw "New input observation checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/genshin/tests/test_observed_settings.py')
if ($LASTEXITCODE -ne 0) { throw "Observed settings checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/genshin/tests/test_returned_raster_validation.py')
if ($LASTEXITCODE -ne 0) { throw "Returned raster validation checks failed with exit $LASTEXITCODE" }

& $Python -B (Join-Path $root 'OptiScaler/genshin/tests/test_returned_raster_capture.py')
if ($LASTEXITCODE -ne 0) { throw "Returned raster capture checks failed with exit $LASTEXITCODE" }

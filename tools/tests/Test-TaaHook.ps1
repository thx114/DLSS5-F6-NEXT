#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Artifact,
    [Parameter(Mandatory)][string]$DlssRuntime,
    [ValidateSet('dlss','dlss_12','auto')][string]$Backend='dlss_12',
    [string]$OutputRoot,
    [ValidateSet('default','native','wrapped')][string]$FactoryRoute='default',
    [string]$ProjectionEvidence,
    [string]$ConsumerEvidence,
    [string]$PackedEvidence,
    [string]$MatrixEvidence,
    [string]$ObservationShader,
    [string]$NrRuntimeDirectory,
    [string]$CorePath='D:/APPS/test/TEST/HoYoShadeHub_Portable_1.4.5.0_x64_Full/HoYoShade/ReShade64.dll',
    [switch]$EnableSourceNr,
    [ValidateSet('host','starrail')][string]$GameIdentity='host',
    [ValidateSet('before-sr-r11','before-sr-fp16','before-sr-f32','off','discover','skip','pixel','pixel-source-nr','auto-source-return','auto-observation-joined','pixel-upscale','pixel-fullsize','pixel-fullsize-replay','auto-fullsize-pattern','auto-fullsize-pattern-flip','auto-fullsize-matrix-replay','auto-continuity','auto-fullsize-continuity','auto-fullsize-pattern-replay','auto-fullsize','auto-fullsize-replay','auto-fullsize-dispatch','auto-fullsize-opaque','auto-pixel-larger-screen','auto-packed-fullsize','auto-packed-gap','auto-packed','auto-packed-range','auto-packed-recovery','compute','compute-upscale','shutdown','state-switch','auto-pixel','auto-pixel-upscale','auto-compute','auto-compute-upscale','auto-no-jitter','auto-switch','auto-linked-pixel','auto-linked-compute','auto-unsupported','auto-raster-evidence','auto-raster-warmup','auto-raster-timed','auto-raster-extent','auto-projection','auto-projection-timed','auto-projection-stale','auto-projection-mismatch','auto-projection-unknown','auto-projection-multi','auto-projection-no-dsv-default','auto-projection-no-dsv-enabled','auto-projection-dsv-always','auto-projection-dsv-less')][string[]]$Cases=@(),
    [string]$DevCmd='D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat'
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$Artifact) {$Artifact=Join-Path $root 'build/out/OptiScaler.dll'}
$dll=(Get-Item -LiteralPath $Artifact).FullName
$runtime=(Get-Item -LiteralPath $DlssRuntime).FullName
if(!$Cases.Count) {
    $Cases=@('off','discover','skip','pixel','pixel-upscale','compute','compute-upscale','state-switch','auto-pixel','auto-pixel-upscale','auto-compute','auto-compute-upscale','auto-no-jitter','auto-switch','auto-linked-pixel','auto-linked-compute','auto-unsupported','auto-raster-evidence')
    # Native vendor Shutdown1 is an explicit separate test: the old direct
    # receipt host already reproduces its _nvngx+44e54 AV on this machine.
    # -Cases shutdown retains that failure; it is never caught or passed.
    if($Backend -eq 'dlss_12') {$Cases+='shutdown'}
}
$testRoot=if($OutputRoot){[IO.Path]::GetFullPath($OutputRoot)}else{Join-Path $root "build/taa-hook-tests/$Backend"}
$null=[IO.Directory]::CreateDirectory($testRoot)
$exe=Join-Path $testRoot 'TaaHookHost.exe'
$source=Join-Path $root 'OptiScaler/inputs/taa/tests/TaaHookHost.cpp'
$command='call "{0}" -no_logo -arch=x64 -host_arch=x64 && cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /Od /I"{1}/OptiScaler" /Fe:"{2}" /Fo:"{3}/" "{4}" /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib' -f $DevCmd,$root,$exe,$testRoot,$source
& cmd.exe /d /s /c $command *> (Join-Path $testRoot 'compile.log')
if ($LASTEXITCODE -ne 0) {Get-Content (Join-Path $testRoot 'compile.log');throw 'TAA hook host compilation failed'}
$results=@()
function Add-FixtureLibrary([string]$Source,[string]$Destination) {
    # These libraries are immutable inputs. Reuse their file object on the
    # same volume instead of duplicating it for every isolated fixture case.
    if(Test-Path -LiteralPath $Destination) {
        if((Get-FileHash -LiteralPath $Destination).Hash -eq (Get-FileHash -LiteralPath $Source).Hash) {return}
        # Unlink this fixture path; never write through an earlier hard link.
        Remove-Item -LiteralPath $Destination
    }
    if([StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetPathRoot($Source),[IO.Path]::GetPathRoot($Destination))) {
        $null=New-Item -ItemType HardLink -Path $Destination -Target $Source -ErrorAction Stop
    } else {Copy-Item -LiteralPath $Source -Destination $Destination}
}
foreach($case in $Cases) {
    $path=Join-Path $testRoot $case
    $null=[IO.Directory]::CreateDirectory($path)
    Add-FixtureLibrary $dll (Join-Path $path 'OptiScaler.dll')
    Add-FixtureLibrary $runtime (Join-Path $path 'nvngx_dlss.dll')
    if($case.StartsWith('auto-projection')) {
        if(!$ProjectionEvidence){throw 'Projection cases require exact captured shader evidence'}
        Copy-Item -LiteralPath (Join-Path $ProjectionEvidence 'PS-137B32A04AB724F0.dxbc') -Destination (Join-Path $path 'projection-motion.ps.dxbc')
        Copy-Item -LiteralPath (Join-Path $ProjectionEvidence 'VS-F9C1F953EE80980A.dxbc') -Destination (Join-Path $path 'projection-motion.vs.dxbc')
    }
    if($case -eq 'auto-observation-joined') {
        if(!$ObservationShader){throw 'Observation case requires complete captured ObservationShader'}
        Copy-Item -LiteralPath $ObservationShader -Destination (Join-Path $path 'unresolved-temporal.ps.dxbc')
    }
    if($case.Contains('matrix')) {
        if(!$MatrixEvidence){throw 'Matrix cases require captured MatrixEvidence'}
        Copy-Item -LiteralPath (Join-Path $MatrixEvidence 'PS-6CA04A04AFEE2570.dxbc') -Destination (Join-Path $path 'matrix.ps.dxbc')
        Copy-Item -LiteralPath (Join-Path $MatrixEvidence 'VS-F1CE86BC0BE8E994.dxbc') -Destination (Join-Path $path 'matrix.vs.dxbc')
    }
    $fullsize=$case.Contains('fullsize') -or $case -eq 'auto-source-return'
    if($fullsize -and !$case.StartsWith('auto-packed')) {
        if(!$ConsumerEvidence){throw 'Full-size cases require exact captured ConsumerEvidence'}
        Copy-Item -LiteralPath (Join-Path $ConsumerEvidence 'PS-F63F3FF630FEBDB4.dxbc') -Destination (Join-Path $path 'consumer.ps.dxbc')
        Copy-Item -LiteralPath (Join-Path $ConsumerEvidence 'VS-55E141E63591B33C.dxbc') -Destination (Join-Path $path 'consumer.vs.dxbc')
        if($case.Contains('pattern') -and $case.Contains('replay')) {Copy-Item -LiteralPath (Join-Path $ConsumerEvidence 'PS-88CBD14623C28B8E.dxbc') -Destination (Join-Path $path 'post.ps.dxbc')}
    }
    if($case.StartsWith('auto-packed')) {
        if(!$PackedEvidence){throw 'Packed cases require exact captured PackedEvidence'}
        Copy-Item -LiteralPath (Join-Path $PackedEvidence 'PS-AE2AA729C623837A.dxbc') -Destination (Join-Path $path 'packed-producer.ps.dxbc')
        Copy-Item -LiteralPath (Join-Path $PackedEvidence 'VS-98109168E808BC85.dxbc') -Destination (Join-Path $path 'packed-producer.vs.dxbc')
        Copy-Item -LiteralPath (Join-Path $PackedEvidence 'PS-B12FF59247A26796.dxbc') -Destination (Join-Path $path 'packed-resolve.ps.dxbc')
        if($fullsize){Copy-Item -LiteralPath (Join-Path $PackedEvidence 'PS-F3EF7F661CCF4AF8.dxbc') -Destination (Join-Path $path 'packed-consumer.ps.dxbc')}
    }
    $beforeSr=$case.StartsWith('before-sr-')
    if($beforeSr) {
        $coreHash=(Get-FileHash -LiteralPath $CorePath -Algorithm SHA256).Hash
        if($coreHash -ne '0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7') {throw 'Float fixture requires the pinned ReShade core'}
        $null=[IO.Directory]::CreateDirectory((Join-Path $path 'HoYoShade'))
        Add-FixtureLibrary $CorePath (Join-Path $path 'HoYoShade/ReShade64.dll')
        Copy-Item -LiteralPath (Join-Path $root 'OptiScaler/integration/tests/before-sr/ProbeFloat.fx') -Destination $path
        [IO.File]::WriteAllText((Join-Path $path 'preset.ini'),"Techniques=ProbeFloat@ProbeFloat.fx`nTechniqueSorting=ProbeFloat@ProbeFloat.fx`n")
        [IO.File]::WriteAllText((Join-Path $path 'ReShade.ini'),"[GENERAL]`nEffectSearchPaths=$path`nPresetPath=$path/preset.ini`n[OVERLAY]`nTutorialProgress=4`n")
    }
    $beforeSrEnabled=if($beforeSr){'true'}else{'false'}
    $sourceNr=$EnableSourceNr -or $case -in @('pixel-source-nr','auto-source-return')
    if($sourceNr) {
        if(!$NrRuntimeDirectory){throw 'Source NR case requires verified NrRuntimeDirectory'}
        foreach($name in @('nvngx_dlssnr.dll','nvngx.dll_dlssnr.dll')) {Add-FixtureLibrary (Join-Path $NrRuntimeDirectory $name) (Join-Path $path $name)}
    }
    $nrEnabled=if($sourceNr){'true'}else{'false'}
    $sourceReturn=$case -eq 'auto-source-return'
    $interop=if($sourceReturn -or $FactoryRoute -ne 'default'){"FGInput=Upscaler
FGOutput=DLSSG
External=false"}else{''}
    $resident=if($sourceReturn){'StarRailForceResident=true'}else{''}
    $caseExe=$exe
    if($beforeSr) {$caseExe=Join-Path $path 'TaaHookHost.exe';Copy-Item -LiteralPath $exe -Destination $caseExe}
    $caseIdentity=if($sourceReturn -or $GameIdentity -eq 'starrail'){'starrail'}else{'host'}
    if($caseIdentity -eq 'starrail') {$caseExe=Join-Path $path 'StarRail.exe';Copy-Item -LiteralPath $exe -Destination $caseExe}
    $mode=if($case -eq 'off'){'Off'}elseif($case -eq 'discover'){'Discover'}else{'Replace'}
    $skip=if($case -eq 'skip'){'100'}else{'auto'}
    $profile=if($case.StartsWith('auto')){'auto'}else{'taa-profile.ini'}
    # Fixture-only INI. Installed user settings and runtime choices are untouched.
    $ini=@"
[Upscalers]
Dx11Upscaler=$Backend
Dx12Upscaler=dlss
[TAA]
Mode=$mode
Profile=$profile
Candidate=auto
[FrameGen]
Enabled=false
$interop
[DlssNr]
Enabled=$nrEnabled
FinishedPicture=true
$resident
PresentedTemporalDetail=true
PresentedTemporalDurationMs=1000
MotionYInverted=true
DepthTextureFlipY=true
MotionTextureFlipY=true
[HoYoShade]
Enabled=$beforeSrEnabled
Stage=1
[CAS]
Enabled=false
[OutputScaling]
Enabled=false
[Plugins]
Path=$path/plugins
LoadAsiPlugins=false
[Libraries]
OptiDllPath=$path
NvngxDlssPath=$path
[Spoofing]
DxgiFactoryWrapping=$(if($FactoryRoute -eq 'wrapped'){'true'}elseif($FactoryRoute -eq 'native'){'false'}else{'auto'})
[Hotfix]
SkipFirstFrames=$skip
[Log]
LogToFile=true
LogToConsole=false
LogLevel=1
LogAsync=false
LogFileName=./OptiScaler.log
"@
    [IO.File]::WriteAllText((Join-Path $path 'OptiScaler.ini'),$ini,[Text.UTF8Encoding]::new($false))
    $arguments=@($case);if($FactoryRoute -ne 'default') {$arguments+=$FactoryRoute}
    $process=Start-Process -FilePath $caseExe -ArgumentList $arguments -WorkingDirectory $path -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $path 'stdout.log') -RedirectStandardError (Join-Path $path 'stderr.log')
    $null=$process.Handle
    $timeout=if($beforeSr){65000}else{45000}
    $finished=$process.WaitForExit($timeout)
    if(!$finished) {$process.Kill();$process.WaitForExit()}
    $stdout=[IO.File]::ReadAllText((Join-Path $path 'stdout.log'))
    $log=if(Test-Path (Join-Path $path 'OptiScaler.log')){[IO.File]::ReadAllText((Join-Path $path 'OptiScaler.log'))}else{''}
    $outcome=if($case -eq 'auto-observation-joined'){'PASS actual B12 seed and same-owner raster joins:'}else{'PASS actual F6 TAA Hook and provider pixels'}
    $pass=$finished -and $process.ExitCode -eq 0 -and $stdout.Contains($outcome)
    if($beforeSr) {
        $pass=$pass -and $stdout.Contains('PASS independent TAA BeforeSR float source values:')
        $pass=$pass -and $stdout.Contains('phases=3 verified=12 source_unchanged=true auxiliary_unchanged=true')
        if($Backend -eq 'dlss') {
            $pass=$pass -and $log.Contains('source_values=true') -and $log.Contains('HoYoShade BeforeSR native executed')
        } else {
            $pass=$pass -and $log.Contains('producer_color=source-values') -and $log.Contains('effects_executed=true')
        }
    }
    # The typed internal TAA receipt path bypasses the exported wrapper's numeric status log.
    # Success markers follow receipt identity checks and the actual physical return.
    $privateBoundary=$null
    if($FactoryRoute -ne 'default') {
        $interopCreates=[regex]::Matches($log,'Dx11wDx12SC::Dx11wDx12SC Dx11wDx12SC \d+ created,').Count
        $plainCreates=[regex]::Matches($log,'Created new swapchain:').Count
        $wrappedCreates=[regex]::Matches($log,'Created new WrappedIDXGISwapChain4:').Count
        $privateBoundary=[pscustomobject]@{PublicInteropCreates=$interopCreates;UnexpectedPlainCreates=$plainCreates;UnexpectedWrappedCreates=$wrappedCreates}
        $pass=$pass -and $stdout.Contains("PASS actual public factory route: $FactoryRoute wrapped=")
        $pass=$pass -and $interopCreates -eq 1 -and $plainCreates -eq 0 -and $wrappedCreates -eq 0
    }
    $ngxEvaluated=$log.Contains('EvaluateDx11 NVSDK_NGX_D3D11_EvaluateFeature Handle:')
    if($mode -eq 'Discover') {$pass=$pass -and $log.Contains('TAA candidate') -and !$log.Contains('TAA evaluate:')}
    elseif($case -eq 'skip') {$pass=$pass -and $ngxEvaluated -and [regex]::IsMatch($log,'TAA evaluate:[^\r\n]*copied_back=false') -and !$log.Contains('copied_back=true') -and !$log.Contains('TAA consumer delivered:')}
    elseif($case -in @('auto-observation-joined','auto-no-jitter','auto-unsupported','auto-raster-evidence','auto-raster-warmup','auto-raster-timed','auto-raster-extent','auto-projection-stale','auto-projection-mismatch','auto-projection-unknown','auto-projection-multi','auto-projection-dsv-less')) {
        $pass=$pass -and !$log.Contains('TAA evaluate:') -and $log.Contains('TAA analysis:') -and $log.Contains('ready=false')
        if($case -eq 'auto-unsupported') {$pass=$pass -and $log.Contains('unsupported_instruction=discard')}
    }
    elseif($case -eq 'auto-fullsize-opaque') {$pass=$pass -and $log.Contains('reason=opaque command-list submission') -and !$log.Contains('TAA consumer delivered:') -and $log.Contains('copied_back=true')}
    elseif($fullsize) {$pass=$pass -and $ngxEvaluated -and [regex]::IsMatch($log,'TAA consumer delivered: render=256x256 output=427x427[^\r\n]*physical_returned=true');if($case.StartsWith('auto')){$pass=$pass -and $log.Contains('source=automatic')}}
    elseif($beforeSr) {$pass=$pass -and $log.Contains('TAA evaluate:') -and $log.Contains('copied_back=true')}
    elseif($mode -eq 'Replace') {$pass=$pass -and $log.Contains('copied_back=true') -and $ngxEvaluated;if($case.StartsWith('auto')){$pass=$pass -and $log.Contains('source=automatic')}}
    else {$pass=$pass -and !$log.Contains('TAA evaluate:')}
    if($Backend -eq 'auto') {$pass=$pass -and $log.Contains('resolved=DLSS') -and !$log.Contains('Creating new FSR 2.2.1 feature')}
    if($case.Contains('pattern')) {$pass=$pass -and $stdout.Contains('PASS directional source and final consumer pixels')}
    if($case.StartsWith('auto')) {$pass=$pass -and $stdout.Contains('PASS exact asynchronous TAA shader evidence')}
    if($case -in @('auto-raster-evidence','auto-raster-warmup','auto-raster-timed','auto-raster-extent')) {$pass=$pass -and $stdout.Contains('PASS linked indexed instanced raster VS and consecutive observed constants')}
    if($case -in @('auto-raster-timed','auto-raster-extent')) {$pass=$pass -and $stdout.Contains('PASS actual four timed capture bursts:') -and $log.Contains('TAA capture window complete:')}
    if($case -eq 'auto-raster-warmup') {$pass=$pass -and $stdout.Contains('PASS actual delayed producer after twelve unrelated owner frames with CPU constant warmup')}
    if($case.StartsWith('auto-projection')) {$pass=$pass -and $stdout.Contains('PASS actual indexed projection source without resolve jitter:')}
    if($case -eq 'auto-projection-timed') {$pass=$pass -and $log.Contains('TAA capture window complete:')}
    if($case -eq 'auto-projection-dsv-less') {$pass=$pass -and $log.Contains('reason=depth-tested TAA draw')}
    if($case -eq 'auto-raster-extent') {
        $pass=$pass -and $stdout.Contains('PASS actual late extent capture bursts:')
        $pass=$pass -and $log.Contains('TAA capture rearmed: epoch=1') -and $log.Contains('TAA capture rearmed: epoch=2')
        $pass=$pass -and !$log.Contains('TAA capture rearmed: epoch=3')
    }
    $coverageSamples=0
    if($case -in @('auto-projection-no-dsv-default','auto-projection-no-dsv-enabled','auto-projection-dsv-always','auto-projection-dsv-less')) {
        $hash=[regex]::Match([IO.File]::ReadAllText((Join-Path $path 'taa-profile.ini')),'ShaderHash=([0-9a-fA-F]+)').Groups[1].Value.ToUpperInvariant()
        $records=@(Get-ChildItem -LiteralPath (Join-Path $path "taa-shaders/$($process.Id)/temporal") -Filter "PS-$hash-*.meta.txt")
        $bound=if($case -in @('auto-projection-dsv-always','auto-projection-dsv-less')){'true'}else{'false'}
        $default=if($case -eq 'auto-projection-no-dsv-default'){'true'}else{'false'}
        $coverage=if($bound -eq 'false'){'no_attachment'}elseif($case -eq 'auto-projection-dsv-always'){'unconditional'}else{'depth_test'}
        foreach($record in $records){
            $text=[IO.File]::ReadAllText($record.FullName)
            $pass=$pass -and $text.Contains("dsv_bound=$bound dsv_valid=$bound") -and $text.Contains("depth_state_default=$default") -and $text.Contains("ds_coverage=$coverage") -and $text.Contains('stencil_front func=') -and $text.Contains('stencil_back func=')
        }
        $coverageSamples=$records.Count;$pass=$pass -and $coverageSamples -gt 0
    }
    if($sourceNr -and $Backend -in @('dlss_12','auto')) {
        $sourceWrites=[regex]::Matches($log,'F6 source NR:.*written=true').Count
        $pass=$pass -and $sourceWrites -ge 1 -and $log.Contains('stage=sr-output same_recording=true')
        if(!$sourceReturn) {
            # This case covers the source model's full guide/feedback receipts.
            $pass=$pass -and $log.Contains('orderedNativeQueue=true')
            # In the switch case, sparse global samples may belong to Present NR.
            $pass=$pass -and $log.Contains('hasDepth=true')
            $pass=$pass -and $log.Contains('detailFeedbackActive=true detailFeedbackHistoryUsable=true')
            $pass=$pass -and $log.Contains('detailFeedbackRecorded=true detailFeedbackPublished=true')
            $pass=$pass -and $log.Contains('feedbackReset=false')
        }
        if($case -eq 'auto-source-return') {
            $pass=$pass -and $log.Contains('fresh-current-color-without-native-guides')
            $pass=$pass -and $log.Contains('Dx11wDx12SC 1 created')
            $sourceMatches=[regex]::Matches($log,'F6 source NR: source=\d+ generation=(\d+)[^\r\n]*written=true')
            $taaHandles=@([regex]::Matches($log,'NVSDK_NGX_D3D11_CreateFeature HandleId: (\d+)')|ForEach-Object {$_.Groups[1].Value})
            # Initial output-layout discovery rebuilds once. The TAA instance
            # then survives Present NR fallback and returns with the same ID.
            $pass=$pass -and $taaHandles.Count -eq 2 -and $sourceMatches.Count -ge 2
            if($taaHandles.Count -eq 2) {
                $stable=$taaHandles[-1]
                $stableSources=@($sourceMatches|Where-Object {$_.Groups[1].Value -eq $stable})
                $resume=[regex]::Match($log,"TAA continuity: source_frame=49 feature=$stable reset=true render=256x256 output=427x427 current=true")
                $pass=$pass -and $stableSources.Count -ge 2 -and $resume.Success
                # NR's sparse source log need not repeat after the same SR
                # handle returns. Verify its fresh recording after recovery,
                # alongside the host's 18+18 current consumer pixel assertions.
                if($resume.Success) {
                    $tail=$log.Substring($resume.Index)
                    $pass=$pass -and [regex]::IsMatch($tail,'DLSS-NR first model recording: epoch=\d+[^\r\n]*result=1')
                }
            }
        } else {$pass=$pass -and !$log.Contains('fresh-current-color-without-native-guides')}
    }
    if($case.Contains('matrix')) {$pass=$pass -and $stdout.Contains('PASS exact PS6CA VSF1CE matrix and multi-texture oracle:')}
    $continuityProof=$null
    if($case.Contains('continuity')) {
        $pass=$pass -and $stdout.Contains('PASS stable feature through temporal gaps:')
        $created=@([regex]::Matches($log,'NVSDK_NGX_D3D11_CreateFeature HandleId: (\d+)')|ForEach-Object {$_.Groups[1].Value})
        $released=[regex]::Matches($log,'F5 lifecycle begin: provider D3D(?:11|12) ReleaseFeature handle ').Count
        $evaluated=@([regex]::Matches($log,'EvaluateDx11 NVSDK_NGX_D3D11_EvaluateFeature Handle: (\d+)')|ForEach-Object {$_.Groups[1].Value})
        $continuity=@([regex]::Matches($log,'TAA continuity: source_frame=(\d+) feature=(\d+) reset=(true|false)'))
        $resetFrames=@($continuity|Where-Object {$_.Groups[3].Value -eq 'true'}|ForEach-Object {[int]$_.Groups[1].Value})
        $expectedCreates=if($fullsize){2}else{1}
        # Native DX11 releases its vendor object directly in DLSSFeatureDx11's
        # destructor; only the interop provider emits this release trace.
        $expectedReleases=if($fullsize -and $Backend -ne 'dlss'){1}else{0}
        $expectedEvals=if($fullsize){57}else{58}
        $expectedResets=if($fullsize){'3,5,33,46'}else{'3,33,46'}
        $sameHandle=$created.Count -gt 0 -and @($evaluated|Where-Object {$_ -ne $created[-1]}).Count -eq $(if($fullsize){1}else{0})
        $pass=$pass -and $created.Count -eq $expectedCreates -and $released -eq $expectedReleases -and $evaluated.Count -eq $expectedEvals -and $sameHandle
        $pass=$pass -and ($resetFrames -join ',') -eq $expectedResets
        $pass=$pass -and !$log.Contains('TAA SR evaluation failed')
        # The sole high-size rebuild is the actual 256->427 layout change.
        # Source gaps/jitter gaps reset history on return; a rejected consumer
        # still evaluates current full-size SR without resetting or rebuilding it.
        $continuityProof=[pscustomobject]@{Created=$created;ProviderReleases=$released;Evaluations=$evaluated.Count;ResetSourceFrames=$resetFrames;StableFinalHandle=$sameHandle}
    }
    $row=[pscustomobject]@{Case=$case;Backend=$Backend;FactoryRoute=$FactoryRoute;PrivateBoundary=$privateBoundary;GameIdentity=$caseIdentity;Pass=$pass;Continuity=$continuityProof;CoverageSamples=$coverageSamples;PID=$process.Id;Exit=$process.ExitCode;Timeout=(!$finished);ArtifactSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'OptiScaler.dll')).Hash;RuntimeSHA256=(Get-FileHash -LiteralPath (Join-Path $path 'nvngx_dlss.dll')).Hash;Path=$path;Pixels=$stdout.Trim()}
    $row | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $path 'receipt.json') -Encoding utf8
    $results+=$row
    $row | Format-List
}
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $testRoot 'receipt.json') -Encoding utf8
if(@($results|Where-Object {!$_.Pass}).Count) {throw "TAA hook fixture failed; see $testRoot"}

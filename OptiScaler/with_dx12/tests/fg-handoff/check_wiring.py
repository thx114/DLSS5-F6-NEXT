"""Static integration checks; GPU ordering and pixels are tested by fixture.cpp."""
from pathlib import Path
import importlib.util
import re
import unittest

ROOT = Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location('extract_nr', ROOT / 'OptiScaler/dlssnr/tests/nr-reconfiguration/prepare.py')
parser = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parser)

def read(path):
    return (ROOT / path).read_text(encoding='utf-8-sig')

FG = read('OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp')
FG += "\n" + "\n".join(p.read_text(encoding="utf-8-sig") for p in sorted(
    next(a / "OptiScaler/framegen/dlssg" for a in Path(__file__).resolve().parents
         if (a / "OptiScaler/framegen/dlssg").is_dir()).glob("DLSSG_Dx12_*.cpp")))
DX11 = read('OptiScaler/with_dx12/dx11_with_dx12.cpp')
WITH = read('OptiScaler/with_dx12/with_dx12.cpp')

class HandoffWiring(unittest.TestCase):
    def test_all_factory_publications_use_role_guard(self):
        for path, count in (('OptiScaler/hooks/DxgiFactory_Hooks.cpp', 6),
                            ('OptiScaler/hooks/DxgiFactory_WrappedCalls.cpp', 3)):
            source = read(path)
            self.assertEqual(source.count('WithDx12::AdoptSwapchainQueue('), count, path)
            # Existing DX11 construction may mirror the already adopted pair.
            remaining = source.replace('State::Instance().currentCommandQueue = WithDx12::GetD3D12CommandQueue();', '')
            self.assertIsNone(re.search(r'currentCommandQueue\s*=(?!=)', remaining), path)
        adopt = parser.extract(WITH, 'void WithDx12::AdoptSwapchainQueue(')
        setter = parser.extract(WITH, 'void WithDx12::SetD3D12Objects(')
        self.assertLess(adopt.index('CreationScope::Active()'), adopt.index('currentCommandQueue ='))
        for mutation in ('_d3d12Device =', '_d3d12CommandQueue =', 'UpdateStateObjects()'):
            self.assertLess(setter.index('CreationScope::Active()'), setter.index(mutation))

    def test_sdk_factories_have_creation_scope(self):
        for call in ('factory->CreateSwapChain(cmdQueue', 'factory2->CreateSwapChainForHwnd(cmdQueue'):
            pos = FG.index(call)
            self.assertIn('CreationScope outputCreation(_finalOutputProvider)', FG[pos-180:pos])

    def test_physical_resize_keeps_output_queue_local(self):
        body = parser.extract(read('OptiScaler/wrapped/wrapped_swapchain.cpp'),
                              'HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::ResizeBuffers1(')
        self.assertIn('_device = *ppPresentQueue;', body)
        self.assertIn('if (!_providerOutput)\n            WithDx12::AdoptSwapchainQueue(', body)
        self.assertNotRegex(body, r'currentCommandQueue\s*=(?!=)')

    def test_evaluate_guard_seals_before_return(self):
        body = read('OptiScaler/inputs/FG/Upscaler_Inputs_Dx11wDx12.cpp')
        self.assertLess(body.index('GuardExecution(FGExecutionScope::Inputs)'), body.index('SetUpscalerGuidePair('))
        start = body.index('const bool tagged=own->SetUpscalerGuidePair(')
        end = body.index('return;', start)
        self.assertIn('game==GameRouting::Identity::Genshin', body[start:end])
        self.assertIn('game==GameRouting::Identity::StarRail && !starRailDiagnosticRead', body[start:end])
        self.assertIn('GuideCapture::NeedsOpenFgGuideRecording()', body[start:end])
        self.assertNotIn('GetPreviewDemand()', body[start:end])
        self.assertIn('own->SealDx11GuideInputs(tagged,_dx12CommandQueue)', body[start:end])

    def test_borrowed_inputs_pinned_before_first_publication(self):
        body = parser.extract(FG, 'bool DLSSG_Dx12::SetUpscalerGuidePair(')
        pin = body.index('_dx11GuideInputs[slot][0]=depth;')
        self.assertLess(body.index('dx11-guide-source-queue-mismatch'), pin)
        self.assertLess(pin, body.index('publish(FG_ResourceType::Velocity'))
        self.assertIn('_dx11GuideBorrowed[slot]=true;', body)
        self.assertLess(body.index('guide-copy-generation-needs-drain'), body.index('borrowDx11Guides();'))
        self.assertLess(body.index('depth-scale-allocation'), body.index('borrowDx11Guides();'))
        self.assertIn('if (!scale) borrowDx11Guides();', body)

    def test_real_submit_then_handback_then_sealed_identity(self):
        body = parser.extract(FG, 'bool DLSSG_Dx12::SealDx11GuideInputs(')
        self.assertLess(body.index('CaptureAcceptedGuides(slot)'), body.index('SubmitUICommandList(slot)'))
        self.assertLess(body.index('SubmitUICommandList(slot)'), body.index('SyncDx12ToDx11(_gameCommandQueue)'))
        self.assertLess(body.index('SyncDx12ToDx11(_gameCommandQueue)'), body.index('_dx11GuideSealedFrame[slot]=_frameCount'))
        for required in ('BlockFgGuideWrites()', 'QuarantineFailedTeardown()', 'SameOwner(sourceQueue,_gameCommandQueue)'):
            self.assertIn(required, body)

    def test_every_shared_cache_write_follows_reader_seal(self):
        body = parser.extract(DX11, 'Dx11WithDx12::PrepareResourcesResult Dx11WithDx12::PrepareUpscalerResources(')
        guard = body.index('GuardExecution(FGExecutionScope::Lifecycle)')
        seal = body.index('guideOwner->SealDx11GuideInputs(true,Dx12CommandQueue)')
        self.assertLess(guard, seal)
        self.assertLess(seal, body.index('cache.frameId = frameId'))
        self.assertLess(seal, body.index('PrepareCachedResource('))
        self.assertNotIn('GuardExecution(FGExecutionScope::Inputs)', body)
        sealing = parser.extract(FG, 'bool DLSSG_Dx12::SealDx11GuideInputs(')
        self.assertLess(sealing.index('other!=slot && _dx11GuideBorrowed[other]'),
                        sealing.index('if (!_dx11GuideBorrowed[slot])'))
        pair = parser.extract(FG, 'bool DLSSG_Dx12::SetUpscalerGuidePair(')
        self.assertIn('game==GameRouting::Identity::StarRail', pair)
        present = parser.extract(FG, 'bool DLSSG_Dx12::Present(')
        self.assertLess(present.index('SealDx11GuideInputs(true,_gameCommandQueue)'),
                        present.index('if (IsStarRailUiOwner())'))

    def test_core_receipt_follows_real_signal(self):
        submit = parser.extract(FG, 'bool DLSSG_Dx12::SubmitUICommandList(')
        self.assertLess(submit.index('FAILED(_gameCommandQueue->Signal('), submit.index('RecordDx11GuideSubmission(index)'))
        self.assertLess(submit.index('IFGFeature_Dx12::SubmitUICommandList(index)'), submit.rindex('RecordDx11GuideSubmission(index)'))
        seal = parser.extract(FG, 'bool DLSSG_Dx12::SealDx11GuideInputs(')
        for check in ('submission.frame!=_frameCount', 'submission.generation!=depth.generation',
                      'submission.fence!=_uiFence', 'submission.value!=_uiAllocatorFenceValues[slot]',
                      'SameOwner(submission.queue,_gameCommandQueue)'):
            self.assertIn(check, seal)
        self.assertNotIn('_guideObservations', seal)

    def test_no_late_borrowed_guide_reread(self):
        capture = parser.extract(FG, 'void DLSSG_Dx12::CaptureAcceptedGuides(')
        present = parser.extract(FG, 'bool DLSSG_Dx12::Present(')
        self.assertIn('const bool ownedGuides=_dx11GuideSealedFrame[slot]!=_frameCount &&', capture)
        self.assertIn('_dx11GuideSealedFrame[fIndex]!=_frameCount', present)
        self.assertIn('GuideCapture::PreviewMode::FgColour', present)

    def test_exact_reader_queue_and_existing_fence_domain(self):
        body = parser.extract(DX11, 'bool Dx11WithDx12::SyncDx12ToDx11(')
        self.assertIn('completedReaderQueue ? completedReaderQueue : Dx12CommandQueue', body)
        self.assertIn('SyncDx12CommandQueue->QueryInterface', body)
        self.assertIn('else if (!EnsureSyncResourcesLocked())', body)
        self.assertLess(body.index('signalQueue->Signal('), body.index('Dx11DeviceContext->Wait('))
        for forbidden in ('WaitForSingleObject', 'GetCompletedValue', 'Sleep('):
            self.assertNotIn(forbidden, body)

    def test_terminal_failure_protects_cache_and_pins(self):
        for signature in ('void Dx11WithDx12::ReleaseSyncResourcesLocked(',
                          'bool Dx11WithDx12::EnsureSyncResourcesLocked(',
                          'void Dx11WithDx12::ResetUpscalerResourceCache(',
                          'void Dx11WithDx12::ReleaseSharedResource(',
                          'bool Dx11WithDx12::CopyTextureFrom11To12('):
            self.assertIn('FgGuideWritesBlocked.load', parser.extract(DX11, signature))
        quarantine = parser.extract(FG, 'void DLSSG_Dx12::QuarantineFailedTeardown(')
        self.assertIn('_dx11GuideInputs[i][0].Detach(); _dx11GuideInputs[i][1].Detach();', quarantine)
        release = parser.extract(FG, 'bool DLSSG_Dx12::ReleaseObjectsChecked(')
        self.assertIn('_dx11GuideInputs[i][0].Reset(); _dx11GuideInputs[i][1].Reset();', release)

if __name__ == '__main__':
    unittest.main()

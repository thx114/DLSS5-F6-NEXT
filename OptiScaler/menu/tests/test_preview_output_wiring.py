"""Static output ownership/order checks; GPU pixel coverage is a separate suite."""
from pathlib import Path

root = Path(__file__).resolve().parents[3]
overlay = (root / 'OptiScaler/menu/menu_overlay_dx.cpp').read_text(encoding='utf-8-sig')
common = (root / 'OptiScaler/menu/menu_common.cpp').read_text(encoding='utf-8-sig')
cache = (root / 'OptiScaler/menu/PreviewOutputCache.h').read_text(encoding='utf-8-sig')
render = overlay[overlay.index('static void RenderImGui_DX12('):overlay.index('ID3D12GraphicsCommandList* MenuOverlayDx::MenuCommandList()')]
present = overlay[overlay.index('void MenuOverlayDx::Present('):]
route = 'const bool sealedPreviewOutput = waitForOutputMenu || State::Instance().activeFgOutput == FGOutput::NoFG;'
assert route in present
assert present.index(route) < present.index('RenderImGui_DX11(pSwapChain)')
assert 'RenderImGui_DX12(pSwapChain, waitForOutputMenu, resumedGenshinContext, sealedPreviewOutput)' in present
assert 'bool resumedGenshinContext, const bool sealedPreviewOutput)' in render
assert 'FGEnabled' not in render and 'activeFgOutput' not in render and 'fgRoute' not in render
assert 'const bool sealedPreviewOutput =' not in render
assert 'g_menuPreviewOutput.Select(GuideCapture::GetPreviewDemand(), sealedPreviewOutput)' in render
assert 'const bool previewDemand = hasPreview || g_menuPreviewOutput.HasImage()' in render
assert 'frameSlot = SelectMenuFrameSlot();' in render
assert 'SelectMenuFrameSlot(!hasPreview)' not in render
assert render.index('g_menuPreviewOutput.Record(') < render.index('RenderWithBackbufferReceipt(')
assert 'if (previewReaderRecorded) g_menuPreviewReaders[frameSlot] = true' in render
assert render.index('GuideCapture::Reset(list)') < render.index('g_menuPreviewOutput.Record(')
assert 'GuideCapture::Executing(Queue, 1, lists)' in overlay
assert 'GuideCapture::Submitted(Queue, 1, lists, previewReceipt)' in overlay
cleanup = overlay[overlay.index('static void CleanupRenderTargetDx12('):overlay.index('static void RenderImGui_DX11(')]
assert cleanup.index('if (!g_overlayGpuCompletion.CanRetire())') < cleanup.index('g_menuPreviewOutput.Retire()')
assert cleanup.index('GuideCapture::Reset(g_menuFrameLists[slot])') < cleanup.index('g_menuPreviewOutput.Retire()')
assert 'Displayed.lease.reset()' in cache and 'Displayed.texture = nullptr' in cache
assert 'if (!Enabled || !device || !list || !target) return false' in cache
assert 'snapshot.configuration != Displayed.configuration' in cache
assert 'snapshot.epoch != Displayed.epoch' in cache
retire = cache[cache.index('void Retire()'):]
assert retire.index('GuideCapture::ClearDisplayed(LastSubmitted)') < retire.index('Texture.Reset()')
assert 'LastSubmitted = Displayed;' in cache
assert 'CopyResource(Texture.Get(), target)' in cache and 'CopyResource(target, Texture.Get())' in cache
fps = common[common.index('void MenuCommon::RenderPerformanceOverlay('):common.index('static void UpscalerFileChecks(')]
assert 'if (config->ShowFps.value_or_default())' in fps
assert 'const auto corner = config->FpsOverlayPosition.value_or_default()' in fps
assert 'ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f)' in fps
assert 'overlayPosition' not in fps
print('Menu preview: sealed output, exact lifetime, independent FPS, composition and placement wiring checked')

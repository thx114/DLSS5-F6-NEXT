from pathlib import Path
import sys

root = Path(__file__).resolve().parents[3]
source = (root / 'OptiScaler/wrapped/wrapped_swapchain.cpp').read_text(encoding='utf-8-sig')
signature = 'HRESULT WrappedIDXGISwapChain4::ResizeProviderOutput('
start = source.index(signature)
opening = source.index('{', start)
level = 1
end = opening + 1
while level:
    level += (source[end] == '{') - (source[end] == '}')
    end += 1
body = source[start:end]
assert 'State::' not in body and 'WaitForFinishedPicture' not in body
assert 'WithDx12' not in body and 'Mutex' not in body
for method, gate, source_work in [
    ('HookResizeBuffers(', 'return ResizeProviderOutput(', 'HoYoShade::DirectResizeScope'),
    ('ResizeBuffers1(UINT BufferCount', 'return ResizeProviderOutput(', 'HoYoShade::DirectResizeScope'),
    ('HookSetFullscreenState(', 'if (_providerOutput) return _real->SetFullscreenState', 'bool ffxLock'),
]:
    begin = source.index('HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::' + method)
    assert source.index(gate, begin) < source.index(source_work, begin)
assert 'State::Instance().isShuttingDown || (providerOutput && pDevice == nullptr)' in source
Path(sys.argv[1]).write_text(body + '\n', encoding='utf-8')

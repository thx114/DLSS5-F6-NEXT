"""Extract live queue adoption, guide sealing and GPU handback for WARP.

SDK snapshots are represented by explicit GPU copies. No NVIDIA SDK or game
is executed; the fixture does not measure display FPS or visual acceptance.
"""
from pathlib import Path
import hashlib
import importlib.util

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
spec = importlib.util.spec_from_file_location('extract_nr', ROOT / 'OptiScaler/dlssnr/tests/nr-reconfiguration/prepare.py')
parser = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parser)
paths = [ROOT / p for p in (
    'OptiScaler/framegen/FinalOutputEndpoint.h',
    'OptiScaler/framegen/FinalOutputEndpoint.cpp',
    'OptiScaler/with_dx12/with_dx12.cpp',
    'OptiScaler/with_dx12/dx11_with_dx12.cpp',
    'OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp',
)]
sources = [p.read_text(encoding='utf-8-sig') for p in paths]
take = parser.extract
adopt = take(sources[2], 'void WithDx12::AdoptSwapchainQueue(')
setter = take(sources[2], 'void WithDx12::SetD3D12Objects(')
baseline_adopt = parser.replace_once(adopt, 'framegen::final_output::CreationScope::Active() || !queue', '!queue')
baseline_adopt = parser.replace_once(baseline_adopt, 'WithDx12::AdoptSwapchainQueue(', 'WithDx12::BaselineAdopt(')
baseline_adopt = parser.replace_once(baseline_adopt, '    SetD3D12Objects(', '    BaselineSet(')
baseline_setter = parser.replace_once(setter, '    if (framegen::final_output::CreationScope::Active()) return;\n', '')
baseline_setter = parser.replace_once(baseline_setter, 'WithDx12::SetD3D12Objects(', 'WithDx12::BaselineSet(')
submit = take(sources[4], 'bool DLSSG_Dx12::SubmitUICommandList(')
submit = parser.replace_once(submit,
    '_gameCommandQueue->Signal(_uiFence,_uiAllocatorFenceValues[index])',
    'TestUiSignal(_gameCommandQueue,_uiFence,_uiAllocatorFenceValues[index])')
handback = take(sources[3], 'bool Dx11WithDx12::SyncDx12ToDx11(')
negative_handback = parser.replace_once(handback,
    'Dx11DeviceContext->Wait(Dx11FenceTextureCopy, fenceValue)', 'S_OK /* negative: omitted real DX11 wait */')
negative_handback = parser.replace_once(negative_handback, 'Dx11WithDx12::SyncDx12ToDx11(', 'Dx11WithDx12::MissingWriterWait(')
seal = parser.replace_once(take(sources[4], 'bool DLSSG_Dx12::SealDx11GuideInputs('),
    'Dx11WithDx12::SyncDx12ToDx11(_gameCommandQueue)',
    '(omitWriterWait ? Dx11WithDx12::MissingWriterWait(_gameCommandQueue) : Dx11WithDx12::SyncDx12ToDx11(_gameCommandQueue))')
header = '\n'.join('// ' + p.relative_to(ROOT).as_posix() + ' SHA256 ' + hashlib.sha256(p.read_bytes()).hexdigest() for p in paths)
out = ROOT / 'build/common-core-tests/FgHandoffProduction.inl'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(header + '\nnamespace framegen::final_output {\n' +
    take(sources[0], 'class CreationScope final') + ';\n' +
    take(sources[1], 'CreationScope::CreationScope(') + '\n' +
    take(sources[1], 'CreationScope::~CreationScope(') + '\n}\n' +
    adopt + '\n' + setter + '\n' + baseline_adopt + '\n' + baseline_setter + '\n' +
    handback + '\n' + negative_handback + '\n' + submit + '\n' +
    seal + '\n', encoding='utf-8')
print('PASS extracted actual adoption/creation scope, UI submission, guide seal and shared-fence handback')

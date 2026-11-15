"""Extract the production admission/copy body and ABI-independent binding tuple.

The fixture replaces only COM command sinks and synchronization with counters;
real queue/fence and pixel behavior is covered by Test-F6EvalReceipt.ps1.
"""
from pathlib import Path
import hashlib
import importlib.util

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
spec = importlib.util.spec_from_file_location(
    'extract', ROOT / 'OptiScaler/dlssnr/tests/nr-reconfiguration/prepare.py')
parser = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parser)
header = ROOT / 'OptiScaler/with_dx12/dx11_with_dx12.h'
source = ROOT / 'OptiScaler/with_dx12/dx11_with_dx12.cpp'
binding = parser.extract(header.read_text(encoding='utf-8-sig'), '    struct OutputCopy\n')
body = parser.extract(source.read_text(encoding='utf-8-sig'), 'bool Dx11WithDx12::CopyUpscalerOutputToDx11(')
out = ROOT / 'build/common-core-tests'
out.mkdir(parents=True, exist_ok=True)
identity = '\n'.join('// ' + p.relative_to(ROOT).as_posix() + ' SHA256 ' + hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in (header, source))
(out / 'Dx11OutputCopyBinding.inl').write_text(identity + '\n' + binding + ';\n', encoding='utf-8')
(out / 'Dx11OutputCopyProduction.inl').write_text(identity + '\n' + body + '\n', encoding='utf-8')
print('PASS extracted current DX11 output handoff binding and copy body')

"""Extract the production admission/copy body and ABI-independent binding tuple.

The fixture replaces only COM command sinks and synchronization with counters;
real queue/fence and pixel behavior is covered by Test-F6EvalReceipt.ps1.
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import re

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
spec = importlib.util.spec_from_file_location(
    'extract', ROOT / 'OptiScaler/dlssnr/tests/nr-reconfiguration/prepare.py')
parser = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parser)
header = ROOT / 'OptiScaler/with_dx12/dx11_with_dx12.h'
source = ROOT / 'OptiScaler/with_dx12/dx11_with_dx12.cpp'
bridge = ROOT / 'OptiScaler/upscalers/IFeature_Dx11wDx12.cpp'
binding = parser.extract(header.read_text(encoding='utf-8-sig'), '    struct OutputCopy\n')
body = parser.extract(source.read_text(encoding='utf-8-sig'), 'bool Dx11WithDx12::CopyUpscalerOutputToDx11(')
bridge_text = bridge.read_text(encoding='utf-8-sig')


def condition(pattern):
    match = re.search(pattern, bridge_text)
    if not match:
        raise RuntimeError('Cannot extract current bridge output admission: ' + pattern)
    return match.group(1)


delivery = condition(r'const bool canDeliver =\s*([^;]+);')
publication = condition(r'if \((evalResult && sourceOutputNr[^\n]*)\)\s*\{')
observation = condition(r'if \((canDeliver && sourceOutputNr[^\n]*)\)\s*\{')
observation = observation.replace('F6PresentationProbe::observer', 'observer')
gates = '''
namespace BridgeDelivery {
bool CanDeliver(bool parametersRestored, [[maybe_unused]] bool sourceOutputNr,
                bool orderedOutput, ID3D12Resource* recordedOutput,
                const Dx11WithDx12::OutputCopy& outputCopy) {
    return DELIVERY;
}
bool CanPublish(bool evalResult, bool sourceOutputNr,
                [[maybe_unused]] ID3D12Resource* recordedOutput,
                [[maybe_unused]] const Dx11WithDx12::OutputCopy& outputCopy) {
    return PUBLICATION;
}
bool CanObserve(bool canDeliver, bool sourceOutputNr, bool observer,
                [[maybe_unused]] ID3D12Resource* recordedOutput,
                [[maybe_unused]] const Dx11WithDx12::OutputCopy& outputCopy) {
    return OBSERVATION;
}
}
'''.replace('DELIVERY', delivery).replace('PUBLICATION', publication).replace('OBSERVATION', observation)
args = argparse.ArgumentParser(description=__doc__)
args.add_argument('--output', type=Path, default=ROOT / 'build/common-core-tests')
out = args.parse_args().output
out.mkdir(parents=True, exist_ok=True)
identity = '\n'.join('// ' + p.relative_to(ROOT).as_posix() + ' SHA256 ' + hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in (header, source, bridge))
(out / 'Dx11OutputCopyBinding.inl').write_text(identity + '\n' + binding + ';\n', encoding='utf-8')
(out / 'Dx11OutputCopyProduction.inl').write_text(identity + '\n' + body + gates, encoding='utf-8')
print('PASS extracted current DX11 output handoff, copy body and bridge delivery/publication gates')

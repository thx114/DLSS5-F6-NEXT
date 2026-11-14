"""Extract the actual critical colour-stage failure branches; no handwritten copy."""
from pathlib import Path

root = Path(__file__).resolve().parents[5]
source = (root / 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text(encoding='utf-8')
out = root / 'build/common-core-tests'
out.mkdir(parents=True, exist_ok=True)

def guard(needle):
    start = source.index(needle)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

for name, needle in [('encode', 'if (!DispatchPass(cmdList, encodeParams'),
                     ('resize', 'if (!DispatchPass(cmdList, down, modelInput')]:
    (out / f'colour-{name}.inl').write_text(guard(needle), encoding='utf-8')
start = source.index('composed = DispatchPass(cmdList, resolveParams')
end = source.index('if (composed && CalibrationActive())', start)
(out / 'colour-resolve.inl').write_text(source[start:end], encoding='utf-8')
start = source.index('const bool targetSupportsUav =')
end = source.index(';', start) + 1
# Keep the descriptor's actual owner in the fixture too. Testing an input
# descriptor here would invent a stale-output failure the product cannot take.
dispatch = source.index('void DlssNr_Dx12::Dispatch(')
target = source.index('ID3D12Resource* target = output;', dispatch)
descriptor = source.index('const D3D12_RESOURCE_DESC desc = target->GetDesc();', target)
(out / 'colour-destination.inl').write_text(
    source[target:source.index(';', target) + 1].replace('ID3D12Resource*', 'auto*') + '\n' +
    source[descriptor:source.index(';', descriptor) + 1] + '\n' + source[start:end], encoding='utf-8')
start = source.index('ID3D12Resource* resolveOriginal =')
end = source.index(';', source.index('ID3D12Resource* resolveTarget =', start)) + 1
# Only the API resource type is adapted to images; the actual binding and flag
# selection expressions are compiled unchanged by the production HLSL fixture.
(out / 'colour-bindings.inl').write_text(
    source[start:end].replace('ID3D12Resource*', 'const Image*'), encoding='utf-8')
print('Extracted current colour admission, binding and failure control flow')

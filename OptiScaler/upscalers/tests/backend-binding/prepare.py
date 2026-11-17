"""Extract the live CPU backend binding and device-scoped transport query."""
from pathlib import Path
import hashlib
import json

ROOT = Path(__file__).resolve().parents[4]
out = ROOT / 'build/common-core-tests'
out.mkdir(parents=True, exist_ok=True)
provider = ROOT / 'OptiScaler/upscalers/FeatureProvider_Dx11.cpp'
policy = ROOT / 'OptiScaler/framegen/dlssg/StarRailF5DsPolicy.h'
transport = ROOT / 'OptiScaler/with_dx12/dx11_with_dx12_sc.cpp'
p = provider.read_text(encoding='utf-8-sig')
s = policy.read_text(encoding='utf-8-sig')
t = transport.read_text(encoding='utf-8-sig')
parts = [p[p.index('namespace {\nthread_local'):p.index('\nbool FeatureProvider_Dx11::GetFeature')],
         s[s.index('namespace StarRailF5Ds'):],
         t[t.index('bool Dx11wDx12SC::StarRailF5DsTransport() const'):t.index('\nHRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present(')]]
(out / 'Dx11BackendBindingExtracted.h').write_text('\n'.join(parts), encoding='utf-8')
(out / 'Dx11BackendBindingExtraction.json').write_text(json.dumps(
    {str(x.relative_to(ROOT)): hashlib.sha256(x.read_bytes()).hexdigest() for x in (provider,policy,transport)}, indent=2), encoding='utf-8')
print('PASS extracted actual provider binding, initialization scope and device-matched source policy')

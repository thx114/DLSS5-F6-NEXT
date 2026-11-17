"""Extract private NGX ownership and the live scratch-rebuild branch."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[5]
output = Path(sys.argv[1])
output.mkdir(parents=True, exist_ok=True)
tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)


def block(text, signature):
    start = text.index(signature)
    depth = 0
    for token in tokens.finditer(text, start):
        if token[0] == '{':
            depth += 1
        elif token[0] == '}':
            depth -= 1
            if depth == 0:
                return text[start:token.end()]
    raise ValueError('Unterminated production block: ' + signature)


deferred = (root / 'OptiScaler/shaders/dlssnr/DlssNr_DeferredSr.inl').read_text(encoding='utf-8-sig')
generation = block(deferred, 'struct Generation')
if '--negative-gpu-only' in sys.argv:
    old = 'return DlssNrGpuLifetime::Complete(uses);'
    assert generation.count(old) == 1
    generation = generation.replace(old, 'return std::all_of(uses.begin(), uses.end(), [](const auto& use) { return use->Complete() || use->GpuComplete(); });')
(output / 'DeferredLifetimeProduction.inl').write_text(
    generation + ';\n' + block(deferred, 'struct Use') + ';\n', encoding='utf-8')
dispatch = (root / 'OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp').read_text(encoding='utf-8-sig')
scratch = block(dispatch, 'if (resolutionChanged || placementChanged)')
if '--negative-scratch-guard' in sys.argv:
    old = 'ParkNrResource(g_nr.output);'
    assert scratch.count(old) == 1
    scratch = scratch.replace(old, 'if (placementChanged) ' + old)
(output / 'NrScratchRebuildProduction.inl').write_text(
    'void Rebuild(bool resolutionChanged, bool placementChanged) {\n' + scratch + '\n}\n', encoding='utf-8')
mask = (root / 'OptiScaler/ui_mask/DrawUiMask.cpp').read_text(encoding='utf-8-sig')
demand = '\n'.join(block(mask, name) for name in ('bool Number(', 'bool Parse(', 'void Configure(', 'bool Enabled('))
if '--negative-mask-demand' in sys.argv:
    old = 'settings.enabled && (settings.nr || settings.filters)'
    assert demand.count(old) == 1
    demand = demand.replace(old, 'settings.enabled')
(output / 'UiMaskDemandProduction.inl').write_text(demand + '\n', encoding='utf-8')
hooks = (root / 'OptiScaler/resource_tracking/GenericDepth_Dx11.cpp').read_text(encoding='utf-8-sig')
assert 'DrawUiMaskEnabled.value_or_default()' not in hooks
assert hooks.count('DrawUiMask::Enabled()') == 2
print('Extracted actual Deferred generation/Use and NR scratch retirement:', root)

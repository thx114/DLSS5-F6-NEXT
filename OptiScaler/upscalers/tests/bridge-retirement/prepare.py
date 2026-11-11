"""Extract the actual bridge release and NR closure for real WARP tests.

Host declarations and three explicit HRESULT fault injections are seams.
The baseline control removes the new closure loop from the actual release.
"""
from pathlib import Path
import hashlib
import importlib.util

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
parser_path = ROOT / 'OptiScaler/dlssnr/tests/nr-reconfiguration/prepare.py'
spec = importlib.util.spec_from_file_location('nr_extract', parser_path)
parser = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parser)
paths = [ROOT / 'OptiScaler/upscalers/IFeature_Dx11wDx12.cpp',
         ROOT / 'OptiScaler/shaders/dlssnr/DlssNr_Late.inl']
sources = [p.read_text(encoding='utf-8-sig') for p in paths]
release = parser.extract(sources[0], 'bool IFeature_Dx11wDx12::ReleaseSharedResources(')
close = parser.extract(sources[1], 'HRESULT RetireFinishedPictureRecording(')
for old, new in [('allocator->Reset()', 'ResetAllocator(allocator)'),
                 ('cmd->Reset(allocator, nullptr)', 'ResetList(cmd, allocator)'),
                 ('cmd->Close()', 'CloseList(cmd)')]:
    close = parser.replace_once(close, old, new)
start = release.index('    // GPU completion alone leaves these executable recordings pinned by NR.')
end = release.index('    // Final Genshin teardown must release the provider while its borrowed', start)
baseline = release[:start] + release[end:]
baseline = parser.replace_once(baseline, 'IFeature_Dx11wDx12::ReleaseSharedResources(',
                              'IFeature_Dx11wDx12::BaselineRelease(')
header = '\n'.join('// ' + p.relative_to(ROOT).as_posix() + ' SHA256 ' + hashlib.sha256(p.read_bytes()).hexdigest()
                   for p in paths)
out = ROOT / 'build/common-core-tests/BridgeRetirementProduction.inl'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text('// GENERATED actual production bodies; three explicit HRESULT fault seams.\n' + header +
               '\nnamespace DlssNr {\n' + close + '\n}\n' + release + '\n' + baseline + '\n',
               encoding='utf-8', newline='\n')
print('PASS current bridge release and NR closure extracted; baseline mutation generated')

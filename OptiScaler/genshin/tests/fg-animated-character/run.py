"""Standalone shader fixtures only; no embedded generation/product build/deploy."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import subprocess

A = Path(__file__).resolve().parent
P = A.parents[2]
R = P.parents[2]
sha = lambda p: hashlib.sha256(Path(p).read_bytes()).hexdigest()
parser = argparse.ArgumentParser()
parser.add_argument('--phase', required=True)
args = parser.parse_args()
assert re.fullmatch(r'[A-Za-z0-9_-]+', args.phase), 'phase must be one directory name'
E = A / args.phase
E.mkdir(exist_ok=False)
env_script = R / 'audit/r754-native-alias-build-20261022/capture-msvc-env.cmd'
capture = subprocess.run(['cmd.exe', '/d', '/c', str(env_script)], capture_output=True)
assert capture.returncode == 0, capture.stderr.decode(errors='replace')
env = os.environ.copy()
for line in capture.stdout.decode('utf-8', errors='replace').splitlines():
    if '=' in line and not line.startswith('='):
        k, v = line.split('=', 1)
        env[k] = v
env.pop('CL', None)
env.pop('_CL_', None)
inputs = [P/'genshin/GenshinFgGameMotion_Hlsl.h', A/'R852_GenshinFgGameMotion_Hlsl.h',
          A/'fixture.cpp', Path(__file__).resolve(), env_script]
untouched = [P/'native/F5LowShaderBytecode.h', P/'genshin/GenshinFgGameMotion.h',
             P/'genshin/GenshinLegacyCapture.cpp',
             Path('D:/CODE/genshin_fsr_brigde/Dx11FsrBridge/Ffx12Backend.cpp'),
             Path('D:/CODE/genshin_fsr_brigde/Dx11FsrBridge/Dx11FsrBridge.cpp')]
before = {str(p): sha(p) for p in untouched}
preimage = json.loads((A/'preimage.json').read_text(encoding='utf-8'))
assert sha(A/'R852_GenshinFgGameMotion_Hlsl.h') == preimage['genshin\\GenshinFgGameMotion_Hlsl.h']
stages = []

def run(name, argv):
    result = subprocess.run(list(map(str, argv)), cwd=E, env=env, capture_output=True, timeout=180)
    (E/(name+'.stdout.log')).write_bytes(result.stdout)
    (E/(name+'.stderr.log')).write_bytes(result.stderr)
    stages.append(dict(name=name, argv=list(map(str, argv)), exitCode=result.returncode))
    (E/'stages.json').write_text(json.dumps(stages, indent=2), encoding='utf-8')
    print(name, result.returncode, result.stdout.decode(errors='replace')[-1400:], flush=True)
    return result

exe = E/'animated-fixture.exe'
cl = Path('D:/VS2022BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe')
compiled = run('compile', [cl, '/nologo', '/std:c++20', '/EHsc', '/W4', '/O2', '/fp:fast',
                          '/utf-8', '/MD', '/Fo'+str(E/'fixture.obj'), '/Fe'+str(exe),
                          A/'fixture.cpp', '/link', 'd3d11.lib', 'd3dcompiler.lib', 'dxgi.lib'])
assert compiled.returncode == 0
results = {}
for mode in ('hardware', 'warp'):
    for negative in (True, False):
        label = mode+('-negative-control' if negative else '-candidate')
        out = E/label
        out.mkdir()
        argv = [exe, out, mode]+(['--baseline-control'] if negative else [])
        result = run(label, argv)
        output = result.stdout.decode(errors='replace')
        counts = re.search(r'RESULT checks=(\d+) failures=(\d+) debug_errors=(\d+) debug_warnings=(\d+) static=(\d+) animated=(\d+) recovered=(\d+) same_ray=(\d+)', output)
        adapter = re.search(r'^ADAPTER .+$', output, re.M)
        results[label] = dict(exitCode=result.returncode,
            adapter=adapter.group() if adapter else None,
            counts=dict(zip(('checks','failures','debugErrors','debugWarnings','static','animated','recovered','sameRay'), map(int,counts.groups()))) if counts else None,
            artifacts={p.name:sha(p) for p in out.iterdir() if p.is_file()})
        expected = (result.returncode == 1 and counts and int(counts[2]) > 1000) if negative else (result.returncode == 0 and counts and int(counts[2]) == 0)
        results[label]['expectedOutcome'] = bool(expected and int(counts[3]) == 0 and int(counts[4]) == 0)
after = {str(p): sha(p) for p in untouched}
receipt = dict(scope='SOURCE_AND_FIXTURE_ONLY; embedded/product-build/deploy/launch NOT_RUN',
    phase=args.phase, inputs={str(p):sha(p) for p in inputs},
    protectedBefore=before, protectedAfter=after, protectedUnchanged=before==after,
    compilerSHA256=sha(cl), results=results, stages=stages)
(E/'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
assert before == after
assert all(r['expectedOutcome'] for r in results.values())

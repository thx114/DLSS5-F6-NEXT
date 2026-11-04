"""Audit-only native fixture. Never builds/deploys/launches the product."""
from pathlib import Path
import argparse, hashlib, json, os, re, subprocess, time
A=Path(__file__).resolve().parent
P=A.parents[2]
R=P.parents[2]
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
parser=argparse.ArgumentParser()
parser.add_argument('--phase',required=True)
args=parser.parse_args()
E=A/args.phase
E.mkdir(exist_ok=False)
capture=subprocess.run(['cmd.exe','/d','/c',str(R/'audit/r754-native-alias-build-20261022/capture-msvc-env.cmd')],capture_output=True,text=True,encoding='utf-8')
assert capture.returncode==0
env=os.environ.copy()
for line in capture.stdout.splitlines():
    if '=' in line and not line.startswith('='):
        k,v=line.split('=',1);env[k]=v
env.pop('CL',None);env.pop('_CL_',None)
stages=[]
def run(name,argv,required=True):
    result=subprocess.run(list(map(str,argv)),cwd=E,env=env,capture_output=True)
    (E/(name+'.stdout.log')).write_bytes(result.stdout)
    (E/(name+'.stderr.log')).write_bytes(result.stderr)
    stages.append(dict(name=name,argv=list(map(str,argv)),exitCode=result.returncode))
    (E/'stages.json').write_text(json.dumps(stages,indent=2),encoding='utf-8')
    print(name,result.returncode,result.stdout.decode(errors='replace')[-2200:],flush=True)
    if required:assert result.returncode==0
    return result
exe=E/'dynamic-fixture.exe'
run('compile',['D:/VS2022BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe','/nologo','/std:c++20','/EHsc','/W4','/O2','/fp:fast','/utf-8','/MD','/Fo'+str(E/'fixture.obj'),'/Fe'+str(exe),A/'fixture.cpp','/link','d3d11.lib','d3dcompiler.lib','dxgi.lib'])
results={}
for mode in ('hardware','warp'):
    out=E/mode;out.mkdir()
    import shutil
    shutil.copy2(A/'pre-fix/hardware/r849-embedded.dxbc',out/'r849-baseline.dxbc')
    result=run(mode,[exe,out,mode],required=False)
    results[mode]=dict(exitCode=result.returncode,stdoutSHA256=sha(E/(mode+'.stdout.log')),
        artifacts={p.name:sha(p) for p in out.iterdir() if p.is_file()})
receipt=dict(scope='FIXTURE_ONLY: no product TU/full build/deploy/launch',phase=args.phase,results=results,stages=stages,
    inputs={str(p):sha(p) for p in [P/'genshin/GenshinFgGameMotion.h',P/'genshin/GenshinFgGameMotion_Hlsl.h',P/'native/F5LowShaderBytecode.h',A/'fixture.cpp',Path(__file__).resolve(),R/'audit/r754-native-alias-build-20261022/capture-msvc-env.cmd']})
(E/'receipt.json').write_text(json.dumps(receipt,indent=2),encoding='utf-8')
if args.phase!='pre-fix':assert all(v['exitCode']==0 for v in results.values())

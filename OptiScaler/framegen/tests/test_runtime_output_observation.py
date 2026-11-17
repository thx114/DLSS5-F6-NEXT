"""Compile actual producer observation methods with SDK/context lifetime boundaries mocked."""
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[3]
HERE=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative-stale',action='store_true')
args=parser.parse_args()
def read(path): return (ROOT/path).read_text(encoding='utf-8-sig')
def function(source,signature):
    start=source.index(signature); brace=source.index('{',start); end=brace+1; depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}'); end+=1
    return source[start:end]
base=read('OptiScaler/framegen/IFGFeature_Dx12.cpp')
dlssg=read('OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp')
xe=read('OptiScaler/framegen/xefg/XeFG_Dx12.cpp')
for signature in ('bool XeFG_Dx12::CreateSwapchainContext(', 'bool XeFG_Dx12::DestroySwapchainContext('):
    assert 'std::lock_guard statusLock(_presentStatusMutex);' in function(xe,signature)
methods='\n'.join((function(base,'framegen::RuntimeSnapshot IFGFeature_Dx12::RuntimeIdentity('),
    function(base,'framegen::RuntimeSnapshot IFGFeature_Dx12::RuntimeObservation('),
    function(dlssg,'void DLSSG_Dx12::ObservePresentReturned('),
    function(xe,'void XeFG_Dx12::ObservePresentReturned(')))
if args.negative_stale:
    old='sample.queryEndNs <= snapshot.tickNs && snapshot.tickNs - sample.queryEndNs <= 3000000000ull'
    assert methods.count(old)==1
    methods=methods.replace(old,'true')
fixture=(HERE/'runtime_output_observation.cpp').read_text(encoding='utf-8')
with tempfile.TemporaryDirectory(prefix='.runtime-output-',dir=HERE) as temporary:
    directory=Path(temporary); cpp=directory/'fixture.cpp'; exe=directory/'fixture.exe'; batch=directory/'compile.cmd'
    cpp.write_text(fixture.replace('// INSERT_PRODUCTION',methods),encoding='utf-8')
    batch.write_text('@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{ROOT / "OptiScaler"}" /Fe:"{exe}" /Fo:"{directory}/" "{cpp}"\n'
        'exit /b %errorlevel%\n',encoding='utf-8',newline='\r\n')
    subprocess.run(['cmd.exe','/d','/c',str(batch)],cwd=directory,check=True)
    subprocess.run([str(exe)],cwd=directory,check=True,timeout=30)

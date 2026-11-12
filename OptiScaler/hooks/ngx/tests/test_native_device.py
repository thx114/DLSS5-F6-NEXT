"""Execute extracted real device admission/attachment methods with COM mocks."""
import argparse
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[4]
HERE=Path(__file__).resolve().parent
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative',action='store_true',help='restore raw interface pointer comparison in the real init adapter')
args=parser.parse_args()

def function(source,signature):
    start=source.index(signature)
    brace=source.index('{',start)
    depth,pos=1,brace+1
    while depth:
        depth+=(source[pos]=='{')-(source[pos]=='}')
        pos+=1
    return source[start:pos]

hook=(ROOT/'OptiScaler/hooks/ngx/NativeDx12Hooks.cpp').read_text(encoding='utf-8-sig')
inputs=(ROOT/'OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp').read_text(encoding='utf-8-sig')
helper=(ROOT/'OptiScaler/framegen/dlssg/FgDepthContract.h').read_text(encoding='utf-8-sig')
accept=function(hook,'    static bool AcceptInitDevice(')
if args.negative:
    old='dlssg::depth_contract::SameOwner(observedInitDevice, device)'
    assert accept.count(old)==1
    accept=accept.replace(old,'observedInitDevice == device')
code='namespace dlssg::depth_contract {\n'+function(helper,'inline bool SameOwner(')+'\n}\n'
code+=function(inputs,'static bool IsInitializedOnDevice(')+'\n'
assert inputs.count('if (IsInitializedOnDevice(InDevice))') == 3
code+='namespace NgxInput {\n'
for sig in ['bool CanAttachNativeDx12Input(','bool AttachNativeDx12Input(','bool DetachNativeDx12Input(']:
    code+=function(inputs,sig)+'\n'
code+='struct Api {\n'+accept+'\n'+function(hook,'    static bool AcceptShutdownDevice(')+'\n'+function(hook,'    static void ObserveInitDevice(')+'\n'+function(hook,'    static Microsoft::WRL::ComPtr<ID3D12Device> AcquireCommandDevice(')+'\n'+function(hook,'    static bool AcceptCommandDevice(')+'\n};\n}\n'
out=ROOT/'build'/('ngx-device-negative' if args.negative else 'ngx-device-tests')
out.mkdir(parents=True,exist_ok=True)
(out/'production-device.inc').write_text(code,encoding='utf-8')
exe=out/'NativeDeviceTests.exe'
batch=out/'compile.cmd'
batch.write_text('@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\nif errorlevel 1 exit /b %errorlevel%\n'+f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{out}" /Fe:"{exe}" /Fo:"{out / "NativeDeviceTests.obj"}" "{HERE / "NativeDeviceTests.cpp"}"\nexit /b %errorlevel%\n',encoding='utf-8',newline='\r\n')
result=subprocess.run(['cmd.exe','/d','/c',str(batch)],cwd=out)
if result.returncode:raise SystemExit(result.returncode)
raise SystemExit(subprocess.run([str(exe)],cwd=out,timeout=30).returncode)

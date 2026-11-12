"""Execute the real NGX caller adapter with an injected module inventory."""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative', action='store_true', help='restore the prior unknown-caller rejection')
args = parser.parse_args()

def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, pos = 1, brace + 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start:pos]

adapter = (ROOT / 'OptiScaler/hooks/ngx/NativeDx12Hooks.cpp').read_text(encoding='utf-8-sig')
helper = (ROOT / 'OptiScaler/framegen/dlssg/FgDepthContract.h').read_text(encoding='utf-8-sig')
resolver = function(adapter, 'InputOrigin ResolveInputOrigin(')
if args.negative:
    old = 'return InputOrigin::SynchronousGame;'
    assert resolver.count(old) == 1
    resolver = resolver.replace(old, 'return InputOrigin::Backend;')
generated = ('namespace dlssg::depth_contract {\n' +
             function(helper, 'template<class T> inline Microsoft::WRL::ComPtr<T> NativeObject(') + '\n}\n' +
             'namespace NgxInput {\n' +
             function(adapter, 'bool NativeDelegation(') + '\n' +
             function(adapter, 'bool NativeInitDelegation(') + '\n' +
             function(adapter, 'bool NativeStopDelegation(') + '\n' +
             function(adapter, 'enum class InputOrigin') + ';\n' + resolver + '\n' +
             function(adapter, 'bool External(') + '\n}\n')
out = ROOT / 'build' / ('ngx-caller-negative' if args.negative else 'ngx-caller-tests')
out.mkdir(parents=True, exist_ok=True)
(out / 'production-caller.inc').write_text(generated, encoding='utf-8')
dev = 'D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat'
exe = out / 'NativeCallerTests.exe'
command = (f'call "{dev}" -no_logo -arch=x64 -host_arch=x64\n'
           'if errorlevel 1 exit /b %errorlevel%\n'
           f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{out}" /I"{ROOT / "external/streamline"}" '
           f'/Fe:"{exe}" /Fo:"{out / "NativeCallerTests.obj"}" "{HERE / "NativeCallerTests.cpp"}" '
           'd3d12.lib dxgi.lib\nexit /b %errorlevel%\n')
batch = out / 'compile.cmd'
batch.write_text(command, encoding='utf-8', newline='\r\n')
compiled = subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=out)
if compiled.returncode:
    raise SystemExit(compiled.returncode)
raise SystemExit(subprocess.run([str(exe)], cwd=out, timeout=30).returncode)

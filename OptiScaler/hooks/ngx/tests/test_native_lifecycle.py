"""Compile and run the production NGX lifecycle adapters against mock vendors.

Run after the lifecycle changes land. Only extracted adapter functions and the
real router are under test; module lookup, device ownership and SDK are mocks.
The compiler and generated files live under the local build directory.
"""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
HOOK = ROOT / 'OptiScaler/hooks/ngx/NativeDx12Hooks.cpp'
VSDEV = Path('D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat')


def extract(source, signature):
    try:
        start = source.index(signature)
        opening = source.index('{', start)
        depth, pos = 1, opening + 1
        while depth:
            depth += (source[pos] == '{') - (source[pos] == '}')
            pos += 1
        return source[start:pos]
    except (ValueError, IndexError) as error:
        raise SystemExit(f'Cannot extract {signature!r} from {HOOK}: {error}') from error


source = HOOK.read_text(encoding='utf-8-sig')
signatures = [
    'struct Depth',
    'struct Replay',
    'template <class F, class... Args> NVSDK_NGX_Result NativeCall(',
    'template <class F, class... Args> NVSDK_NGX_Result NativeInitCall(',
    'template <class F, class... Args> NVSDK_NGX_Result NativeStopCall(',
    'template <class F, class... Args> NVSDK_NGX_Result SelectedCall(',
    'bool NativeDelegation(',
    'bool NativeInitDelegation(',
    'bool NativeStopDelegation(',
    'bool InternalProjectCall(',
    'bool InternalReplay(',
    'NVSDK_NGX_Result Init(',
    'NVSDK_NGX_Result InitExt(',
    'NVSDK_NGX_Result InitProject(',
    'NVSDK_NGX_Result Stop(',
    'NVSDK_NGX_Result StopDevice(',
    'NVSDK_NGX_Result Destroy(',
]
pieces = [extract(source, signature) for signature in signatures]
pieces[0] += ';'
pieces[1] += ';'
generated = '\n'.join(pieces) + '\n'

out = ROOT / 'build/ngx-lifecycle-tests'
out.mkdir(parents=True, exist_ok=True)
(out / 'production-lifecycle.inc').write_text(generated, encoding='utf-8')
exe = out / 'NativeLifecycleTests.exe'
batch = out / 'compile.cmd'
batch.write_text(
    f'@echo off\ncall "{VSDEV}" -no_logo -arch=x64 -host_arch=x64\n'
    'if errorlevel 1 exit /b %errorlevel%\n'
    f'cl /nologo /std:c++20 /EHsc /W4 /Od /I"{out}" '
    f'/Fe:"{exe}" /Fo:"{out / "NativeLifecycleTests.obj"}" '
    f'"{HERE / "NativeLifecycleTests.cpp"}"\nexit /b %errorlevel%\n',
    encoding='utf-8', newline='\r\n')
result = subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=out)
if result.returncode:
    raise SystemExit(result.returncode)
raise SystemExit(subprocess.run([str(exe)], cwd=out, timeout=30).returncode)

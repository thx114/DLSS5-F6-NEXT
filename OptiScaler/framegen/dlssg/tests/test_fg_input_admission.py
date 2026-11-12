"""Execute actual CanSubmitInputs with hook installation failure injection."""
import argparse
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--negative', action='store_true', help='bypass the source hook check for its current provider')
args = parser.parse_args()
source = (ROOT / 'OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp').read_text(encoding='utf-8-sig')
start = source.index('bool DLSSG_Dx12::CanSubmitInputs()')
brace = source.index('{', start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
body = source[start:end]
if args.negative:
    needle = 'if (state.currentFG == this && !state.externalFrameGeneration &&'
    assert body.count(needle) == 1
    body = body.replace(needle, 'if (state.currentFG != this && !state.externalFrameGeneration &&')
out = ROOT / 'build' / ('fg-input-negative' if args.negative else 'fg-input-tests')
out.mkdir(parents=True, exist_ok=True)
(out / 'production-input-admission.inc').write_text(body, encoding='utf-8')
exe = out / 'FgInputAdmissionTests.exe'
batch = out / 'compile.cmd'
batch.write_text(
    '@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\n'
    'if errorlevel 1 exit /b %errorlevel%\n'
    f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{out}" /Fe:"{exe}" '
    f'/Fo:"{out / "FgInputAdmissionTests.obj"}" "{HERE / "FgInputAdmissionTests.cpp"}"\n'
    'exit /b %errorlevel%\n', encoding='ascii', newline='\r\n')
compiled = subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=out)
if compiled.returncode:
    raise SystemExit(compiled.returncode)
raise SystemExit(subprocess.run([str(exe)], cwd=out, timeout=30).returncode)

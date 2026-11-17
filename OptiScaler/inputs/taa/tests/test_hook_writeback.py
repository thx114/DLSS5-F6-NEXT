from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[4]
source = (root / 'OptiScaler/resource_tracking/GenericDepth_Dx11.cpp').read_text(encoding='utf-8-sig')
negative = '--negative-old-order' in sys.argv[1:]
output = root / ('build/taa-hook-writeback-negative' if negative else 'build/taa-hook-writeback')
output.mkdir(parents=True, exist_ok=True)

def body(signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]', source[opening:]):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return source[start:opening + token.end()]
    raise RuntimeError(signature)

# Match definitions rather than their earlier forward declarations.
parts = []
for name in ('hkDepthMap', 'hkDepthUnmap'):
    signature = re.search(r'^(?:HRESULT|void) STDMETHODCALLTYPE ' + name + r'\([^;{]*\)\s*\{', source, re.M).group()
    parts.append(body(signature))
if negative:
    sample = '    if (taa::NeedsHooks()) taa::constants::BeforeUnmap(c,r,sub);'
    parts[1] = parts[1].replace(sample, '')
    opening = parts[1].index('{') + 1
    parts[1] = parts[1][:opening] + '\n' + sample + parts[1][opening:]
(output / 'taa_map_hooks.inc').write_text('\n'.join(parts), encoding='utf-8')
batch = output / 'compile.cmd'
batch.write_text(
    '@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\n'
    'if errorlevel 1 exit /b %errorlevel%\n'
    f'cl /nologo /utf-8 /std:c++20 /EHsc /W4 /WX /I"{root}/OptiScaler" /I"{output}" '
    f'"{Path(__file__).with_name("TaaHookWritebackTests.cpp")}" '
    f'/Fo:"{output}/fixture.obj" /Fe:"{output}/fixture.exe" /link d3d11.lib\nexit /b %errorlevel%\n', encoding='utf-8')
subprocess.run(['cmd.exe', '/d', '/c', str(batch)], check=True)
result = subprocess.run([str(output / 'fixture.exe')], text=True, capture_output=True)
print(result.stdout, end='')
print(result.stderr, end='', file=sys.stderr)
if negative:
    if result.returncode != 19 or 'completed staging writeback' not in result.stderr:
        raise RuntimeError('Old-order negative control escaped or failed for an unrelated reason')
    print('PASS old Map/Unmap staging order rejected by actual mirror/GPU contract')
else:
    result.check_returncode()

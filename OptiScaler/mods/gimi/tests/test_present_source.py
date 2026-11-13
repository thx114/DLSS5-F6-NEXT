from pathlib import Path
import re
import subprocess

root=Path(__file__).resolve().parents[4]
source=(root/'OptiScaler/hooks/D3D11_Hooks.cpp').read_text(encoding='utf-8-sig')
def body(signature, text=source):
    start=text.index(signature); opening=text.index('{',start); depth=0
    for token in re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*[\s\S]*?\*/|[{}]',text[opening:]):
        if token.group()=='{': depth+=1
        elif token.group()=='}':
            depth-=1
            if not depth: return text[start:opening+token.end()]
    raise RuntimeError(signature)
output=root/'build/genshin-reshade-20261114/gimi-present-tests'
output.mkdir(parents=True,exist_ok=True)
(output/'shader_ingress.inc').write_text(body('struct ShaderCreationScope {')+';\n'+
    body('template<size_t Index> static HRESULT hkCreatePixelShader(')+'\n'+
    body('template<size_t Index> static HRESULT hkVisibleCreateVertexShader('),encoding='utf-8')
runtime=(root/'OptiScaler/mods/gimi/GimiRuntime.cpp').read_text(encoding='utf-8-sig')
(output/'source_registry.inc').write_text('\n'.join(body(sig,runtime) for sig in (
    'bool SelectPresentSource(', 'NativePresentSourcePtr AcquirePresentSource(', 'void ForgetPresentSource(')),encoding='utf-8')
depth=(root/'OptiScaler/resource_tracking/GenericDepth_Dx11.cpp').read_text(encoding='utf-8-sig')
(output/'source_register.inc').write_text(body('IDXGISwapChain* NativeChain(',depth)+'\n'+
    body('bool RegisterModPresentSource(',depth),encoding='utf-8')
fixture=Path(__file__).with_name('present_source.cpp')
batch=output/'compile.cmd'
batch.write_text(f'@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\n'
    f'if errorlevel 1 exit /b %errorlevel%\ncl /nologo /std:c++20 /EHsc /MD /W4 /WX /I"{root}/OptiScaler" /I"{output}" '
    f'"{fixture}" /Fo:"{output}/fixture.obj" /Fe:"{output}/fixture.exe" /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib\nexit /b %errorlevel%\n',encoding='utf-8')
subprocess.run(['cmd.exe','/d','/c',str(batch)],check=True)
subprocess.run([str(output/'fixture.exe')],check=True)

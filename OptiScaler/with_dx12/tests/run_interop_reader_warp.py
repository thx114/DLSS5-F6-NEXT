"""Execute production admission against actual DX11/DX12 shared WARP work."""
from pathlib import Path
import importlib.util, subprocess, argparse
here=Path(__file__).resolve().parent
parser=argparse.ArgumentParser();parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
import sys
old=sys.argv;sys.argv=[str(here/'test_interop_copy_retirement.py')]
spec=importlib.util.spec_from_file_location('extractor',here/'test_interop_copy_retirement.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);sys.argv=old
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'InteropSlotAdmissionProduction.h').write_text(module.admission,encoding='utf-8')
legacy=module.prepare_writer.replace('Dx11wDx12SC::_PrepareSharedWriter(','Dx11wDx12SC::_PrepareSharedWriterLegacy(',1).replace('reader && FAILED(_dx11Context4->Wait(_dx11CopyReaderFence, reader))','reader && false',1)
assert legacy!=module.prepare_writer
(out/'InteropReaderWriterProduction.inl').write_text(module.prepare_writer+'\n'+module.consume+'\n'+legacy,encoding='utf-8')
exe=out/'InteropReaderWarpTests.exe';batch=out/'compile.cmd'
batch.write_text(f'@echo off\ncall "D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64\nif errorlevel 1 exit /b %errorlevel%\ncl /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /Od /I"{out}" /Fe:"{exe}" /Fo:"{out}/" "{here / "InteropReaderWarpTests.cpp"}" d3d12.lib d3d11.lib dxgi.lib\nexit /b %errorlevel%\n',encoding='utf-8',newline='\r\n')
subprocess.run(['cmd.exe','/d','/c',str(batch)],check=True)
subprocess.run([str(exe)],check=True)
negative=subprocess.run([str(exe),'--negative-skip-reader-wait'],capture_output=True,text=True)
(out/'negative.log').write_text(negative.stdout+negative.stderr,encoding='utf-8')
if negative.returncode!=1 or 'DX11 write remains behind the actual pending reader' not in negative.stderr:raise RuntimeError(f'negative control not detected: {negative}')
print('PASS missing actual GPU reader Wait negative fails at the pending-write assertion')

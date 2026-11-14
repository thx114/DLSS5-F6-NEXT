"""Compile and exercise the actual SDK query wrapper with guarded old ABIs.

python -B OptiScaler/hooks/tests/test_dlssg_state_observation.py
No graphics device, game, SDK DLL, or product build is started.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--devcmd", type=Path, default=Path("D:/VS2022BuildTools/Common7/Tools/VsDevCmd.bat"))
parser.add_argument("--negative-request-bound", action="store_true",
                    help="remove the request ABI bound in the isolated fixture; must fail")
parser.add_argument("--negative-exception-error", action="store_true",
                    help="restore the lost exception LastError in the isolated fixture; must fail")
args = parser.parse_args()
source = (ROOT / "OptiScaler/hooks/Streamline_Hooks.cpp").read_text(encoding="utf-8-sig")
begin = source.index("sl::Result StreamlineHooks::QueryDlssgState(")
end = source.index("\nbool StreamlineHooks::ReadDlssgStateSample(", begin)
production = source[begin:end]
if args.negative_request_bound:
    old = "knownLayout && requestVersion>=3 && state.structVersion>=3"
    assert production.count(old) == 1
    production = production.replace(old, "knownLayout && state.structVersion>=3")
if args.negative_exception_error:
    old = "SetLastError(sdkLastError);\n        throw;"
    assert production.count(old) == 1
    production = production.replace(old, "throw;")
fixture = (HERE / "dlssg_state_observation.cpp").read_text(encoding="utf-8")
with tempfile.TemporaryDirectory(prefix=".dlssg-observation-", dir=HERE) as temporary:
    directory = Path(temporary)
    cpp, exe, batch = (directory / name for name in ("fixture.cpp", "fixture.exe", "compile.cmd"))
    cpp.write_text(fixture.replace("// INSERT_PRODUCTION_QUERY", production), encoding="utf-8")
    batch.write_text(
        f'@echo off\ncall "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{ROOT / "OptiScaler"}" '
        f'/I"{ROOT / "external/streamline"}" /Fe:"{exe}" /Fo:"{directory}/" "{cpp}"\n'
        'exit /b %errorlevel%\n', encoding="utf-8", newline="\r\n")
    subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=directory, check=True)
    subprocess.run([str(exe)], cwd=directory, check=True, timeout=30)

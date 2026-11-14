"""Compile and exercise the actual SDK query wrapper with guarded old ABIs.

python -B OptiScaler/hooks/tests/test_dlssg_state_observation.py
The owning path uses a real WARP fence. No game, SDK DLL or product build is started.
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
parser.add_argument("--negative-owning-entry-error", action="store_true",
                    help="let old capture Release pollute SDK entry LastError; must fail")
parser.add_argument("--negative-owning-abi", action="store_true",
                    help="remove the owning preflight request bound; must fail at guarded tail")
parser.add_argument("--negative-reader-wait", action="store_true",
                    help="remove the actual producer queue Wait in an isolated header; must fail")
parser.add_argument("--negative-query-forwarding", action="store_true",
                    help="allow a query-stage Present callback to steal the input association; must fail")
parser.add_argument("--negative-input-exception-error", action="store_true",
                    help="lose the real Present exception LastError before COM cleanup; must fail")
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
if args.negative_owning_entry_error:
    old = "SetLastError(sdkLastError); // Releasing an older owning output may have changed it."
    assert production.count(old) == 1
    production = production.replace(old, "// Deliberately omit entry error restoration.")
if args.negative_owning_abi:
    old = "requestVersion>=3 && requestVersion<=4 &&"
    assert production.count(old) == 1
    production = production.replace(old, "")
fixture = (HERE / "dlssg_state_observation.cpp").read_text(encoding="utf-8")
provider = (ROOT / "OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp").read_text(encoding="utf-8-sig")
provider_header = (ROOT / "OptiScaler/framegen/dlssg/DLSSG_Dx12.h").read_text(encoding="utf-8-sig")
template_begin = provider_header.index("    template<class Present>\n    HRESULT ObserveInputPresent(")
template_end = provider_header.index("    // Called before an actual proxy-buffer rebuild", template_begin)
methods_begin = provider.index("framegen::input_present::Binding DLSSG_Dx12::CurrentInputPresentBinding(")
methods_end = provider.index("\nvoid DLSSG_Dx12::RecordInputPresentObservation(", methods_begin)
rebuild_begin = provider.index("bool DLSSG_Dx12::BeginInputBufferRebuild(", methods_end)
rebuild_end = provider.index("\nsl::Result DLSSG_Dx12::QueryProviderState(", rebuild_begin)
provider_template = provider_header[template_begin:template_end]
if args.negative_input_exception_error:
    old = "catch (...) {boundary.error=GetLastError();throw;}"
    assert provider_template.count(old) == 1
    provider_template = provider_template.replace(old, "catch (...) {throw;}")
provider_fixture = (HERE / "dlssg_input_present.inl").read_text(encoding="utf-8")
provider_fixture = provider_fixture.replace("// INSERT_PRODUCTION_INPUT_TEMPLATE", provider_template)
provider_fixture = provider_fixture.replace("// INSERT_PRODUCTION_INPUT_METHODS",
    provider[methods_begin:methods_end] + provider[rebuild_begin:rebuild_end])
fixture = fixture.replace("// INSERT_PRODUCTION_INPUT_FIXTURE", provider_fixture)
with tempfile.TemporaryDirectory(prefix=".dlssg-observation-", dir=HERE) as temporary:
    directory = Path(temporary)
    if args.negative_reader_wait or args.negative_query_forwarding:
        header = (ROOT / "OptiScaler/framegen/Dx12InputPresent.h").read_text(encoding="utf-8-sig")
        if args.negative_reader_wait:
            old = "return producer->Wait(fence_.Get(),fence_.Value());"
            assert header.count(old) == 1
            header = header.replace(old, "return S_OK;")
        for name in ("DlssgInputPresent.h", "DlssgStateSample.h", "Dx12InputFence.h"):
            header = header.replace(f'#include "{name}"', f'#include <framegen/{name}>')
        (directory / "framegen").mkdir()
        (directory / "framegen/Dx12InputPresent.h").write_text(header, encoding="utf-8")
        if args.negative_query_forwarding:
            forwarding = (ROOT / "OptiScaler/framegen/DlssgInputPresent.h").read_text(encoding="utf-8-sig")
            old = "!entry->presenting_ || entry->api_==api"
            assert forwarding.count(old) == 1
            forwarding = forwarding.replace(old, "entry->api_==api")
            (directory / "framegen/DlssgInputPresent.h").write_text(forwarding, encoding="utf-8")
    cpp, exe, batch = (directory / name for name in ("fixture.cpp", "fixture.exe", "compile.cmd"))
    cpp.write_text(fixture.replace("// INSERT_PRODUCTION_QUERY", production), encoding="utf-8")
    batch.write_text(
        f'@echo off\ncall "{args.devcmd}" -no_logo -arch=x64 -host_arch=x64\n'
        'if errorlevel 1 exit /b %errorlevel%\n'
        f'cl /nologo /std:c++20 /EHsc /W4 /WX /Od /I"{directory}" /I"{ROOT / "OptiScaler"}" '
        f'/I"{ROOT / "external/streamline"}" /Fe:"{exe}" /Fo:"{directory}/" "{cpp}" /link d3d12.lib dxgi.lib\n'
        'exit /b %errorlevel%\n', encoding="utf-8", newline="\r\n")
    subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=directory, check=True)
    subprocess.run([str(exe)], cwd=directory, check=True, timeout=30)
